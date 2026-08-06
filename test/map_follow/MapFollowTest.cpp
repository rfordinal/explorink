#include <gtest/gtest.h>

#include "MapFollow.h"

namespace {

// A 480x800 screen with the marker sitting at the ride-mode ladder anchor
// (MapViewport::kMarkerLadder step 3 = 690), heading north, nothing spent.
MapFollow::Request baseRequest() {
  MapFollow::Request request;
  request.fixX = 240;
  request.fixY = 690;
  request.drawnX = 240;
  request.drawnY = 690;
  request.screenWidth = 480;
  request.screenHeight = 800;
  request.anchorHeadingStep = 0;
  request.fixHeadingStep = 0;
  request.partialMoves = 0;
  return request;
}

TEST(MapFollowHeading, DriftWraps) {
  EXPECT_EQ(MapFollow::headingDriftSteps(0, 0), 0);
  EXPECT_EQ(MapFollow::headingDriftSteps(1, 0), 1);
  // N against NNW is one step, not fifteen -- the wrap is the whole point.
  EXPECT_EQ(MapFollow::headingDriftSteps(0, 15), 1);
  EXPECT_EQ(MapFollow::headingDriftSteps(15, 0), 1);
  EXPECT_EQ(MapFollow::headingDriftSteps(4, 0), 4);
  EXPECT_EQ(MapFollow::headingDriftSteps(0, 12), 4);
  // Opposite: eight steps either way round, so it must not report nine.
  EXPECT_EQ(MapFollow::headingDriftSteps(0, 8), 8);
  EXPECT_EQ(MapFollow::headingDriftSteps(8, 0), 8);
}

TEST(MapFollowHeading, RelativeIsMeasuredFromTheFrame) {
  // A frame drawn for the fix it is showing puts the arrow straight up.
  EXPECT_EQ(MapFollow::relativeHeadingStep(6, 6), 0);
  // Turned two steps right since the frame was drawn.
  EXPECT_EQ(MapFollow::relativeHeadingStep(8, 6), 2);
  // And two steps left, which has to wrap rather than go negative.
  EXPECT_EQ(MapFollow::relativeHeadingStep(4, 6), 14);
  EXPECT_EQ(MapFollow::relativeHeadingStep(0, 1), 15);
}

TEST(MapFollowKeepIn, EdgesAreOutside) {
  EXPECT_TRUE(MapFollow::insideKeepIn(240, 400, 480, 800));
  // Exactly on the margin counts as inside; one pixel short of it does not.
  EXPECT_TRUE(MapFollow::insideKeepIn(MapFollow::kKeepInMarginPx, MapFollow::kKeepInMarginPx, 480, 800));
  EXPECT_FALSE(MapFollow::insideKeepIn(MapFollow::kKeepInMarginPx - 1, 400, 480, 800));
  EXPECT_FALSE(MapFollow::insideKeepIn(240, MapFollow::kKeepInMarginPx - 1, 480, 800));
  EXPECT_FALSE(MapFollow::insideKeepIn(480 - MapFollow::kKeepInMarginPx, 400, 480, 800));
  EXPECT_FALSE(MapFollow::insideKeepIn(240, 800 - MapFollow::kKeepInMarginPx, 480, 800));
}

TEST(MapFollowDecide, TinyMoveTouchesNothing) {
  MapFollow::Request request = baseRequest();
  request.fixX = 240 + MapFollow::kMinMovePx - 1;
  request.fixY = 690 + MapFollow::kMinMovePx - 1;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::Skip);
}

TEST(MapFollowDecide, OneAxisPastTheFloorIsEnough) {
  MapFollow::Request request = baseRequest();
  request.fixX = 240 + MapFollow::kMinMovePx;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::MoveMarker);

  request = baseRequest();
  // Backwards counts the same as forwards: the floor is on distance, not sign.
  request.fixY = 690 - MapFollow::kMinMovePx;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::MoveMarker);
}

