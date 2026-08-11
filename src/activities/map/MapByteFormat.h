#pragma once

// How a transfer is stated to a rider: bytes, kB, MB, and durations.
//
// One definition, three readers -- the sync screen, the map screen's debug
// readout, and (ported, not shared) the phone app's TileFormat.kt. The rider
// watches the panel and the phone at the same time, so two of them formatting
// one transfer differently is a bug in itself. It happened twice: the debug
// readout printed raw bytes while the sync screen printed kB, and the app
// computed kB as 1024 bytes while this computes 1000.
//
// **Decimal kB/MB, not KiB.** The number sits next to a file size a rider
// compares against a phone's storage screen, which is decimal everywhere.

#include <cstdint>
#include <cstdio>

namespace mapfmt {

inline void formatBytes(uint32_t bytes, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) return;
  if (bytes < 1000) {
    snprintf(out, outSize, "%lu B", static_cast<unsigned long>(bytes));
  } else if (bytes < 1000000) {
    snprintf(out, outSize, "%lu kB", static_cast<unsigned long>((bytes + 500) / 1000));
  } else {
    // One decimal past a megabyte -- "1 MB" for anything from 1.0 to 1.9 would
    // hide most of a fetch's progress.
    const uint32_t tenths = (bytes + 50000) / 100000;
    snprintf(out, outSize, "%lu.%lu MB", static_cast<unsigned long>(tenths / 10),
             static_cast<unsigned long>(tenths % 10));
  }
}

inline void formatDuration(uint32_t seconds, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) return;
  if (seconds >= 3600) {
    snprintf(out, outSize, "%luh %lum", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>((seconds % 3600) / 60));
  } else if (seconds >= 60) {
    snprintf(out, outSize, "%lum %lus", static_cast<unsigned long>(seconds / 60),
             static_cast<unsigned long>(seconds % 60));
  } else {
    snprintf(out, outSize, "%lus", static_cast<unsigned long>(seconds));
  }
}

}  // namespace mapfmt
