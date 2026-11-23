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
  //
  // Sends at most kMaxBlocksPerPoll indication blocks per call -- each one
  // can block this task for up to BlePositionServer's 3 s confirm timeout, so
  // this bounds how long one call can freeze the screen. Whatever is left
  // (unread ring bytes, an unflushed partial batch) stays queued in order and
  // is picked up by the next call.
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

  // Caps how many indication blocks poll() will actually put on the link in
  // one call. Each block can cost BlePositionServer::sendCommandBlock up to
  // its kConfirmTimeoutMs (3 s) waiting for the peer's confirm, and that wait
  // runs on this activity task -- so N blocks sent in one poll() is an N x 3 s
  // worst-case freeze of the whole screen (buttons dead, nothing redraws)
  // against a hung-but-subscribed phone (docs/ble-review-2026-08.md,
  // "Console flush can freeze the activity task").
  //
  // 1 would halve that ceiling but doubles how many poll() ticks a multi-line
  // listing needs to clear, which doubles perceived listing latency on a
  // *healthy* link -- the common case would pay for the pathological one. 2 is
  // the compromise taken.
  //
  // **It does not bound the freeze.** This used to claim "worst case 6 s
  // instead of the previous unbounded". Measured false in the desktop
  // simulator 2026-08-23: one `info` at MTU 23 against a peer that stops
  // confirming froze the activity task for 69.1 s, with no loop() iteration
  // for 63 s of it (docs/tile-freshness.md, "The cap does not bound the
  // freeze"). The cap is checked in poll()'s loop over *input* bytes, and one
  // input byte -- the '\n' -- makes feed() run a whole command, which calls
  // appendReply 23 times; neither appendReply nor flushReplies consults
  // blocksSentThisPoll_. So this caps *commands* per poll(), not blocks, and
  // the freeze is per command. A real bound has to be checked where the blocks
  // are emitted.
  static constexpr int kMaxBlocksPerPoll = 2;

  // Appends one reply line (a '\n' is added), flushing first if it no longer
  // fits. A line longer than the budget goes out on its own.
  void appendReply(const char* line);

  // Sends whatever is buffered. Nothing to send is success. Unconditional --
  // always increments blocksSentThisPoll_ and never refuses to send, because
  // appendReply calls this to make room in the fixed-size batch_ and a refusal
  // there would leave the caller writing past a full buffer. The cap is
  // enforced by poll() instead: it stops feeding new input once the cap is
  // spent and skips its own trailing flush call in that case, leaving
  // whatever is left in batch_ for the next poll().
  bool flushReplies();

  MapCommandConsole console_;
  char batch_[kBatchBytes] = {};
  size_t batchLen_ = 0;

  // How many blocks poll() has already sent this call. Reset at the top of
  // poll(); read and incremented only on the activity task, so no lock.
  int blocksSentThisPoll_ = 0;
};
