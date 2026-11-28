#pragma once

#include <atomic>
#include <cstdint>

// The handshake between the BLE host task and the activity task that decides
// whether a frame (or a disconnect) may touch transfer state. T-2104, items 2
// and 4.
//
// Two flags. `attached_` says "this receiver accepts work"; `inFrame_` says "the
// host task is inside a callback right now". The host task publishes inFrame_
// and *then* reads attached_; the activity task (detach) clears attached_ and
// *then* reads inFrame_. With both pairs sequentially consistent, at least one
// side sees the other's write: either the frame sees attached_ false and backs
// out, or detach sees inFrame_ true and waits. Never both missing each other.
//
// **Why seq_cst and not release/acquire.** This is a store followed by a load of
// a *different* variable on each side -- Dekker's shape. A release store keeps
// earlier accesses before it, and an acquire load keeps later accesses after it;
// neither stops a later load from being performed before an earlier store. Under
// acq/rel both sides may read the other flag's old value, and the frame runs
// while detach() tears down under it. Only seq_cst forbids that (C++20
// [atomics.order], the single total order S). The plan for T-2104 asked for
// release/acquire; that is not enough for this pattern, and the host test
// MapTransferGate.ConcurrentCloseNeverOverlapsAFrame is there to catch it.
//
// docs/transfer-receiver-threading.md has the measurement and what the gate does
// not close.
//
// **The order used to be the losing one.** onFrame read attached_ first and
// published inFrame_ after, and detach() wrote attached_ then read inFrame_. On a
// dual-core board (env:x4pro, env:t5s3pro: NimBLE on core 0, the Arduino task on
// core 1) detach() could see inFrame_ false while a frame sat between its two
// lines and abandon the transfer underneath it.
//
// **A disconnect goes through the same gate.** The disconnect hook runs on the
// host task, so it never overlaps a frame. It does overlap detach(): both used to
// be able to run abandon() on one object at once.
//
// Header-only, no Arduino, so test/map_transfer can drive it with real threads.
namespace maptransfer {

class FrameGate {
 public:
  // Activity task. Before the hooks are installed.
  void open() { attached_.store(true, std::memory_order_seq_cst); }

  bool attached() const { return attached_.load(std::memory_order_seq_cst); }

  // Host task, at the top of every frame and every disconnect. True means the
  // caller is inside and must leave() -- use Ticket, not these two by hand.
  bool enter() {
    inFrame_.store(true, std::memory_order_seq_cst);
    if (attached_.load(std::memory_order_seq_cst)) return true;
    // Release, not seq_cst: nothing was touched, and the only reader is close()'s
    // wait loop, which needs to see the false eventually, not in any order.
    inFrame_.store(false, std::memory_order_release);
    return false;
  }

  // Release: everything the frame wrote to transfer state happens-before
  // close()'s acquire load that sees this false.
  void leave() { inFrame_.store(false, std::memory_order_release); }

  // Activity task. Stops new work and waits for work already inside to leave.
  // `nowMs` and `yield` are the clock and the sleep, so the host test can run
  // this with std::chrono and the device with millis() and vTaskDelay(1).
  // True when nothing was inside at the end; false when the wait ran out, which
  // the caller treats as "proceed anyway" (MapTransferReceiver::detach, why).
  template <class NowMs, class Yield>
  bool close(const uint32_t waitMs, NowMs nowMs, Yield yield) {
    attached_.store(false, std::memory_order_seq_cst);
    const uint32_t start = nowMs();
    while (inFrame_.load(std::memory_order_seq_cst)) {
      if (nowMs() - start >= waitMs) return false;
      yield();
    }
    return true;
  }

  // Holds the gate for one callback. A destructor rather than a set/clear pair:
  // a clear left behind by a later early `return` would leave inFrame_ stuck
  // true, and every later close() would wait out its whole bound -- for ever in
  // MapTransferReceiver::detach(), which waits without one.
  class Ticket {
   public:
    explicit Ticket(FrameGate& gate) : gate_(gate), in_(gate.enter()) {}
    ~Ticket() {
      if (in_) gate_.leave();
    }
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    explicit operator bool() const { return in_; }

   private:
    FrameGate& gate_;
    const bool in_;
  };

 private:
  std::atomic<bool> attached_{false};
  std::atomic<bool> inFrame_{false};
};

}  // namespace maptransfer