TEST(MapFollowDecide, LeavingTheKeepInFrameReAnchors) {
  MapFollow::Request request = baseRequest();
  request.fixY = 800 - MapFollow::kKeepInMarginPx;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::ReAnchor);
}

TEST(MapFollowDecide, HeadingDriftReAnchorsEvenStandingStill) {
  MapFollow::Request request = baseRequest();
  // Same pixel, but the rider has turned 90 degrees: the frame is oriented for
  // the road they left, so the movement floor must not swallow this.
  request.fixHeadingStep = MapFollow::kMaxHeadingDriftSteps;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::ReAnchor);

  // One step under the limit is a marker-arrow change, not a redraw -- and with
  // no movement, not even that.
  request.fixHeadingStep = MapFollow::kMaxHeadingDriftSteps - 1;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::Skip);
}

TEST(MapFollowDecide, DriftReAnchorsInEitherDirection) {
  MapFollow::Request request = baseRequest();
  request.anchorHeadingStep = 0;
  request.fixHeadingStep = static_cast<uint8_t>(16 - MapFollow::kMaxHeadingDriftSteps);
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::ReAnchor);
}

TEST(MapFollowDecide, GhostingBudgetForcesACleanFrame) {
  MapFollow::Request request = baseRequest();
  request.fixX = 240 + MapFollow::kMinMovePx;
  request.partialMoves = MapFollow::kMaxPartialMoves - 1;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::MoveMarker);

  request.partialMoves = MapFollow::kMaxPartialMoves;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::ReAnchor);
}

TEST(MapFollowDecide, GhostingBudgetBeatsTheMovementFloor) {
  // A spent budget must produce the clean frame even when the fix has barely
  // moved -- otherwise a slow rider never gets one.
  MapFollow::Request request = baseRequest();
  request.partialMoves = MapFollow::kMaxPartialMoves;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::ReAnchor);
}

TEST(MapFollowDecide, ARunOfFixesFollowsThenReAnchors) {
  // What a rider heading north actually produces: the marker walks up the
  // screen a step at a time and the map is redrawn once, when it runs out of
  // room -- not once per fix.
  MapFollow::Request request = baseRequest();
  int moves = 0;
  int reAnchors = 0;
  for (int i = 0; i < 40; ++i) {
    request.fixY = static_cast<int16_t>(request.drawnY - 16);
    const MapFollow::Action action = MapFollow::decide(request);
    if (action == MapFollow::Action::ReAnchor) {
      ++reAnchors;
      // What renderViewport() does: marker back to the ladder anchor, budget
      // reset, frame re-oriented.
      request.drawnX = 240;
      request.drawnY = 690;
      request.partialMoves = 0;
      request.anchorHeadingStep = request.fixHeadingStep;
      continue;
    }
    ASSERT_EQ(action, MapFollow::Action::MoveMarker);
    ++moves;
    request.drawnX = request.fixX;
    request.drawnY = request.fixY;
    request.partialMoves = static_cast<uint8_t>(request.partialMoves + 1);
  }
  EXPECT_GT(moves, reAnchors * 4);
  EXPECT_GT(reAnchors, 0);
}

// --- A route holding the frame (docs/route-navigation.md, "The decision") ---

TEST(MapFollowRouteFrame, HeadingNeverReAnchorsWhileTheRouteHoldsTheFrame) {
  MapFollow::Request request = baseRequest();
  request.routeHoldsFrame = true;
  // Fully reversed -- eight steps, the most a heading can drift. On a free-ride
  // frame this is the strongest possible reason to redraw; here it is no reason
  // at all, because the frame is the route's and the arrow carries the rider's.
  request.fixHeadingStep = 8;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::Skip);

  // And it must not sneak back in once the marker is also moving.
  request.fixX = 240 + MapFollow::kMinMovePx;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::MoveMarker);
}

TEST(MapFollowRouteFrame, EveryHeadingStepIsIgnored) {
  // The switchback case: the tangent swings through every step of the ring over
  // one pass. Not one of them may redraw.
  for (uint8_t step = 0; step < 16; ++step) {
    MapFollow::Request request = baseRequest();
    request.routeHoldsFrame = true;
    request.fixHeadingStep = step;
    request.fixX = 240 + MapFollow::kMinMovePx;
    EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::MoveMarker) << "step " << int(step);
  }
}

