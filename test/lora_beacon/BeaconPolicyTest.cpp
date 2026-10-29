#include <gtest/gtest.h>

#include "BeaconPolicy.h"
#include "MapFixTrust.h"

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

TEST(BeaconPolicyThreshold, AnUnstatedAccuracyBehavesAsBefore) {
  // Every GNSS fix on this hardware lands here: the receiver reports no
  // accuracy in any protocol and HDOP cannot stand in for one.
  EXPECT_EQ(BeaconPolicy::moveThresholdM(MapFixTrust::kAccuracyUnstated), BeaconPolicy::kMoveMetres);
}

TEST(BeaconPolicyThreshold, AGoodFixKeepsTheHundredMetres) {
  EXPECT_EQ(BeaconPolicy::moveThresholdM(5), BeaconPolicy::kMoveMetres);
  EXPECT_EQ(BeaconPolicy::moveThresholdM(MapFixTrust::kLooseAtOrAboveM - 1), BeaconPolicy::kMoveMetres);
}

TEST(BeaconPolicyThreshold, AFixTheMarkerWouldNotTrustDoublesIt) {
  EXPECT_EQ(BeaconPolicy::moveThresholdM(MapFixTrust::kLooseAtOrAboveM), BeaconPolicy::kLooseMoveMetres);
  EXPECT_EQ(BeaconPolicy::moveThresholdM(50), BeaconPolicy::kLooseMoveMetres);
}

TEST(BeaconPolicyThreshold, TheWanderUnderTreesDoesNotEarnAPacket) {
  Input in = settled();
  in.accuracyM = 40;
  // 120 m against a 40 m error circle measured from a fixed anchor is the
  // shape GPS noise makes under canopy, not movement.
  in.movedM = 120;
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Decline);

  // Real movement still gets through.
  in.movedM = 200;
  EXPECT_EQ(BeaconPolicy::decide(in).reason, Reason::Moved);
}

TEST(BeaconPolicySettled, StoppingShortOfTheThresholdIsSaidOnce) {
  Input in = settled();
  in.movedM = 60;  // walked a bit, below the 100 m threshold
  in.stillForMs = BeaconPolicy::kSettleHoldFloors * in.floorMs;

  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::Settled);

  // And exactly once: the latch is what keeps a long rest from being a
  // packet per cadence.
  in.settledAnnounced = true;
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Decline);
}

TEST(BeaconPolicySettled, OnePauseAtAGateIsNotAStop) {
  Input in = settled();
  in.movedM = 60;
  in.stillForMs = BeaconPolicy::kSettleHoldFloors * in.floorMs - 1;
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Decline);
}

TEST(BeaconPolicySettled, StoppingWhereTheGroupAlreadyThinksYouAreIsNotNews) {
  Input in = settled();
  // Inside the settle radius of the last position sent, so the marker on
  // everybody's panel is already in the right place.
  in.movedM = BeaconPolicy::kSettleRadiusM - 1;
  in.stillForMs = BeaconPolicy::kSettleHoldFloors * in.floorMs * 10;
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Decline);
}

TEST(BeaconPolicySettled, MovedWinsWhenBothApply) {
  Input in = settled();
  in.movedM = 150;
  in.stillForMs = BeaconPolicy::kSettleHoldFloors * in.floorMs;
  // Same packet either way -- the rider's coordinate. Moved is the simpler
  // truth and the rule that already existed.
  EXPECT_EQ(BeaconPolicy::decide(in).reason, Reason::Moved);
}

TEST(BeaconPolicyMoving, SlowIsNotParked) {
  Input in = settled();
  in.movedM = 60;  // scrambling, below the move threshold
  in.sinceLastSentMs = BeaconPolicy::kMovingCapSec * 1000UL;
  const Decision d = BeaconPolicy::decide(in);
  EXPECT_EQ(d.action, Action::Send);
  EXPECT_EQ(d.reason, Reason::Moving);
}

TEST(BeaconPolicyMoving, StandingStillStillWaitsTheWholeHour) {
  Input in = settled();
  in.movedM = BeaconPolicy::kSettleRadiusM - 1;  // GPS wander, not walking
  in.sinceLastSentMs = BeaconPolicy::kMovingCapSec * 1000UL;
  // The ten-minute cap is for somebody who is demonstrably moving. A parked
  // node must not buy itself six times the airtime on noise.
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Decline);
}

TEST(BeaconPolicyMoving, TenMinutesIsTheLine) {
  Input in = settled();
  in.movedM = 60;
  in.sinceLastSentMs = BeaconPolicy::kMovingCapSec * 1000UL - 1;
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Decline);
}

TEST(BeaconPolicyMoving, TheFloorStillOutranksIt) {
  Input in = settled();
  in.floorMs = 300000;  // the five-minute rung
  in.movedM = 60;
  in.sinceLastSentMs = 200000;
  // Ten minutes have not passed and neither has the floor. Nothing may send
  // more often than the rider's cadence row allows.
  EXPECT_EQ(BeaconPolicy::decide(in).action, Action::Nothing);
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
