#pragma once

#include "MapMarkerShape.generated.h"
#include "MapViewport.h"

// The position marker's dimensions, per zoom rung.
//
// Split out of MapActivity.cpp 2026-08-12, when the marker stopped being one
// fixed size: MapActivity.h needs the type for its own accessor, and the host
// tests need the arithmetic without pulling in the activity.
//
// Full size, at MapViewport::ZoomStep::markerScale8 == 8. Every number below is
// scaled from these by markerMetricsFor(), so the marker is one shape drawn at
// three sizes rather than three shapes.
constexpr int kMarkerRingDiameter = 54;
constexpr int kMarkerRingWidth = 3;
constexpr int kMarkerHikeDotDiameter = 18;
// Hike's heading hand: from the dot out to the ring's inner edge
// (kMarkerRingDiameter / 2 - kMarkerRingWidth = 24), 4 px wide. The reach must
// stay inside the patch box's half-extent or a marker move leaves the hand
// behind on the map -- see the note where it is drawn.
constexpr int kMarkerHikeHandReach = kMarkerRingDiameter / 2 - kMarkerRingWidth;
constexpr int kMarkerHikeHandHalfW = 2;
// Tourist/Ride draw a heading arrow whose whole shape (base width, waist notch,
// shoulders -- see MapMarkerShape.generated.h) is a fixed ratio of this one
// center-to-tip length, generated from src/components/icons/marker-ride.svg
// by scripts/gen_marker_shape.py. Only the tip length is a free knob per mode.
// Still the arrow cycle used, now drawn for tourist. OPEN, 2026-10-27: a
// walker's heading comes from a phone in a pocket and an arrow is a claim
// about direction that hike deliberately refuses to make (see the dot above).
// Left unchanged with the rename because a marker is judged on the panel and
// nothing has been judged yet -- changing it blind would trade one unargued
// shape for another.
constexpr int kMarkerTouristTipLen = 16;
constexpr int kMarkerRideTipLen = 25;
constexpr int kMarkerHaloMargin = 5;  // white backing, past the ring's own radius

// A ring that is not claiming to know where the rider is gets broken into arcs
// instead of drawn whole (MapFixTrust::MarkerStyle::ringBroken). Eight gaps,
// punched as white squares on the ring at every other heading step.
//
// **A count, not a length.** Fixing the count and letting the gap scale with the
// rung keeps the shape reading as "a broken ring" at every size; fixing the gap
// length in pixels would leave the coarse rungs looking like a whole ring with a
// nick in it, which is the one thing this must not be mistaken for.
//
// Eight because it is every other entry of kMarkerHeadingDir, so the positions
// cost no trig at all -- the same 16-step table the heading glyph already
// indexes.
constexpr int kMarkerRingGapCount = 8;

// The sleep marker: what replaces the live marker on the way into a quick-resume
// sleep (MapActivity::drawSleepMarker). Deliberately NOT scaled by rung -- it is
// not tracking anything any more, so a size that varies with zoom would only
// make it harder to recognise.
//
// Shape is Hike's minus the heading hand: ring plus centre dot. That shape is
// the point. It says "this is where you were" and, unlike every live marker,
// says nothing about which way you face -- which on a sleeping device would be a
// claim about the past dressed as the present. The white halo is what makes it
// findable at this size: it punches a hole in the map ink underneath.
//
// Sizes were judged on the glass, not calculated. First pass was ring 18 / dot 6
// / halo 3; on the panel that read as findable but too small, and the maintainer
// asked for half again, so these are those scaled by 3/2 (2026-08-19). Ring 27 is
// exactly half the live marker's 54, and the dot keeps Hike's dot:ring ratio of
// 1/3.
//
// What makes it findable is the shape staying recognisable, not its area: the
// white halo punches a hole in the map ink, and small enough, the ring stroke and
// the dot read as one blob. Per CLAUDE.md a laptop PNG is the wrong medium for
// this call, so it goes on the panel and gets looked at.
constexpr int kSleepMarkerRing = 27;
// Does not scale with the ring. It is 2 px because the live marker's 3 px is
// what this shape must NOT be mistaken for, not because 2 px is any kind of
// limit -- see markerMetricsFor() for why the old "stroke floor" claim here was
// wrong.
constexpr int kSleepMarkerRingWidth = 2;
constexpr int kSleepMarkerDot = 9;
// 4.5 rounded up, which also lands on the live marker's own full-scale halo
// (kMarkerHaloMargin): the halo's job is punching a hole in the map ink, and that
// does not get easier on a smaller marker.
constexpr int kSleepMarkerHalo = 5;

// The marker, at one rung's scale. Every length the marker draws with, so that
// nothing reads a full-size constant directly and quietly ignores the scale.
//
// Why the marker shrinks at all is in MapViewport::ZoomStep::markerScale8: it
// is a fixed pixel object over ground that shrinks under it, and at 45 m/px the
// full-size ring covers 2.4 km of map.
//
// What it does *not* buy is a cheaper refresh. Measured on the X4 2026-08-05: a
// windowed refresh costs the same 500 ms whatever its area (see moveMarker()).
// A smaller marker saves the framebuffer read-back and write-back either side
// of it, which is memcpy, not waveform.
struct MarkerMetrics {
  int ring;
  int ringWidth;
  int hikeDot;
  int hikeHandReach;
  int hikeHandHalfW;
  // Side of the white square punched at each of kMarkerRingGapCount points to
  // break the ring. Derived from the stroke rather than chosen: a gap has to be
  // wider than the line it is cutting or it reads as a bruise on the ring
  // instead of a hole through it.
  int ringGap;
  int touristTipLen;
  int rideTipLen;
  int haloMargin;
  // The marker's halo box: the unit of every partial operation. Everything the
  // marker can draw is inside it (the halo is the outermost thing
  // drawPositionMarker() paints), so saving this box before the marker goes
  // down and writing it back afterwards erases the marker exactly.
  int box;
};

