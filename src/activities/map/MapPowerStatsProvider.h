#pragma once

#include <BlePositionServer.h>
#include <HalPowerManager.h>
#include <PowerTelemetry.h>

#include "MapCommandConsole.h"

// Fills the `stats` command's reply from the device's own counters.
//
// Shared by every screen that owns a console -- the map and the tile sync
// screen -- because the numbers are the device's, not the screen's, and two
// copies of this would drift the moment a field is added. The console half
// itself must stay free of Arduino (it is host-tested), which is why this is a
// separate header rather than a method on MapConsoleState.
//
// Always succeeds: the counters exist from boot and a board with no battery
// backend reports 0 mV, which `stats` prints honestly rather than hiding.
inline bool fillMapPowerStats(MapPowerStats& out) {
  const PowerTelemetry::Snapshot s = POWER_TELEMETRY.snapshot();
  out.batteryMv = powerManager.getBatteryMillivolts();
  out.batteryPct = powerManager.getBatteryPercentage();
  out.uptimeS = s.uptimeS;
  out.cpuMhz = s.cpuMhz;
  out.fullClockMs = s.fullClockMs;
  out.throttledMs = s.throttledMs;
  out.loopIters = s.loopIters;
  out.loopBusyMs = s.loopBusyMs;
  out.loopMaxMs = s.loopMaxMs;
  out.refreshFull = s.refreshFull;
  out.refreshHalf = s.refreshHalf;
  out.refreshFast = s.refreshFast;
  out.refreshWindow = s.refreshWindow;
  out.panelBusyMs = s.panelBusyMs;
  out.freeHeap = static_cast<uint32_t>(ESP.getFreeHeap());
  out.minFreeHeap = static_cast<uint32_t>(ESP.getMinFreeHeap());
  out.rssiDbm = freeink::BlePositionServer::getInstance().rssi();
  return true;
}
