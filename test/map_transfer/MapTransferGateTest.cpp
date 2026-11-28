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

namespace {

uint32_t nowMs() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void yieldNow() { std::this_thread::yield(); }

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
  constexpr int kRounds = 20000;
  int overlaps = 0;
  int lateFrames = 0;
  int framesIn = 0;
  for (int round = 0; round < kRounds; ++round) {
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
        if (!t) continue;
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
  EXPECT_EQ(overlaps, 0);
  EXPECT_EQ(lateFrames, 0);
  // The race has to have actually run: frames got in before the close.
  EXPECT_GT(framesIn, 0);
}
