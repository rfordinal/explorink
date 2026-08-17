#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "PinRecord.h"
#include "PinStore.h"

// Turning a stream of card bytes into records, and a replay over PinStore.
//
// Pure: no Arduino, no HAL, no SD. PinLog reads the file in chunks and pushes
// them in here, which is what keeps every damage rule host-testable -- the
// reader's behaviour on a bad CRC, an unknown version and a torn last line is
// tested with no card behind it (test/pins/).
//
// Never whole-file into RAM: one fixed line buffer, whatever the log's length
// (docs/pins-plan.md, "Reading, and damage").

class PinLogScanner {
 public:
  // Called once per complete line, empty lines included -- the caller decides
  // what an empty line means. A plain function pointer plus context, not
  // std::function, for its heap and binary cost (CLAUDE.md).
  using LineFn = void (*)(void* ctx, std::string_view line);

  PinLogScanner(LineFn fn, void* ctx) : fn_(fn), ctx_(ctx) {}

  void feed(const char* data, size_t len);

  // End of file. An unterminated tail is a torn write and is **discarded**, not
  // parsed: half a record can still pass a field count and would then be applied
  // as if it were whole. Counted in droppedTail().
  void finish();

  // Lines longer than a record can legally be, discarded to their own line end.
  uint32_t droppedLong() const { return droppedLong_; }
  bool droppedTail() const { return droppedTail_; }

 private:
  void emit();

  LineFn fn_;
  void* ctx_;
  char buf_[kPinLineMax + 1] = {};
  size_t len_ = 0;
  bool discarding_ = false;
  uint32_t droppedLong_ = 0;
  bool droppedTail_ = false;
};

struct PinReplayStats {
  uint32_t applied = 0;  // records that reached the store
  uint32_t skipped = 0;  // lines refused: bad CRC, bad version, bad fields, no room
};

// Decode one line and apply it. An undecodable line counts as skipped and the
// replay carries on -- **a damaged record never invalidates the log**. An empty
// line is neither applied nor skipped.
void pinReplayLine(std::string_view line, PinStore& store, PinReplayStats& stats);

// Scanner plus store: feed the file, then finish(). Used by PinLog on the device
// and directly by the tests.
class PinLogReplayer {
 public:
  explicit PinLogReplayer(PinStore& store) : store_(store), scanner_(&onLine, this) {}

  void feed(const char* data, size_t len) { scanner_.feed(data, len); }
  void finish();

  const PinReplayStats& stats() const { return stats_; }

 private:
  static void onLine(void* ctx, std::string_view line);

  PinStore& store_;
  PinReplayStats stats_;
  PinLogScanner scanner_;
};
