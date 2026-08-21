#pragma once

#include <cstdint>

// One CSV row per minute onto the SD card, for as long as the device is awake.
//
// Why a file and not just the `stats` console command: the interesting
// baseline is the device on battery with **no phone connected**, and that state
// has no channel to answer a question on -- USB needs VBUS (which also charges,
// so the battery reading stops meaning anything) and BLE needs a central. A
// file survives the disconnect, survives the ride, and survives the device
// running flat, which is the measurement that ends every campaign.
//
// Counters are cumulative since boot, not per-interval deltas. Differencing two
// rows is trivial in analysis and cannot lose an event; a delta written by the
// device would lose whatever happened in the interval a row failed to write.
//
// The header line is written once per boot, not once per file. It doubles as
// the boot marker -- a reader splits runs on it instead of matching timestamps
// by hand -- and it keeps a file readable across a column change, because each
// boot's rows carry their own column list. Skip any line starting with
// "uptime_s".
//
// Every row carries `build` (TRAILINK_VERSION). The device appends across
// boots, so one file holds rows from several firmwares; without the column
// nothing says which wrote what, and two runs cannot be compared. That gap was
// found the hard way reading run 1 (docs/power-plan.md).
//
// Cost: one open/append/close of ~160 bytes every 60 s. At sixty rows an hour
// the file grows about 10 KB per hour of riding, so nothing rotates it.
class PowerLog {
 public:
  static constexpr uint32_t kIntervalMs = 60000;

  // Call once per main-loop iteration. Writes at most one row per kIntervalMs
  // and returns immediately the rest of the time. Silent no-op until the SD
  // card is mounted.
  static void tick();

  // Name the run this row belongs to. Written into every row's `state` column
  // so an analysis groups by it instead of by a wall-clock note kept somewhere
  // else. Truncated to 15 characters; a comma or a newline becomes '_', because
  // either would shift every later column of that row.
  //
  // Nobody has to call it: the column reads "-" until somebody does, and every
  // run before this column existed is grouped by `ble` and `build` alone.
  static void setState(const char* label);

  // Where the rows go. Under the same on-device root as the tiles; the path
  // still says trailink because the card's directory has not been renamed
  // (parent CLAUDE.md).
  static constexpr const char* kPath = "/trailink/power.csv";
  static constexpr const char* kDir = "/trailink";
};
