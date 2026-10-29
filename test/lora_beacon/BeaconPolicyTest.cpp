#include <gtest/gtest.h>

#include "BeaconPolicy.h"

using BeaconPolicy::Action;
using BeaconPolicy::Decision;
using BeaconPolicy::Input;
using BeaconPolicy::Reason;

namespace {

// A node that has been running a while, has a clock and a fix, and sent an
// advert one floor period ago. Every test below changes one thing about it, so
// what the test is about is the line that differs from this.
Input settled() {
  Input in;
  in.hasSentEver = true;
  in.floorMs = 70000;
  in.sinceLastSentMs = 70000;
  in.hasWallClock = true;
  in.hasFix = true;
  in.lastSentHadFix = true;
  in.movedM = 0;
  in.declineLoggedEver = false;
  in.sinceLastDeclineLogMs = 0;
  return in;
}

TEST(BeaconPolicyFloor, NothingHappensInsideTheFloor) {
  Input in = settled();
  in.sinceLastSentMs = 69999;
  in.movedM = 10000;  // ten kilometres is still not worth breaking the floor
  // The floor is a duty-cycle limit, not a comfort setting: no rule may
  // produce more packets per hour than it allows.
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Nothing);
}

TEST(BeaconPolicyFloor, InsideTheFloorIsNotAnEventAndIsNeverLogged) {
  Input in = settled();
  in.sinceLastSentMs = 1000;
  const Decision d = BeaconPolicy::decide(in);
  // It happens thousands of times an hour. A row per tick would bury the rows
  // that mean something.
  EXPECT_EQ(d.action, Action::Nothing);
  EXPECT_FALSE(d.logDecline);
}

TEST(BeaconPolicyFloor, TheFirstAdvertDoesNotWaitOutACadence) {
  Input in = settled();
  in.hasSentEver = false;
  in.sinceLastSentMs = 0;
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::First);
}

TEST(BeaconPolicyClock, NoWallClockRefusesEvenWithNewsToTell) {
  Input in = settled();
  in.hasWallClock = false;
  in.movedM = 5000;
  const Decision d = BeaconPolicy::decide(in);
  // MeshCore stamps the advert from the RTC and the receiver dates the marker
  // by that stamp, so an advert with no clock behind it arrives years old and
  // is hidden on arrival. Measured exactly that way, 2026-10-23.
  EXPECT_EQ(d.action, Action::Decline);
  EXPECT_EQ(d.reason, Reason::NoClock);
}

TEST(BeaconPolicyClock, TheClockIsCheckedBeforeTheFirstAdvertToo) {
  Input in = settled();
  in.hasSentEver = false;
  in.hasWallClock = false;
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Decline);
  EXPECT_EQ(d.reason, Reason::NoClock);
  // With hasSentEver false the floor never gates, so this path runs on every
  // loop tick -- the throttle below is what keeps it from writing a row each
  // time.
  EXPECT_TRUE(d.logDecline);
}

TEST(BeaconPolicyClock, ButTheFloorStillComesFirst) {
  Input in = settled();
  in.hasWallClock = false;
  in.sinceLastSentMs = 100;
  // A clockless node inside its floor is silent, not declining: the cheapest
  // check runs first and there is nothing to report yet.
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Nothing);
}

TEST(BeaconPolicyNews, StandingStillWithNothingToSayDeclines) {
  const Decision d = BeaconPolicy::decide(settled());
  EXPECT_EQ(d.action, Action::Decline);
  EXPECT_EQ(d.reason, Reason::NoNews);
}

TEST(BeaconPolicyNews, AHundredMetresIsTheLine) {
  Input in = settled();
  in.movedM = BeaconPolicy::kMoveMetres - 1;
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Decline);

  in.movedM = BeaconPolicy::kMoveMetres;
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::Moved);
}

TEST(BeaconPolicyNews, DistanceIsOnlyReadWhenBothEndsAreRealPositions) {
  Input in = settled();
  in.hasFix = false;
  in.lastSentHadFix = false;
  in.movedM = 50000;  // stale arithmetic from before the fix was lost
  // Two nameplates are not 50 km apart. The distance is meaningless here and
  // must not reach the test.
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Decline);
  EXPECT_EQ(d.reason, Reason::NoNews);
}

TEST(BeaconPolicyNews, SeeingTheSkyAgainIsNewsOnItsOwn) {
  Input in = settled();
  in.lastSentHadFix = false;
  in.hasFix = true;
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::FixChanged);
}

TEST(BeaconPolicyNews, LosingTheSkyIsNewsToo) {
  Input in = settled();
  in.lastSentHadFix = true;
  in.hasFix = false;
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::FixChanged);
}

TEST(BeaconPolicyNews, AnHourParkedStillSaysWhereThePubIs) {
  Input in = settled();
  in.sinceLastSentMs = BeaconPolicy::kMaxSilenceSec * 1000UL - 1;
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Decline);

  in.sinceLastSentMs = BeaconPolicy::kMaxSilenceSec * 1000UL;
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::Keepalive);
}

TEST(BeaconPolicyNews, TheCapHoldsWithNoFixAtAll) {
  Input in = settled();
  in.hasFix = false;
  in.lastSentHadFix = false;
  in.sinceLastSentMs = BeaconPolicy::kMaxSilenceSec * 1000UL;
  // A node with no fix sends its nameplate once an hour, never 0,0.
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::Keepalive);
}

TEST(BeaconPolicyDeclineLog, TheFirstDeclineOfARunIsWritten) {
  Input in = settled();
  in.declineLoggedEver = false;
  EXPECT_TRUE(BeaconPolicy::decide(in).logDecline);
}

TEST(BeaconPolicyDeclineLog, OneRowPerCadenceAndNoMore) {
  Input in = settled();
  in.declineLoggedEver = true;

  in.sinceLastDeclineLogMs = in.floorMs - 1;
  EXPECT_FALSE(BeaconPolicy::decide(in).logDecline);

  in.sinceLastDeclineLogMs = in.floorMs;
  EXPECT_TRUE(BeaconPolicy::decide(in).logDecline);
}

TEST(BeaconPolicyDeclineLog, TheDeclineStampIsNotTheSendStamp) {
  Input in = settled();
  in.declineLoggedEver = true;
  in.sinceLastDeclineLogMs = 0;
  // An hour of declines must not postpone the parked heartbeat: the decline
  // throttle and the silence cap read two different clocks, and this is the
  // case that proves they are not the same number.
  in.sinceLastSentMs = BeaconPolicy::kMaxSilenceSec * 1000UL;
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::Keepalive);
}

}  // namespace
