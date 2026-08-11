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

void PowerTelemetry::onRefresh(Refresh kind, uint32_t busyMs) {
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
      break;
  }
  panelBusyMs_ += busyMs;
}

void PowerTelemetry::onPanelWait(uint32_t busyMs) { panelBusyMs_ += busyMs; }

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
