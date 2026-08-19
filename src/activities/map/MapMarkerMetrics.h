#pragma once

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
constexpr int kMarkerCycleTipLen = 16;    // center to tip, pixels
constexpr int kMarkerCycleBaseHalfW = 9;  // center to each base corner, pixels
constexpr int kMarkerRideTipLen = 25;
constexpr int kMarkerRideBaseHalfW = 18;
constexpr int kMarkerHaloMargin = 5;  // white backing, past the ring's own radius

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
// Sizes are a judgement call, not a measurement. Ring is 18 against the live
// marker's 54, so a third of it, and the dot keeps Hike's dot:ring ratio (1/3).
// Below roughly this the ring's 2 px stroke and the dot start reading as one
// blob on this panel and the shape stops being recognisable -- which is the
// thing that makes it findable, not its area. Judged on the glass, per
// CLAUDE.md: a laptop PNG is the wrong medium for this call.
constexpr int kSleepMarkerRing = 18;
constexpr int kSleepMarkerRingWidth = 2;  // 2 px is the panel's stroke floor, see markerMetricsFor()
constexpr int kSleepMarkerDot = 6;
constexpr int kSleepMarkerHalo = 3;

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
  int cycleTipLen;
  int cycleBaseHalfW;
  int rideTipLen;
  int rideBaseHalfW;
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
  // Strokes do not scale with the shape: a 3 px ring is already near the
  // thinnest line that survives on this panel at arm's length, and 2 px is the
  // floor. Same for the hand's half-width.
  m.ringWidth = scale8 >= 8 ? kMarkerRingWidth : 2;
  m.hikeDot = markerScaled(kMarkerHikeDotDiameter, scale8);
  m.hikeHandReach = m.ring / 2 - m.ringWidth;
  m.hikeHandHalfW = kMarkerHikeHandHalfW;
  m.cycleTipLen = markerScaled(kMarkerCycleTipLen, scale8);
  m.cycleBaseHalfW = markerScaled(kMarkerCycleBaseHalfW, scale8);
  m.rideTipLen = markerScaled(kMarkerRideTipLen, scale8);
  m.rideBaseHalfW = markerScaled(kMarkerRideBaseHalfW, scale8);
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