TEST(MapFollowRouteFrame, RunningOutOfFrameStillReAnchors) {
  // The one trigger the decision keeps: the rider is about to leave the picture.
  MapFollow::Request request = baseRequest();
  request.routeHoldsFrame = true;
  request.fixY = 800 - MapFollow::kKeepInMarginPx;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::ReAnchor);
}

TEST(MapFollowRouteFrame, TheBudgetIsTheRequestsToGive) {
  MapFollow::Request request = baseRequest();
  request.routeHoldsFrame = true;
  request.fixX = 240 + MapFollow::kMinMovePx;
  request.partialMoveBudget = 40;

  // Past the free-ride budget and still moving the marker: the limit is whatever
  // the caller asked for, not the constant.
  request.partialMoves = MapFollow::kMaxPartialMoves + 1;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::MoveMarker);

  request.partialMoves = 39;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::MoveMarker);
  request.partialMoves = 40;
  EXPECT_EQ(MapFollow::decide(request), MapFollow::Action::ReAnchor);
}

TEST(MapFollowRouteFrame, ASwitchbackPassDrawsOneFrame) {
  // The whole point, end to end. A marker walking across the frame while the
  // heading reverses repeatedly -- what the 503 over Baba does -- must produce
  // no redraw at all until the marker runs out of room.
  //
  // 60 fixes of 10 px up the screen is 600 px of travel from the ladder anchor
  // at y=690, so the marker reaches the keep-in edge and that, once, is allowed.
  MapFollow::Request request = baseRequest();
  request.routeHoldsFrame = true;
  request.partialMoveBudget = 200;  // deliberately not the limit under test
  const uint8_t headings[] = {12, 0, 4, 0, 12, 8, 2, 14};
  int moves = 0;
  int reAnchors = 0;
  for (int i = 0; i < 60; ++i) {
    request.fixHeadingStep = headings[i % 8];
    request.fixY = static_cast<int16_t>(request.drawnY - 10);
    const MapFollow::Action action = MapFollow::decide(request);
    if (action == MapFollow::Action::ReAnchor) {
      ++reAnchors;
      request.drawnX = 240;
      request.drawnY = 690;
      request.partialMoves = 0;
      continue;
    }
    ASSERT_EQ(action, MapFollow::Action::MoveMarker) << "fix " << i;
    ++moves;
    request.drawnY = request.fixY;
    request.partialMoves = static_cast<uint16_t>(request.partialMoves + 1);
  }
  // Every fix but the handful that ran out of screen moved the marker, and the
  // redraws that did happen are keep-in ones, not heading ones.
  EXPECT_GT(moves, 50);
  EXPECT_LE(reAnchors, 1);
}

TEST(MapFollowRouteFrame, TheSameRunWithoutTheRouteRedrawsConstantly) {
  // The control for the test above: identical fixes, routeHoldsFrame false. This
  // is what the panel actually did before the decision, and it is why the frozen
  // frame exists.
  MapFollow::Request request = baseRequest();
  const uint8_t headings[] = {12, 0, 4, 0, 12, 8, 2, 14};
  int reAnchors = 0;
  for (int i = 0; i < 60; ++i) {
    request.fixHeadingStep = headings[i % 8];
    request.fixY = static_cast<int16_t>(request.drawnY - 10);
    if (MapFollow::decide(request) == MapFollow::Action::ReAnchor) {
      ++reAnchors;
      request.drawnX = 240;
      request.drawnY = 690;
      request.partialMoves = 0;
      request.anchorHeadingStep = request.fixHeadingStep;
      continue;
    }
    request.drawnY = request.fixY;
    request.partialMoves = static_cast<uint16_t>(request.partialMoves + 1);
  }
  EXPECT_GT(reAnchors, 10);
}

}  // namespace
