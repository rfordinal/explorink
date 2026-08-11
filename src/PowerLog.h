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
// Cost: one open/append/close of ~120 bytes every 60 s. At six rows an hour the
// file grows about 7 KB per hour of riding, so nothing rotates it.
class PowerLog {
 public:
  static constexpr uint32_t kIntervalMs = 60000;

  // Call once per main-loop iteration. Writes at most one row per kIntervalMs
  // and returns immediately the rest of the time. Silent no-op until the SD
  // card is mounted.
  static void tick();

  // Where the rows go. Under the same on-device root as the tiles; the path
  // still says trailink because the card's directory has not been renamed
  // (parent CLAUDE.md).
  static constexpr const char* kPath = "/trailink/power.csv";
  static constexpr const char* kDir = "/trailink";
};
