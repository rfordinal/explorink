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
  std::thread host([&] {
    const maptransfer::FrameGate::Ticket t(gate);
    ASSERT_TRUE(t);
    entered.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    left.store(true);
  });
  while (!entered.load()) yieldNow();
  EXPECT_TRUE(gate.close(5000, nowMs, yieldNow));
  EXPECT_TRUE(left.load());
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
    while (!release.load()) yieldNow();
  });
  while (!entered.load()) yieldNow();
  const uint32_t start = nowMs();
  EXPECT_FALSE(gate.close(30, nowMs, yieldNow));
  EXPECT_GE(nowMs() - start, 30u);
  release.store(true);
  host.join();
}

TEST(MapTransferGate, ConcurrentCloseNeverOverlapsAFrame) {
  // The plan's second done test, and item 2's race. The host thread runs
  // "frames" and "disconnects" back to back, each of which tears the transfer
  // down if it gets in. The activity thread closes the gate and then tears down
  // itself, as detach() does. Two teardowns at once is a failure; so is a frame
  // that gets in after close() returned true.
  //
  // `occupancy` counts who is in a teardown right now. Every round reopens the
  // gate, so the close lands at a different point in the host's loop each time.
  //
  // **Needs two CPUs.** A memory-ordering bug shows only when the two threads
  // really run at once; on one CPU they interleave, nothing reorders, and a
  // release/acquire mutant passes. Worse, the host thread busy-spins, so on one
  // CPU every yield below hands it a whole timeslice and the test took over 90 s
  // (review, 2026-11-28, `taskset -c 0`). Skipped there rather than passed.
  // Measured 2026-11-28 on a 14-core laptop, three runs each: 0 overlaps here,
  // 68-83 for an acq/rel mutant, 140-374 for the old read-then-publish order.
  if (usableCpus() < 2) GTEST_SKIP() << "needs two CPUs to race";
  // Up to 20,000 rounds or 10 s, whichever first, so a loaded CI host ends it in
  // bounded time; at least 2,000 rounds must run or the result is not evidence.
  constexpr int kRounds = 20000;
  constexpr uint32_t kBudgetMs = 10000;
  constexpr int kMinRounds = 2000;
  const uint32_t started = nowMs();
  int rounds = 0;
  int overlaps = 0;
  int lateFrames = 0;
  int framesIn = 0;
  for (int round = 0; round < kRounds && (round < kMinRounds || nowMs() - started < kBudgetMs); ++round) {
    ++rounds;
    maptransfer::FrameGate gate;
    gate.open();
    std::atomic<int> occupancy{0};
    std::atomic<bool> closed{false};
    std::atomic<bool> stop{false};
    std::atomic<int> hostOverlaps{0};
    std::atomic<int> hostLate{0};
    std::atomic<int> hostIn{0};

    std::thread host([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        const maptransfer::FrameGate::Ticket t(gate);
        if (!t) {
          // Turned away: the close has landed. Give a time-shared CPU back so
          // the activity thread finishes its round. Not on the success path,
          // which is where the race lives -- yielding there cut the acq/rel
          // mutant's overlaps from 45-72 to 3-5 per run.
          yieldNow();
          continue;
        }
        hostIn.fetch_add(1, std::memory_order_relaxed);
        if (closed.load()) hostLate.fetch_add(1, std::memory_order_relaxed);
        if (occupancy.fetch_add(1) != 0) hostOverlaps.fetch_add(1, std::memory_order_relaxed);
        occupancy.fetch_sub(1);
      }
    });

    // Let the host get going for a variable moment, then close.
    for (int spin = 0; spin < (round % 64); ++spin) yieldNow();
    ASSERT_TRUE(gate.close(5000, nowMs, yieldNow));
    closed.store(true);
    if (occupancy.fetch_add(1) != 0) ++overlaps;  // detach()'s abandon()
    for (int spin = 0; spin < 50; ++spin) yieldNow();
    occupancy.fetch_sub(1);
    stop.store(true);
    host.join();
    overlaps += hostOverlaps.load();
    lateFrames += hostLate.load();
    framesIn += hostIn.load();
  }
  EXPECT_GE(rounds, kMinRounds);
  EXPECT_EQ(overlaps, 0);
  EXPECT_EQ(lateFrames, 0);
  // The race has to have actually run: frames got in before the close.
  EXPECT_GT(framesIn, 0);
}
