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
  // First contact's coordinates, 0xFFFF when the frame reported none. Added
  // 2026-09-14: the status byte alone cannot tell a finger on the capacitive
  // home key from a finger on the glass, and a finger believed to be on the key
  // often reports as an ordinary glass contact instead -- the pad is small and
  // easy to miss. Without the position there is no way to say which one a
  // capture is looking at.
  //
  // Corrected the same day: this said "a held key reports as an ordinary
  // contact with the key bit clear", which the captures refute -- a held key
  // reports NOTHING (docs/measurements/2026-09-14-gt911-x4pro, cap8: eight
  // seconds of hold, not one frame). The contacts came from a finger on the
  // glass beside the pad.
  uint16_t x;
  uint16_t y;
};

// 2,000 samples is 12 kB, which the C3 can spare transiently and which covers
// the default capture (3 s at 5 ms = 600) six times over. A capture that would
// exceed it is refused rather than silently truncated: a log that stops early
// without saying so reads as "nothing happened after this point", which is the
// exact false negative this instrument exists to avoid.
constexpr uint32_t kMaxSamples = 2000;

constexpr uint16_t kPointReg = 0x8150;
constexpr uint16_t kNoCoord = 0xFFFF;

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

// Wait for room in the output before writing a row.
//
// The dump loses lines without this, and pacing on a fixed row count does not
// fix it: measured on the X4 Pro 2026-09-14, one capture announced 1,601 samples
// and 1,014 arrived, and after a flush-plus-delay every 64 rows a second one
// still lost 95 rows of 1,566. The cause is this fork's own
// `logSerial.setTxTimeoutMs(1)` (load-bearing, src/main.cpp `setup()`): a single
// write has almost no budget to ride out a host stall. `CMD:SCREENSHOT` hit the
// same wall and answered it with `writeAllChunked()` -- retry against
// `availableForWrite()` with a budget of its own. Same answer here.
//
// Bounded, because a host that has stopped reading must not wedge the device:
// after the budget the row is written anyway and the row count in TOUCHLOG_END
// lets the host see what it missed.
void waitForRoom(Print& out, const size_t need) {
  const unsigned long deadline = millis() + 200;
  while (out.availableForWrite() < static_cast<int>(need)) {
    if (static_cast<long>(millis() - deadline) >= 0) return;
    delay(1);
  }
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
    }

    uint16_t x = kNoCoord;
    uint16_t y = kNoCoord;
    if (status != 0xFF && (status & 0x80) != 0 && (status & 0x0F) != 0) {
      // One contact record is enough: the question this answers is where a
      // single finger is, and reading all five would cost four more I2C
      // transactions inside a 5 ms slot.
      uint8_t point[8] = {};
      if (readReg(addr, kPointReg, point, sizeof(point))) {
        // Byte layout differs per panel; this board reports coords at byte 0
        // with no track id (BoardConfig gt911CoordsAtByte0 = true), and these
        // are raw controller coordinates, not panel-oriented ones.
        const uint8_t base = BoardConfig::ACTIVE.touch.gt911CoordsAtByte0 ? 0 : 1;
        x = static_cast<uint16_t>(point[base] | (point[base + 1] << 8));
        y = static_cast<uint16_t>(point[base + 2] | (point[base + 3] << 8));
      }
    }
    // Cleared after the point read, never before: clearing the status is what
    // releases the frame, and the coordinates belong to the frame being
    // acknowledged.
    if (clearAfterRead && status != 0xFF && (status & 0x80) != 0) clearStatus(addr);

    samples[taken].us = sampleUs - startUs;
    samples[taken].status = status;
    samples[taken].intLevel = irqPin >= 0 ? static_cast<uint8_t>(digitalRead(irqPin)) : 2;
    samples[taken].x = x;
    samples[taken].y = y;
    ++taken;

    if (sampleUs - startUs >= durationMs * 1000UL) break;
  }

  // Run-length encode on the (status, INT) pair. Printing every sample would
  // send 2,000 lines for a three-second capture and drown the one transition
  // that matters; printing only changes would hide how long each state held.
  // Both, then: the state, when it started, and how many samples it survived.
  out.printf("TOUCHLOG_DATA:us,status,int,repeats,x,y\n");
  uint32_t runStart = 0;
  uint32_t runCount = 1;
  uint32_t rows = 0;
  for (uint32_t i = 1; i <= taken; ++i) {
    const bool same = i < taken && samples[i].status == samples[runStart].status &&
                      samples[i].intLevel == samples[runStart].intLevel;
    if (same) {
      ++runCount;
      continue;
    }
    waitForRoom(out, 48);
    out.printf("%u,0x%02X,%u,%u,%d,%d\n", static_cast<unsigned>(samples[runStart].us),
               static_cast<unsigned>(samples[runStart].status), static_cast<unsigned>(samples[runStart].intLevel),
               static_cast<unsigned>(runCount),
               samples[runStart].x == kNoCoord ? -1 : static_cast<int>(samples[runStart].x),
               samples[runStart].y == kNoCoord ? -1 : static_cast<int>(samples[runStart].y));
    ++rows;
    runStart = i;
    runCount = 1;
  }

  const uint32_t spanUs = taken > 0 ? samples[taken - 1].us : 0;
  // `rows` is what makes a lossy capture self-reporting: the host counts the
  // data lines it actually received and compares. Without it, a capture that
  // lost half its lines still ends with a confident-looking END.
  out.printf("TOUCHLOG_END:samples=%u,rows=%u,failed=%u,span_us=%u\n", static_cast<unsigned>(taken),
             static_cast<unsigned>(rows), static_cast<unsigned>(failed), static_cast<unsigned>(spanUs));
  free(samples);
  return true;
}