// A length scaled to a rung, never below 1: a stroke or a half-width that
// rounds to 0 would silently stop drawing at the coarse rungs.
constexpr int markerScaled(int fullSize, uint8_t scale8) {
  const int scaled = fullSize * static_cast<int>(scale8) / 8;
  return scaled > 0 ? scaled : 1;
}

constexpr MarkerMetrics markerMetricsFor(uint8_t scale8) {
  MarkerMetrics m{};
  m.ring = markerScaled(kMarkerRingDiameter, scale8);
  // Strokes do not scale with the shape, and the two values below are a
  // **choice that has never been judged on a panel**, not a limit.
  //
  // The comment that stood here until 2026-09-05 said 3 px was "near the
  // thinnest line that survives on this panel at arm's length" and that 2 px
  // was "the floor". Both are wrong, and nothing ever measured them. The map
  // itself is the counter-evidence: the road style floors a visible class at
  // **1 px** (docs/map-style.md, "floors a visible class at 1 px"), tertiary
  // and unclassified roads draw as 1 px hairlines, and a 1 px line across a
  // bend of the Morava was spotted on the panel on 2026-08-08. If 1 px were
  // under the panel's floor, half the road network would be invisible.
  //
  // The claim most likely slid in from toneWayInterior's real 2 px floor, which
  // is about **dither fill**, not strokes: a 1 px dither reads as a dashed line
  // and a dash already means water (docs/map-style.md).
  //
  // So this stays 3/2 for now because that is what the panel has been showing,
  // not because thinner was ruled out. T-259 puts 1, 2 and 3 px rings side by
  // side on the glass; a thinner ring at the coarse rungs would put less ink
  // over the map exactly where the marker starts hiding what it points at.
  // Same for the hand's half-width.
  m.ringWidth = scale8 >= 8 ? kMarkerRingWidth : 2;
  m.hikeDot = markerScaled(kMarkerHikeDotDiameter, scale8);
  m.hikeHandReach = m.ring / 2 - m.ringWidth;
  m.hikeHandHalfW = kMarkerHikeHandHalfW;
  m.ringGap = m.ringWidth * 2 + 2;
  m.touristTipLen = markerScaled(kMarkerTouristTipLen, scale8);
  m.rideTipLen = markerScaled(kMarkerRideTipLen, scale8);
  m.haloMargin = markerScaled(kMarkerHaloMargin, scale8);
  m.box = m.ring + 2 * m.haloMargin;
  return m;
}

// The biggest the marker ever is: what the patch buffer has to hold. Rung 0's
// scale, not a separate number, so a table edit cannot outgrow the buffer.
constexpr MarkerMetrics kMarkerMetricsFull = markerMetricsFor(8);
constexpr int kMarkerBoxSize = kMarkerMetricsFull.box;  // 64

// The hike hand is the only marker part whose reach is a free parameter, so it is
// the one that can be pushed out of the saved patch box. Past that box a move does
// not restore what the hand covered and it smears a trail across the map. Checked
// at every scale the ladder actually uses, because the hand is derived from the
// ring and the halo is not.
constexpr bool markerHandFitsAtEveryRung() {
  for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
    const MarkerMetrics m = markerMetricsFor(MapViewport::kZoomLadder[step].markerScale8);
    if (m.hikeHandReach + m.hikeHandHalfW > m.box / 2) return false;
    if (m.box > kMarkerBoxSize) return false;
  }
  return true;
}
static_assert(markerHandFitsAtEveryRung(),
              "hike heading hand must stay inside the marker patch box at every rung, or a move smears it");

// The uncertainty wedge (MapFixTrust::MarkerStyle::Head::Wedge) needs no bound
// of its own: its two edges run out to hikeHandReach in the directions one
// heading step either side of the fix, so every vertex sits on the same circle
// the hand's tip does and the assert above already covers it. Using the heading
// table for those two directions is what makes that true -- an arbitrary angle
// would not land on that circle in integer arithmetic.

// Same bound, for Tourist/Ride's heading arrow: its farthest vertex
// (kMarkerArrowMaxReachPermille, from marker-ride.svg) scaled by tipLen must
// stay inside the patch box too, or a move leaves it behind on the map.
constexpr bool markerArrowFitsAtEveryRung() {
  for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
    const MarkerMetrics m = markerMetricsFor(MapViewport::kZoomLadder[step].markerScale8);
    if (m.touristTipLen * kMarkerArrowMaxReachPermille / 1000 > m.box / 2) return false;
    if (m.rideTipLen * kMarkerArrowMaxReachPermille / 1000 > m.box / 2) return false;
  }
  return true;
}
static_assert(markerArrowFitsAtEveryRung(),
              "cycle/ride heading arrow must stay inside the marker patch box at every rung, or a move smears it");
