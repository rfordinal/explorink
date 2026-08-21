#include <gtest/gtest.h>

#include "ParkedLoopPolicy.h"

// The table this suite pins is the point of the file under test: the parked
// cadence decision has to be checkable without a device, so a per-tick timer
// added to the map screen six months from now fails here rather than costing
// a milliamp nobody notices (docs/power-idle-sleep.md, "How this gets
// guarded", item 2).

namespace {

using parked_loop::Cadence;
using parked_loop::Inputs;
using parked_loop::isParked;
using parked_loop::tickMs;

// A cadence with the two values clearly apart, so a test failure says which
// branch was taken. The real parked number is not measured yet -- experiment 3
// sets it -- so no test here asserts a specific millisecond count as correct
// policy, only which of the two the decision picked.
constexpr Cadence kTest{/*activeMs=*/10, /*parkedMs=*/200};

// Idle long enough that recent input is not what earns the fast tick.
constexpr uint32_t kIdle = parked_loop::kRecentInputMs + 1;

Inputs parkedAndIdle() {
  Inputs in;
  in.parked = true;
  in.msSinceInput = kIdle;
  return in;
}

}  // namespace

TEST(ParkedLoopPolicy, DefaultCadenceChangesNothing) {
  // The shipped default is today's behaviour on both branches. A build that
  // takes this file without setting a parked value must measure the same loop
  // rate it did before, or the policy has become a silent behaviour change.
  Inputs in = parkedAndIdle();
  EXPECT_EQ(tickMs(in), Cadence{}.activeMs);
  EXPECT_FALSE(isParked(in));
}

TEST(ParkedLoopPolicy, ParkedAndIdleTakesTheParkedCadence) {
  EXPECT_EQ(tickMs(parkedAndIdle(), kTest), 200u);
  EXPECT_TRUE(isParked(parkedAndIdle(), kTest));
}

TEST(ParkedLoopPolicy, NotParkedStaysFast) {
  Inputs in = parkedAndIdle();
  in.parked = false;
  EXPECT_EQ(tickMs(in, kTest), 10u);
  EXPECT_FALSE(isParked(in, kTest));
}

TEST(ParkedLoopPolicy, TransferWinsOverParked) {
  // Throughput is bound by our own tick rate, so a transfer must never be
  // slowed by a screen that thinks it is idle.
  Inputs in = parkedAndIdle();
  in.transferActive = true;
  EXPECT_EQ(tickMs(in, kTest), 10u);
}

TEST(ParkedLoopPolicy, QueuedWorkWinsOverParked) {
  Inputs in = parkedAndIdle();
  in.queuedWork = true;
  EXPECT_EQ(tickMs(in, kTest), 10u);
}

TEST(ParkedLoopPolicy, RecentInputWinsOverParked) {
  // Matches the CPU throttle's own threshold: while the clock is still full
  // from a button press, parking the loop costs latency and saves nothing.
  Inputs in = parkedAndIdle();
  in.msSinceInput = 0;
  EXPECT_EQ(tickMs(in, kTest), 10u);
  in.msSinceInput = parked_loop::kRecentInputMs - 1;
  EXPECT_EQ(tickMs(in, kTest), 10u);
}

TEST(ParkedLoopPolicy, InputExactlyAtTheThresholdParks) {
  // The boundary is stated rather than left to whoever reads the < later.
  Inputs in = parkedAndIdle();
  in.msSinceInput = parked_loop::kRecentInputMs;
  EXPECT_EQ(tickMs(in, kTest), 200u);
}

TEST(ParkedLoopPolicy, EveryFastReasonIndependentlySufficient) {
  // One flag at a time, so a future change that collapses the three reasons
  // into one condition cannot pass by accident.
  for (int which = 0; which < 3; ++which) {
    Inputs in = parkedAndIdle();
    if (which == 0) in.transferActive = true;
    if (which == 1) in.queuedWork = true;
    if (which == 2) in.msSinceInput = 0;
    EXPECT_EQ(tickMs(in, kTest), 10u) << "reason " << which;
  }
}

TEST(ParkedLoopPolicy, PolicyIsUsableAtCompileTime) {
  // constexpr is not decoration: it lets the parked branch be asserted in a
  // static_assert at a call site, and it proves the function touches no
  // hardware and no clock of its own.
  constexpr Inputs kParked{/*parked=*/true, /*queuedWork=*/false, /*transferActive=*/false,
                           /*msSinceInput=*/kIdle};
  static_assert(tickMs(kParked, kTest) == 200u);
  static_assert(isParked(kParked, kTest));
  SUCCEED();
}
