// The frame gate between the BLE host task and the activity task -- T-2104,
// items 2 and 4.
//
// What these tests are for: the two done tests the plan names. detach() during
// a frame still waits the host task out, and a disconnect during detach() cannot
// run a second teardown. Both are the same property of MapTransferGate.h -- once
// close() has returned true, nothing is inside and nothing new gets in -- so the
// race test drives it with two real threads.
//
// What they cannot reach: the device binds the clock to millis() and the yield
// to vTaskDelay(1), and the cores are an S3's, not this host's. The test proves
// the gate's own orderings; it does not prove the receiver calls it on every
// path. That part is read off MapTransferReceiver.cpp (onFrame,
// disconnectTrampoline, detach, the destructor).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "MapTransferGate.h"

#if defined(__linux__)
#include <sched.h>
#endif

namespace {

uint32_t nowMs() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void yieldNow() { std::this_thread::yield(); }

// CPUs this process may actually run on. Not hardware_concurrency(): that counts
// the machine's cores and ignores affinity, so under `taskset -c 0` or a
// one-CPU container quota it still answers 14 and the race test would not skip.
unsigned usableCpus() {
#if defined(__linux__)
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof(set), &set) == 0) return static_cast<unsigned>(CPU_COUNT(&set));
#endif
  return std::thread::hardware_concurrency();
}

}  // namespace

TEST(MapTransferGate, AClosedGateTurnsAFrameAway) {
  maptransfer::FrameGate gate;
  // Never opened: a receiver that was not attached accepts nothing.
  {
    const maptransfer::FrameGate::Ticket t(gate);
    EXPECT_FALSE(t);
  }
  gate.open();
  {
    const maptransfer::FrameGate::Ticket t(gate);
    EXPECT_TRUE(t);
  }
  EXPECT_TRUE(gate.close(0, nowMs, yieldNow));
  const maptransfer::FrameGate::Ticket t(gate);
  EXPECT_FALSE(t);
}

TEST(MapTransferGate, ATurnedAwayFrameDoesNotHoldCloseUp) {
  // A frame refused at the gate must clear its own mark, or every later
  // close() would wait out its whole bound for a frame that never ran.
  maptransfer::FrameGate gate;
  {
    const maptransfer::FrameGate::Ticket t(gate);
  }
  EXPECT_TRUE(gate.close(0, nowMs, yieldNow));
}

TEST(MapTransferGate, CloseWaitsForAFrameInside) {
  // The plan's first done test: detach() during a frame waits the host task
  // out. The frame holds its ticket for 50 ms; close() must not return before
  // the frame has left.
  maptransfer::FrameGate gate;
  gate.open();
  std::atomic<bool> entered{false};
  std::atomic<bool> left{false};
  std::atomic<bool> admitted{false};
  std::thread host([&] {
    const maptransfer::FrameGate::Ticket t(gate);
    admitted.store(static_cast<bool>(t));
    // Set either way, so a gate that wrongly refuses fails below instead of
    // leaving the main thread spinning for ever.
    entered.store(true);
    if (!t) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    left.store(true);
  });
  while (!entered.load()) yieldNow();
  EXPECT_TRUE(admitted.load());
  EXPECT_TRUE(gate.close(5000, nowMs, yieldNow));
  EXPECT_TRUE(left.load() || !admitted.load());
  host.join();
}

TEST(MapTransferGate, CloseGivesUpAfterItsBound) {
  // A wedged card must not hold the screen: close() answers false after the
  // bound and the receiver proceeds (MapTransferReceiver::detach).
  maptransfer::FrameGate gate;
  gate.open();
  std::atomic<bool> release{false};
  std::atomic<bool> entered{false};
  std::thread host([&] {
    const maptransfer::FrameGate::Ticket t(gate);
    entered.store(true);
    // Leaves on its own after 2 s, so a close() that never gives up returns
    // true and fails the EXPECT_FALSE below instead of hanging the suite.
    const uint32_t heldFrom = nowMs();
    while (!release.load() && nowMs() - heldFrom < 2000) yieldNow();
  });
  while (!entered.load()) yieldNow();
  const uint32_t start = nowMs();
  EXPECT_FALSE(gate.close(30, nowMs, yieldNow));
  EXPECT_GE(nowMs() - start, 30u);
  release.store(true);
  host.join();
}

TEST(MapTransferGate, ConcurrentCloseNeverOverlapsAFrame) {
  // The plan's second done test, and item 2's race. A host thread runs frames
  // back to back; each one that gets in occupies "the transfer" for a moment.
  // The activity thread opens the gate, lets frames in, closes it, and then
  // occupies the transfer itself, as detach()'s abandon() does. Both inside at
  // once is a failure.
  //
  // **One host thread for the whole run, many open/close cycles.** The first
  // version started a thread per round, so a run held only a few thousand
  // closes, and on a two-CPU host a release/acquire gate survived it
  // (mutation run, 2026-11-28, `scripts/mutation_check.py`). The
  // window that ordering opens is a store overtaken by a load, nanoseconds
  // wide; it needs closes by the million to be hit every run.
  //
  // **Needs two CPUs, and two physical cores to be a strong check.** On one CPU
  // the threads interleave and nothing reorders, so a broken ordering passes;
  // skipped there rather than passed. Two hyperthreads of one core reorder far
  // less: measured 2026-11-28 on this laptop, the release/acquire mutant failed
  // 0 of 10 runs pinned to CPUs 0,1 (one core) and 5 of 5 on 0,2 or unpinned.
  if (usableCpus() < 2) GTEST_SKIP() << "needs two CPUs to race";

  constexpr uint32_t kBudgetMs = 4000;
  constexpr long kMinCycles = 200000;
  maptransfer::FrameGate gate;
  std::atomic<bool> stop{false};
  std::atomic<int> occupancy{0};
  std::atomic<long> overlaps{0};
  std::atomic<long> framesIn{0};

  // Holds "inside" for a few dozen instructions, so an overlap has a window
  // wider than one atomic op to land in.
  const auto occupy = [&] {
    if (occupancy.fetch_add(1) != 0) overlaps.fetch_add(1);
    for (int spin = 0; spin < 16; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
    occupancy.fetch_sub(1);
  };

  std::thread host([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      const maptransfer::FrameGate::Ticket t(gate);
      if (!t) continue;
      framesIn.fetch_add(1, std::memory_order_relaxed);
      occupy();
    }
  });

  const uint32_t started = nowMs();
  long cycles = 0;
  bool closeTimedOut = false;
  while (nowMs() - started < kBudgetMs) {
    gate.open();
    for (long spin = 0; spin < cycles % 64; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
    if (!gate.close(5000, nowMs, yieldNow)) {
      // Recorded, not ASSERTed: an ASSERT here returns with `host` joinable,
      // which is std::terminate rather than a failure.
      closeTimedOut = true;
      break;
    }
    occupy();  // detach()'s abandon(), with the gate closed
    ++cycles;
  }
  stop.store(true);
  host.join();

  EXPECT_FALSE(closeTimedOut);
  EXPECT_EQ(overlaps.load(), 0);
  EXPECT_GT(framesIn.load(), 0);  // the race has to have run at all
  if (!closeTimedOut && cycles < kMinCycles) {
    GTEST_SKIP() << "only " << cycles << " cycles in " << kBudgetMs << " ms; host too loaded to race";
  }
}