namespace {

// Upper edge of each bucket, in microseconds. Chosen against what the numbers
// have to distinguish, not for round decades: 10 ms is the GT911's own frame
// period, so a gap under it can never miss a frame; 100 ms is roughly a fast UI
// iteration; and everything from 500 ms up is panel-refresh territory, where a
// gesture is lost outright.
constexpr uint32_t kGapBucketUs[] = {2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000};
constexpr uint8_t kGapBuckets = sizeof(kGapBucketUs) / sizeof(kGapBucketUs[0]) + 1;
constexpr uint8_t kWorstGaps = 10;

uint32_t gapCount[kGapBuckets] = {};
uint32_t worstUs[kWorstGaps] = {};
uint32_t worstAtMs[kWorstGaps] = {};
uint32_t gapTotal = 0;
uint32_t lastUpdateUs = 0;

}  // namespace

void noteUpdate() {
  const uint32_t now = micros();
  const uint32_t previous = lastUpdateUs;
  lastUpdateUs = now;
  // The first call after boot or after a report has no predecessor, and a
  // made-up gap there would be the largest number in the table.
  if (previous == 0) return;

  const uint32_t gap = now - previous;
  ++gapTotal;
  uint8_t bucket = kGapBuckets - 1;
  for (uint8_t i = 0; i < kGapBuckets - 1; ++i) {
    if (gap <= kGapBucketUs[i]) {
      bucket = i;
      break;
    }
  }
  ++gapCount[bucket];

  // Insertion into a ten-slot descending list. Linear, and that is fine: it only
  // runs for a gap that beats the current tenth, which after the first second is
  // almost never.
  if (gap <= worstUs[kWorstGaps - 1]) return;
  uint8_t at = kWorstGaps - 1;
  while (at > 0 && worstUs[at - 1] < gap) {
    worstUs[at] = worstUs[at - 1];
    worstAtMs[at] = worstAtMs[at - 1];
    --at;
  }
  worstUs[at] = gap;
  worstAtMs[at] = millis();
}

void reportGaps(Print& out) {
  out.printf("LOOPGAP_BEGIN:samples=%u\n", static_cast<unsigned>(gapTotal));
  uint32_t lower = 0;
  for (uint8_t i = 0; i < kGapBuckets; ++i) {
    const uint32_t upper = i < kGapBuckets - 1 ? kGapBucketUs[i] : 0;
    if (upper == 0) {
      out.printf("%u..inf,%u\n", static_cast<unsigned>(lower), static_cast<unsigned>(gapCount[i]));
    } else {
      out.printf("%u..%u,%u\n", static_cast<unsigned>(lower), static_cast<unsigned>(upper),
                 static_cast<unsigned>(gapCount[i]));
      lower = upper;
    }
  }
  out.printf("LOOPGAP_WORST:us,at_ms\n");
  for (uint8_t i = 0; i < kWorstGaps; ++i) {
    if (worstUs[i] == 0) break;
    out.printf("%u,%u\n", static_cast<unsigned>(worstUs[i]), static_cast<unsigned>(worstAtMs[i]));
  }
  out.printf("LOOPGAP_END\n");

  for (uint8_t i = 0; i < kGapBuckets; ++i) gapCount[i] = 0;
  for (uint8_t i = 0; i < kWorstGaps; ++i) {
    worstUs[i] = 0;
    worstAtMs[i] = 0;
  }
  gapTotal = 0;
  lastUpdateUs = 0;
}

}  // namespace DebugTouchLog

#endif  // ENABLE_TOUCHLOG_CMD
