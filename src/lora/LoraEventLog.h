#pragma once

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include "LoraEventLine.h"

// The file, one per day, same rotation shape as the black box: whole days are
// deleted, never lines, so a file being read on a laptop does not change under
// it. A device with no clock writes `ev-noclock.csv`, which rotation never
// touches because a file it cannot date is a file it cannot judge.
class LoraEventLog {
 public:
  static constexpr const char* kDir = "/trailink/lora";

  // Appends and flushes, creating the day file with its header on first use.
  // False on any card failure. **A failure never stops the radio**: unlike the
  // black box, where a row that did not land must stop the marker moving, a
  // lost diagnostic line costs only the diagnosis.
  static bool append(const LoraLogEvent& ev, uint32_t utc, uint32_t bootId);

  // Deletes dated files older than `keepDays` before `nowUtc`. Does nothing
  // with no clock.
  static void rotate(uint32_t nowUtc, uint32_t keepDays);

  // Days kept by default. Shorter than the black box's fourteen: this is a
  // debugging record of one walk, not a history anybody goes back to.
  static constexpr uint32_t kKeepDays = 7;

  // Longest name written, terminator included: `ev-20261023.csv`.
  static constexpr size_t kNameBytes = 20;

  // Bound on one directory listing, same reason as the black box's.
  static constexpr int kMaxDayFiles = 64;
};

#endif  // ENABLE_LORA_MESH
