#include "PowerTelemetry.h"

#include <Arduino.h>

PowerTelemetry& PowerTelemetry::getInstance() {
  static PowerTelemetry instance;
  return instance;
}

void PowerTelemetry::accrueClock() const {
  const uint32_t now = millis();
  if (!clockStarted_) {
    clockSinceMs_ = now;
    return;
  }
  // Unsigned subtraction, so the 49-day millis() wrap costs one interval, not a
  // negative one.
  const uint32_t elapsed = now - clockSinceMs_;
  if (throttled_) {
    throttledMs_ += elapsed;
  } else {
    fullClockMs_ += elapsed;
  }
  clockSinceMs_ = now;
}

void PowerTelemetry::onRefreshUs(Refresh kind, uint32_t busyUs, WindowSite site) {
  switch (kind) {
    case Refresh::Full:
      ++refreshFull_;
      break;
    case Refresh::Half:
      ++refreshHalf_;
      break;
    case Refresh::Fast:
      ++refreshFast_;
      break;
    case Refresh::Window:
      ++refreshWindow_;
      // Clamped rather than trusted: the enum crosses a library boundary
      // (GfxRenderer hands it down from an activity), and an out-of-range value
      // would write past the array instead of merely mislabelling a refresh.
      if (site < WindowSite::Count) ++windowSite_[static_cast<uint8_t>(site)];
      break;
  }
  busyUs_[static_cast<uint8_t>(kind)] += busyUs;
  // panelBusyMs stays the sum it always was, so every script written against
  // the existing column keeps reading the same number.
  panelBusyMs_ += busyUs / 1000;
}

void PowerTelemetry::onPanelWaitUs(uint32_t busyUs) { panelBusyMs_ += busyUs / 1000; }

void PowerTelemetry::onAsyncCompletedInline(uint32_t busyUs) {
  ++asyncCompletedInline_;
  // Billed to Fast, because that is the waveform that ran. It used to be
  // billed at 0 ms on the theory that the wait would arrive later from
  // waitRefreshComplete() -- but on a driver that never defers, the wait
  // returns immediately and the time was simply lost from the log.
  busyUs_[static_cast<uint8_t>(Refresh::Fast)] += busyUs;
  panelBusyMs_ += busyUs / 1000;
}

void PowerTelemetry::onLoop(uint32_t busyMs) {
  ++loopIters_;
  loopBusyMs_ += busyMs;
  if (busyMs > loopMaxMs_) loopMaxMs_ = busyMs;
}

void PowerTelemetry::onCpuFrequency(uint16_t mhz, bool throttled) {
  accrueClock();
  clockStarted_ = true;
  cpuMhz_ = mhz;
  throttled_ = throttled;
}

PowerTelemetry::Snapshot PowerTelemetry::snapshot() const {
  accrueClock();

  Snapshot s;
  s.uptimeS = millis() / 1000;
  s.refreshFull = refreshFull_;
  s.refreshHalf = refreshHalf_;
  s.refreshFast = refreshFast_;
  s.refreshWindow = refreshWindow_;
  s.panelBusyMs = panelBusyMs_;
  s.busyFullMs = static_cast<uint32_t>(busyUs_[static_cast<uint8_t>(Refresh::Full)] / 1000);
  s.busyHalfMs = static_cast<uint32_t>(busyUs_[static_cast<uint8_t>(Refresh::Half)] / 1000);
  s.busyFastMs = static_cast<uint32_t>(busyUs_[static_cast<uint8_t>(Refresh::Fast)] / 1000);
  s.busyWindowMs = static_cast<uint32_t>(busyUs_[static_cast<uint8_t>(Refresh::Window)] / 1000);
  for (uint8_t i = 0; i < static_cast<uint8_t>(WindowSite::Count); ++i) s.windowBySite[i] = windowSite_[i];
  s.asyncCompletedInline = asyncCompletedInline_;
  s.loopIters = loopIters_;
  s.loopBusyMs = loopBusyMs_;
  s.loopMaxMs = loopMaxMs_;
  s.fullClockMs = fullClockMs_;
  s.throttledMs = throttledMs_;
  s.cpuMhz = cpuMhz_;
  return s;
}

void PowerTelemetry::reset() {
  refreshFull_ = 0;
  refreshHalf_ = 0;
  refreshFast_ = 0;
  refreshWindow_ = 0;
  panelBusyMs_ = 0;
  for (auto& us : busyUs_) us = 0;
  for (auto& n : windowSite_) n = 0;
  asyncCompletedInline_ = 0;
  loopIters_ = 0;
  loopBusyMs_ = 0;
  loopMaxMs_ = 0;
  fullClockMs_ = 0;
  throttledMs_ = 0;
  // Not clockStarted_/cpuMhz_/throttled_: the CPU is on a clock right now, and
  // forgetting which one would misattribute every millisecond until the next
  // frequency change.
  clockSinceMs_ = millis();
}
