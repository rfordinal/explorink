#include "PowerLog.h"

#include <Arduino.h>
#include <BlePositionServer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PowerTelemetry.h>

namespace {

constexpr const char* kLogTag = "PWRLOG";

// The column order is the file format. Appending a column is fine; reordering
// one silently breaks every earlier row a script reads alongside it.
//
// `build` is TRAILINK_VERSION, repeated on every row. It costs ~38 bytes a row
// against ~80, which is nothing on an SD card, and it is the only thing that
// makes two runs comparable: the device appends across boots, so one file holds
// rows from several firmwares and nothing else in the row says which. Measured
// 2026-08-15 -- run 1's file has 61 boots in it and no way to tell them apart
// by build (docs/power-plan.md, run 1).
constexpr const char* kHeader =
    "uptime_s,batt_mv,batt_pct,cpu_mhz,full_clock_ms,throttled_ms,loops,loop_busy_ms,loop_max_ms,"
    "ref_full,ref_half,ref_fast,ref_window,panel_busy_ms,heap,min_heap,ble,build\n";

// 0 = BLE stack down, 1 = advertising with nobody connected, 2 = a central is
// connected.
//
// 0 used to mean "any screen but the map" and no longer does: since 2026-09-04
// a map session with the receiver as its position source runs no BLE at all
// (MapActivity's bleInUse_), so 0 covers that too. A row's screen is not
// recoverable from this column alone. Three states rather than a bool
// because they are three different radio duty cycles, and telling them apart is
// most of what a power log is for.
uint8_t bleState() {
  const auto& ble = freeink::BlePositionServer::getInstance();
  if (!ble.isRunning()) return 0;
  return ble.connIntervalMs() > 0 ? 2 : 1;
}

}  // namespace

void PowerLog::tick() {
  static uint32_t nextDueMs = 0;  // 0 = write the first row on the first call
  static bool disabled = false;
  static bool headerWrittenThisBoot = false;

  if (disabled) return;

  const uint32_t now = millis();
  if (nextDueMs != 0 && static_cast<int32_t>(now - nextDueMs) < 0) return;
  if (!Storage.ready()) return;  // no card yet; try again next loop, do not disable

  nextDueMs = now + kIntervalMs;

  const bool fresh = !Storage.exists(kPath);
  // A card that has never had tiles pushed to it has no /trailink yet, and
  // O_CREAT does not create the parent.
  if (fresh) Storage.ensureDirectoryExists(kDir);
  HalFile file = Storage.open(kPath, O_WRITE | O_CREAT | O_APPEND);
  if (!file.isOpen()) {
    // One line, then stop trying: a card that refuses this file will refuse it
    // every minute for the rest of the ride, and the log spam would be worse
    // than the missing measurement.
    LOG_ERR(kLogTag, "cannot open %s -- power logging off for this boot", kPath);
    disabled = true;
    return;
  }
  // Once per boot, not only on a fresh file. Two reasons, both learned from
  // reading a real file (docs/power-plan.md, run 1):
  //  - it makes each boot's rows self-describing, so a file that spans a column
  //    change stays readable instead of silently mis-parsing the newer rows;
  //  - it is the boot marker the analysis wanted anyway, so a run can be found
  //    without matching timestamps by hand.
  // A reader skips any line starting with "uptime_s".
  if (fresh || !headerWrittenThisBoot) {
    file.print(kHeader);
    headerWrittenThisBoot = true;
  }

  const PowerTelemetry::Snapshot s = POWER_TELEMETRY.snapshot();
  // printf into the file rather than building a String: CLAUDE.md's string
  // policy, and HalFile is a Print so this costs no intermediate buffer of
  // ours.
  // TRAILINK_VERSION goes through %s, never concatenated into the format
  // string: it carries a branch name, and a '%' in one would make printf read
  // an argument that was never passed.
  file.printf("%lu,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u,%s\n",
              static_cast<unsigned long>(s.uptimeS), static_cast<unsigned>(powerManager.getBatteryMillivolts()),
              static_cast<unsigned>(powerManager.getBatteryPercentage()), static_cast<unsigned>(s.cpuMhz),
              static_cast<unsigned long>(s.fullClockMs), static_cast<unsigned long>(s.throttledMs),
              static_cast<unsigned long>(s.loopIters), static_cast<unsigned long>(s.loopBusyMs),
              static_cast<unsigned long>(s.loopMaxMs), static_cast<unsigned long>(s.refreshFull),
              static_cast<unsigned long>(s.refreshHalf), static_cast<unsigned long>(s.refreshFast),
              static_cast<unsigned long>(s.refreshWindow), static_cast<unsigned long>(s.panelBusyMs),
              static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMinFreeHeap()),
              static_cast<unsigned>(bleState()), TRAILINK_VERSION);

  // Explicit: the row must be on the card before the next one is due, and this
  // file is written to across a whole ride that may end with a flat battery
  // rather than a clean shutdown.
  file.flush();
}
