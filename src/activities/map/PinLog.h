#pragma once

#include <cstdint>

#include "PinLogScanner.h"
#include "PinRecord.h"
#include "PinStore.h"

// The append-only pin history on the card. The only file in the pins feature
// that touches storage; everything it needs parsed lives in PinRecord and
// PinLogScanner, so the format rules stay host-testable with no card attached.
//
// All access goes through HalStorage -- SdFat is not thread-safe (CLAUDE.md).
//
// The path keeps `/trailink`: the on-card layout is infrastructure not yet
// renamed (parent CLAUDE.md, Naming).
class PinLog {
 public:
  static constexpr const char* kDir = "/trailink/pins";
  static constexpr const char* kPath = "/trailink/pins/pins.log";

  // Appends one record and flushes. False on any card failure -- the caller must
  // then **not** apply the record to its PinStore, or RAM would claim a pin the
  // history has never heard of.
  //
  // One line per user action is not a write-throttling violation: pin actions are
  // rare and each one is a real change (CLAUDE.md, Resource Protocol 8).
  static bool append(const PinRecord& rec);

  // Rebuilds the active pins. Returns false only when the card itself could not
  // be read; a log that does not exist yet is a successful empty replay, and a
  // damaged record inside it is counted in `stats` and skipped.
  static bool replay(PinStore& store, PinReplayStats& stats);

  // Newest-first paging for `pin log`. `offset` counts back from the newest
  // record. Hands at most `maxCount` records to the visitor, newest first, and
  // returns the total number of valid records in the file (which is what tells
  // the caller whether another page exists).
  //
  // Streams the file rather than holding it: one pass counts, one pass notes the
  // byte offset of each line in the window, then each line is read back at its
  // offset. Cost is two scans plus `maxCount` short reads; memory is one line
  // buffer whatever the log's length.
  static uint32_t page(uint32_t offset, uint32_t maxCount, IPinLogVisitor& visitor);

  // Cap on the offsets one page() call remembers. Ties the stack cost to a
  // constant instead of to `maxCount`, which comes off the wire.
  static constexpr uint32_t kMaxPageEntries = 10;
};
