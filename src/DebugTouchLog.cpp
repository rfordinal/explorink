#include "DebugTouchLog.h"

#ifdef ENABLE_TOUCHLOG_CMD

#include <Arduino.h>
#include <BoardConfig.h>
#include <Wire.h>

#include <cstdlib>

namespace DebugTouchLog {
namespace {

constexpr uint16_t kStatusReg = 0x814E;

// Ceilings, not tuning. Eight seconds is long enough for the slowest thing being
// measured (a windowed panel refresh costs about 1,081 ms, docs/map-follow.md)
// with room to tap during it, and short enough that a stuck capture is over
// before anyone reaches for the cable. 1 ms is below the controller's own
// reporting period at any configured refresh rate, so nothing is gained by
// allowing less.
constexpr uint32_t kMaxDurationMs = 8000;
constexpr uint32_t kMinIntervalUs = 1000;

// One sample. Packed because the count is what decides whether this fits: at the
// 1 ms floor an eight-second capture is 8,000 of them, and 48 kB of heap on a C3
// is not available. At 6 bytes it is 48 kB too -- so the sample cap below, not
// this struct, is what keeps it affordable.
struct __attribute__((packed)) Sample {
  uint32_t us;
  uint8_t status;
  uint8_t intLevel;
};

// 2,000 samples is 12 kB, which the C3 can spare transiently and which covers
// the default capture (3 s at 5 ms = 600) six times over. A capture that would
// exceed it is refused rather than silently truncated: a log that stops early
// without saying so reads as "nothing happened after this point", which is the
// exact false negative this instrument exists to avoid.
constexpr uint32_t kMaxSamples = 2000;

// Probe the two addresses a GT911 can answer on. The SDK resolves this at boot
// and keeps the answer private, so it is resolved again here rather than
// widening the SDK's surface for a bench command.
uint8_t resolveAddress() {
  const BoardConfig::TouchConfig& t = BoardConfig::ACTIVE.touch;
  const uint8_t candidates[2] = {t.i2cAddress, t.i2cAddressAlt};
  for (const uint8_t addr : candidates) {
    if (addr == 0) continue;
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return addr;
  }
  return 0;
}

// Same access the SDK uses (InputManager::gt911ReadReg): 16-bit register
// address, repeated start, then the read.
bool readReg(const uint8_t addr, const uint16_t reg, uint8_t* buf, const uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  const uint8_t got = Wire.requestFrom(addr, len, static_cast<uint8_t>(true));
  if (got != len) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
  return true;
}

// Acknowledge a frame the way mainline Linux does after processing a report:
// `goodix_ts_irq_handler()` writes 0 to the coord register once it is done.
void clearStatus(const uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(static_cast<uint8_t>(kStatusReg >> 8));
  Wire.write(static_cast<uint8_t>(kStatusReg & 0xFF));
  Wire.write(static_cast<uint8_t>(0x00));
  Wire.endTransmission();
}

}  // namespace

bool capture(Print& out, uint32_t durationMs, uint32_t intervalUs, const bool clearAfterRead) {
  if (durationMs == 0 || durationMs > kMaxDurationMs) durationMs = 3000;
  if (intervalUs < kMinIntervalUs) intervalUs = kMinIntervalUs;

  const uint32_t wanted = (durationMs * 1000UL) / intervalUs + 1;
  if (wanted > kMaxSamples) {
    out.printf("TOUCHLOG_ERR:too_many_samples:%u>%u\n", static_cast<unsigned>(wanted),
               static_cast<unsigned>(kMaxSamples));
    return false;
  }

  const uint8_t addr = resolveAddress();
  if (addr == 0) {
    out.printf("TOUCHLOG_ERR:no_controller\n");
    return false;
  }

  const int8_t irqPin = BoardConfig::ACTIVE.touch.irq;
  if (irqPin >= 0) {
    // The SDK drives this line during the GT911's address-select reset dance and
    // then leaves it alone, because nothing here reads it. Put it back to an
    // input so the controller's own assertion is visible. Left as an input
    // afterwards on purpose: that is what the datasheet asks of the host, and no
    // code path in this firmware or the SDK reads or drives it again.
    pinMode(irqPin, INPUT);
  }

  Sample* samples = static_cast<Sample*>(malloc(sizeof(Sample) * wanted));
  if (samples == nullptr) {
    out.printf("TOUCHLOG_ERR:no_heap:%u\n", static_cast<unsigned>(sizeof(Sample) * wanted));
    return false;
  }

  out.printf("TOUCHLOG_BEGIN:%ums,%uus,%s,addr=0x%02X,irq=%d\n", static_cast<unsigned>(durationMs),
             static_cast<unsigned>(intervalUs), clearAfterRead ? "clear" : "noclear", addr, static_cast<int>(irqPin));
  // Flushed before the capture starts: the host must see the marker even if the
  // capture then wedges on the bus, and the write itself must not land inside
  // the timed section.
  out.flush();

  const uint32_t startUs = micros();
  uint32_t taken = 0;
  uint32_t failed = 0;
  uint32_t nextDue = startUs;

  while (taken < wanted) {
    // Busy-wait rather than delayMicroseconds(): the loop is blocked for the
    // whole capture anyway, and a spin keeps the cadence honest when a read
    // overruns its slot.
    while (static_cast<int32_t>(micros() - nextDue) < 0) {
    }
    nextDue += intervalUs;

    const uint32_t sampleUs = micros();
    uint8_t status = 0;
    if (!readReg(addr, kStatusReg, &status, 1)) {
      ++failed;
      // Recorded as 0xFF rather than dropped. A silent gap in the log is
      // indistinguishable from a quiet controller, and telling those two apart
      // is half of what the capture is for.
      status = 0xFF;
    } else if (clearAfterRead && (status & 0x80) != 0) {
      clearStatus(addr);
    }

    samples[taken].us = sampleUs - startUs;
    samples[taken].status = status;
    samples[taken].intLevel = irqPin >= 0 ? static_cast<uint8_t>(digitalRead(irqPin)) : 2;
    ++taken;

    if (sampleUs - startUs >= durationMs * 1000UL) break;
  }

  // Run-length encode on the (status, INT) pair. Printing every sample would
  // send 2,000 lines for a three-second capture and drown the one transition
  // that matters; printing only changes would hide how long each state held.
  // Both, then: the state, when it started, and how many samples it survived.
  out.printf("TOUCHLOG_DATA:us,status,int,repeats\n");
  uint32_t runStart = 0;
  uint32_t runCount = 1;
  for (uint32_t i = 1; i <= taken; ++i) {
    const bool same = i < taken && samples[i].status == samples[runStart].status &&
                      samples[i].intLevel == samples[runStart].intLevel;
    if (same) {
      ++runCount;
      continue;
    }
    out.printf("%u,0x%02X,%u,%u\n", static_cast<unsigned>(samples[runStart].us),
               static_cast<unsigned>(samples[runStart].status), static_cast<unsigned>(samples[runStart].intLevel),
               static_cast<unsigned>(runCount));
    runStart = i;
    runCount = 1;
  }

  const uint32_t spanUs = taken > 0 ? samples[taken - 1].us : 0;
  out.printf("TOUCHLOG_END:samples=%u,failed=%u,span_us=%u\n", static_cast<unsigned>(taken),
             static_cast<unsigned>(failed), static_cast<unsigned>(spanUs));
  free(samples);
  return true;
}

}  // namespace DebugTouchLog

#endif  // ENABLE_TOUCHLOG_CMD
