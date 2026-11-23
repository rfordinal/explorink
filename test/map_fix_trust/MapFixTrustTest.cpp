#include <gtest/gtest.h>

#include "MapFixTrust.h"

using MapFixTrust::Dir;
using MapFixTrust::Pos;

namespace {

TEST(MapFixTrustHdop, NoHdopIsUnstatedAndDoesNotLatch) {
  MapFixTrust::State state;
  // GnssFix initialises hdop to 0 and only a parsed GGA sets it, so 0 is "the
  // receiver stated none" -- the same sentinel role kAccuracyUnstated plays on
  // the metre side.
  EXPECT_EQ(MapFixTrust::posTrustForHdop(0.0f, 9, state), Pos::Unstated);
  EXPECT_EQ(MapFixTrust::posTrustForHdop(9.0f, 9, state), Pos::Loose);
  EXPECT_EQ(MapFixTrust::posTrustForHdop(0.0f, 9, state), Pos::Unstated);
  EXPECT_EQ(state.pos, Pos::Loose);
}

TEST(MapFixTrustHdop, TheLineIsWhereGeometryStopsFittingUnderTheMarker) {
  MapFixTrust::State state;
  // HDOP times about 5 m of ranging error, against the same 25 m the metre
  // side calls Loose.
  EXPECT_EQ(MapFixTrust::posTrustForHdop(1.0f, 9, state), Pos::Trusted);
  EXPECT_EQ(MapFixTrust::posTrustForHdop(3.5f, 9, state), Pos::Trusted);
  EXPECT_EQ(MapFixTrust::posTrustForHdop(5.0f, 9, state), Pos::Loose);
  EXPECT_EQ(MapFixTrust::posTrustForHdop(25.5f, 9, state), Pos::Loose);
}

TEST(MapFixTrustHdop, TheDeadBandKeepsTheSideItAlreadyHas) {
  MapFixTrust::State loose;
  EXPECT_EQ(MapFixTrust::posTrustForHdop(6.0f, 9, loose), Pos::Loose);
  // Inside 3.5..5 with Loose already chosen, it stays Loose -- coming back
  // costs crossing the whole band, so a fix hovering at the line does not
  // repaint the marker every second.
  EXPECT_EQ(MapFixTrust::posTrustForHdop(4.0f, 9, loose), Pos::Loose);
  EXPECT_EQ(MapFixTrust::posTrustForHdop(3.5f, 9, loose), Pos::Trusted);

  MapFixTrust::State fresh;
  // A first stated fix inside the band has no side to keep, and starting Loose
  // would make the alarm the default.
  EXPECT_EQ(MapFixTrust::posTrustForHdop(4.0f, 9, fresh), Pos::Trusted);
}

TEST(MapFixTrustHdop, ThreeSatellitesIsLooseWhateverTheGeometrySays) {
  MapFixTrust::State state;
  // A 2D fix assumed an altitude, and that assumption is wrong by whatever the
  // ground has done since. Excellent HDOP does not rescue it.
  EXPECT_EQ(MapFixTrust::posTrustForHdop(0.9f, 3, state), Pos::Loose);
  // It latches, unlike Unstated: this is a stated bad fix, not a missing one.
  EXPECT_EQ(state.pos, Pos::Loose);
  EXPECT_EQ(MapFixTrust::posTrustForHdop(0.9f, 4, state), Pos::Trusted);
}

TEST(MapFixTrustPos, UnstatedIsItsOwnAnswerAndDoesNotLatch) {
  MapFixTrust::State state;
  // No figure at all is not "bad" -- it is the state an old client leaves, and
  // it must draw the marker that client has always seen.
  EXPECT_EQ(MapFixTrust::posTrustFor(MapFixTrust::kAccuracyUnstated, state), Pos::Unstated);

  // A stated bad fix latches Loose...
  EXPECT_EQ(MapFixTrust::posTrustFor(80, state), Pos::Loose);
  // ...and a following fix with no figure must not report Loose. It reports
  // that nobody said, and leaves the latch alone for the next stated fix.
  EXPECT_EQ(MapFixTrust::posTrustFor(MapFixTrust::kAccuracyUnstated, state), Pos::Unstated);
  EXPECT_EQ(state.pos, Pos::Loose);
}

TEST(MapFixTrustPos, TheLineIsWhereTheErrorStopsFittingUnderTheMarker) {
  MapFixTrust::State state;
  // 27 px of ring radius against 1 m per pixel at the finest rung, rounded
  // down to 25. Under it the true position is somewhere beneath the glyph.
  EXPECT_EQ(MapFixTrust::posTrustFor(5, state), Pos::Trusted);
  EXPECT_EQ(MapFixTrust::posTrustFor(24, state), Pos::Trusted);
  EXPECT_EQ(MapFixTrust::posTrustFor(25, state), Pos::Loose);
}

TEST(MapFixTrustPos, HysteresisStopsAFixOnTheLineRepaintingThePanel) {
  MapFixTrust::State state;
  EXPECT_EQ(MapFixTrust::posTrustFor(30, state), Pos::Loose);
  // Inside the band: every one of these would be a ~500 ms windowed refresh if
  // the verdict flipped, with the rider standing still.
  EXPECT_EQ(MapFixTrust::posTrustFor(24, state), Pos::Loose);
  EXPECT_EQ(MapFixTrust::posTrustFor(19, state), Pos::Loose);
  // Past the far edge it finally comes back.
  EXPECT_EQ(MapFixTrust::posTrustFor(18, state), Pos::Trusted);
  EXPECT_EQ(MapFixTrust::posTrustFor(24, state), Pos::Trusted);
  EXPECT_EQ(MapFixTrust::posTrustFor(25, state), Pos::Loose);
}

TEST(MapFixTrustPos, FirstStatedFixInsideTheBandStartsTrusted) {
  MapFixTrust::State state;
  // Whole band is under the marker at the finest rung, so starting on the
  // alarm side would make the alarm the default.
  EXPECT_EQ(MapFixTrust::posTrustFor(20, state), Pos::Trusted);
}

TEST(MapFixTrustWire, ZeroMeansUnstatedSoAnOldClientIsUnchanged) {
  // The whole back-compatibility argument is this one line: a phone that
  // writes no bits lands on Unstated, which draws the marker it always drew.
  EXPECT_EQ(MapFixTrust::dirTrustFromWireCode(0), Dir::Unstated);
  EXPECT_EQ(MapFixTrust::dirTrustFromWireCode(1), Dir::Good);
  EXPECT_EQ(MapFixTrust::dirTrustFromWireCode(2), Dir::Coarse);
  EXPECT_EQ(MapFixTrust::dirTrustFromWireCode(3), Dir::Unknown);
}

TEST(MapFixTrustWire, RoundTripsAndIgnoresBitsThatAreNotItsOwn) {
  for (uint8_t code = 0; code < 4; ++code) {
    EXPECT_EQ(MapFixTrust::wireCodeForDirTrust(MapFixTrust::dirTrustFromWireCode(code)), code);
  }
  // Callers shift the flags byte down before handing it over; a stray high bit
  // must not invent a fifth state.
  EXPECT_EQ(MapFixTrust::dirTrustFromWireCode(0xFE), Dir::Coarse);
}

TEST(MapFixTrustStyle, UnstatedAndTrustedDrawTheSamePicture) {
  const MapFixTrust::MarkerStyle unstated = MapFixTrust::styleFor({Pos::Unstated, Dir::Unstated});
  const MapFixTrust::MarkerStyle trusted = MapFixTrust::styleFor({Pos::Trusted, Dir::Good});
  EXPECT_FALSE(unstated.ringBroken);
  EXPECT_FALSE(trusted.ringBroken);
  EXPECT_EQ(unstated.head, MapFixTrust::MarkerStyle::Head::Glyph);
  EXPECT_EQ(trusted.head, MapFixTrust::MarkerStyle::Head::Glyph);
}

TEST(MapFixTrustStyle, TheTwoChannelsAreIndependent) {
  // A good heading with a bad position, and the reverse, both have to be
  // drawable -- they are different sensors' answers and neither implies the
  // other.
  const MapFixTrust::MarkerStyle looseButAimed = MapFixTrust::styleFor({Pos::Loose, Dir::Good});
  EXPECT_TRUE(looseButAimed.ringBroken);
  EXPECT_EQ(looseButAimed.head, MapFixTrust::MarkerStyle::Head::Glyph);

  const MapFixTrust::MarkerStyle placedButLost = MapFixTrust::styleFor({Pos::Trusted, Dir::Unknown});
  EXPECT_FALSE(placedButLost.ringBroken);
  EXPECT_EQ(placedButLost.head, MapFixTrust::MarkerStyle::Head::None);

  const MapFixTrust::MarkerStyle vague = MapFixTrust::styleFor({Pos::Loose, Dir::Coarse});
  EXPECT_TRUE(vague.ringBroken);
  EXPECT_EQ(vague.head, MapFixTrust::MarkerStyle::Head::Wedge);
}

}  // namespace
