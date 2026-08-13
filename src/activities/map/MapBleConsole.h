#pragma once

#include "MapCommandConsole.h"

// BLE front end for the map command console (P5).
//
// The mirror image of MapSerialConsole, and deliberately so: same grammar,
// same state, same replies, different transport. That is the entire reason
// MapCommandParser was written free of Arduino and of the activity -- a
// parser reachable only from a loop() cannot be reached from a BLE callback,
// and two parsers would drift within a week.
//
// **This is the channel that ships.** The phone talks BLE; USB serial is a
// desk tool. If the two ever disagree about a command, this one is right.
//
// Bytes arrive on the NimBLE host task and are queued there
// (BlePositionServer::readCommandBytes). poll() drains that queue on the
// activity's own task, so a command that ends in a full-screen refresh
// blocks the activity, never the BLE stack.
class MapBleConsole {
 public:
  explicit MapBleConsole(MapConsoleState& state);

  // Drains whatever the BLE callback has queued, runs any completed lines,
  // notifies the replies. Returns true if the screen needs redrawing.
  bool poll();

 private:
  friend class BatchingReplyWriter;

  // Reply lines are packed into one indication each time they fill this, and
  // whatever is left goes out at the end of poll(). Sized to the biggest ATT
  // payload this link can carry (MTU 256 -> 253 bytes); the actual budget is
  // read per flush from BlePositionServer::commandPayloadBytes(), so a link
  // that never got past the 23-byte default still sends legal indications.
  //
  // A class member, not a local in poll(): the NimBLE host task's stack is
  // 4 KB and firmware CLAUDE.md caps a function's locals at 256 bytes, which
  // poll()'s own 128-byte read buffer already half spends.
  static constexpr size_t kBatchBytes = 253;

  // Appends one reply line (a '\n' is added), flushing first if it no longer
  // fits. A line longer than the budget goes out on its own.
  void appendReply(const char* line);

  // Sends whatever is buffered. Nothing to send is success.
  bool flushReplies();

  MapCommandConsole console_;
  char batch_[kBatchBytes] = {};
  size_t batchLen_ = 0;
};
