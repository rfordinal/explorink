#include "MapGnssHeading.h"

#include <gtest/gtest.h>

// The heading a GNSS fix draws. Every number here comes from the module's own
// constants or from the receiver measurement that chose them (a stationary desk
// reporting speed 1.3 km/h with course 211.9 degrees, 2026-08-31).

namespace {

using MapGnssHeading::State;
using MapGnssHeading::stepFor;

// Gets the state moving and parked on a known step, so a test can start from
// somewhere other than north.
State movingAt(float courseDegrees) {
  State state;
  stepFor(10.0f, courseDegrees, state);
  return state;
}

}  // namespace

TEST(GnssHeading, RestingCourseNeverMovesTheStep) {
  State state;
  // The measured desk reading. It must not turn the compass.
  EXPECT_EQ(stepFor(1.3f, 211.9f, state), 0);
  EXPECT_FALSE(state.moving);
}

TEST(GnssHeading, NorthUntilTheRiderFirstMoves) {
  State state;
  stepFor(0.0f, 90.0f, state);
  stepFor(1.9f, 180.0f, state);
  EXPECT_EQ(state.step, 0);
}

TEST(GnssHeading, AboveTheGateTheCourseIsBelieved) {
  State state;
  EXPECT_EQ(stepFor(30.0f, 90.0f, state), 4);   // east
  EXPECT_EQ(stepFor(30.0f, 180.0f, state), 8);  // south
  EXPECT_EQ(stepFor(30.0f, 270.0f, state), 12); // west
}

TEST(GnssHeading, BetweenTheThresholdsTheGateKeepsItsAnswer) {
  State state = movingAt(90.0f);
  // 2.5 is below kMovingKmh and at or above kHoldingKmh. What the hysteresis
  // holds there is the GATE, not the step: a rider already believed keeps being
  // believed, so the course still drives the heading. Only a drop below
  // kHoldingKmh stops it (the next test).
  //
  // Written the other way round first, asserting the step froze, and the test
  // is what forced the distinction to be said out loud.
  EXPECT_TRUE(state.moving);
  EXPECT_EQ(stepFor(2.5f, 270.0f, state), 12);
  EXPECT_TRUE(state.moving);
}

TEST(GnssHeading, RisingThroughTheGapDoesNotStartBelievingTheCourse) {
  // The same gap from below: a rider who has not yet reached kMovingKmh is not
  // believed just because they passed kHoldingKmh.
  State state;
  EXPECT_EQ(stepFor(2.5f, 270.0f, state), 0);
  EXPECT_FALSE(state.moving);
}

TEST(GnssHeading, DroppingBelowTheLowerThresholdHoldsTheLastStep) {
  State state = movingAt(180.0f);
  EXPECT_EQ(stepFor(0.5f, 0.0f, state), 8);
  EXPECT_FALSE(state.moving);
  // And it keeps holding it, rather than decaying to north.
  EXPECT_EQ(stepFor(0.0f, 45.0f, state), 8);
}

TEST(GnssHeading, TheDeadbandSwallowsCourseNoiseInsideAStep) {
  State state = movingAt(90.0f);
  // 100 degrees still rounds to step 4, and is inside the deadband anyway.
  EXPECT_EQ(stepFor(30.0f, 100.0f, state), 4);
  // 101.25 is the exact boundary between step 4 and step 5. Without the
  // deadband this is where a course sitting still would flip the frame back
  // and forth on noise alone.
  EXPECT_EQ(stepFor(30.0f, 101.25f, state), 4);
}

TEST(GnssHeading, ARealTurnStillLandsWithinOneFix) {
  State state = movingAt(90.0f);
  // A rider turning from east to south-east: past the deadband, so it follows.
  EXPECT_EQ(stepFor(30.0f, 112.5f, state), 5);
}

TEST(GnssHeading, TheShortWayRoundTheCircle) {
  State state = movingAt(0.0f);
  // 350 is 10 degrees from north, not 350. Measured the long way it would look
  // like a huge turn and rotate the frame for nothing.
  EXPECT_EQ(stepFor(30.0f, 350.0f, state), 0);
  // And a real turn the other side of the wrap does follow.
  EXPECT_EQ(stepFor(30.0f, 315.0f, state), 14);
}

TEST(GnssHeading, ACourseOf360NeverLandsOnStep16) {
  State state = movingAt(180.0f);
  EXPECT_EQ(stepFor(30.0f, 360.0f, state), 0);
  EXPECT_LT(state.step, 16);
}

TEST(GnssHeading, EveryStepStaysInsideTheFourBitField) {
  State state;
  for (float course = 0.0f; course < 360.0f; course += 1.0f) {
    // Fresh state each time, so the deadband never suppresses the answer.
    State fresh;
    const uint8_t step = stepFor(30.0f, course, fresh);
    EXPECT_LT(step, 16) << "course " << course;
  }
}
