#pragma once

#include "MapMarkerShape.generated.h"
#include "MapRideMode.h"
#include "MapStyle.h"
#include "MapViewport.h"

// The position marker's dimensions, per zoom rung.
//
// Split out of MapActivity.cpp 2026-08-12, when the marker stopped being one
// fixed size: MapActivity.h needs the type for its own accessor, and the host
// tests need the arithmetic without pulling in the activity.
//
// The full-size numbers (ring, hike dot, hand half-width, cycle/ride tip
// length, halo margin) moved out of this file and into data/mapstyle.json's
// `layers.marker` on <date> -- see MapStyle.h's markerRingPx and friends --
// so a maintainer can retune "hike's marker is too big" from the style file
// mapbuilder/tools/style_watch.py already watches, instead of editing this
// header and rebuilding. markerMetricsFor() below scales whatever the style
// says by the current rung exactly the way it always scaled the old
// constants; only where the numbers come from changed.

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
// make it harder to recognise. Also deliberately NOT in mapstyle.json: unlike
// the live marker, nothing about its size needs tuning per mode or rung, and it
// is one fixed shape judged once on the glass (below), not a knob.
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
// exactly half the live marker's full-size 54, and the dot keeps Hike's dot:ring
// ratio of 1/3.
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
// margin: the halo's job is punching a hole in the map ink, and that does not
// get easier on a smaller marker.
constexpr int kSleepMarkerHalo = 5;

// The marker, at one rung's scale. Every length the marker draws with, so that
// nothing reads a style field directly and quietly ignores the scale.
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
  int cycleTipLen;
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

// `style` is the already-resolved MapStyle for the mode and rung being drawn
// (MapStyleTable.h's mapStyleFor()) -- its markerRingPx/markerHikeDotPx/etc.
// are data/mapstyle.json's `layers.marker`, patched by any `when` that
// matched. `scale8` is that rung's own MapViewport::ZoomStep::markerScale8.
constexpr MarkerMetrics markerMetricsFor(const MapStyle& style, uint8_t scale8) {
  MarkerMetrics m{};
  m.ring = markerScaled(style.markerRingPx, scale8);
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
  // Same for the hand's half-width. **Not style-driven** even though the full
  // size is: this 2 is a rendering floor, not a look a mode should be able to
  // pick, so mapstyle.json's ring_width_px only ever reaches the rung-0..2
  // branch below.
  m.ringWidth = scale8 >= 8 ? style.markerRingWidthPx : 2;
  m.hikeDot = markerScaled(style.markerHikeDotPx, scale8);
  m.hikeHandReach = m.ring / 2 - m.ringWidth;
  m.hikeHandHalfW = style.markerHikeHandHalfWidthPx;
  m.ringGap = m.ringWidth * 2 + 2;
  m.cycleTipLen = markerScaled(style.markerCycleTipLenPx, scale8);
  m.rideTipLen = markerScaled(style.markerRideTipLenPx, scale8);
  m.haloMargin = markerScaled(style.markerHaloMarginPx, scale8);
  m.box = m.ring + 2 * m.haloMargin;
  return m;
}

// The patch buffer's fixed size (MapActivity.cpp's kMarkerPatchBytes) -- a
// real RAM ceiling, not derived from whatever mapstyle.json currently says, so
// that a style edit can never silently grow the buffer it is checked against.
// 64 because that is what the marker's full size (ring 54 + halo margin 5 on
// each side) has always come to; raising it costs RAM and is a firmware change
// of its own, not a style edit. markerFitsEveryStyleVariant() below is what
// makes this a promise rather than a hope: it is checked, at compile time,
// against every (mode, rung) combination data/mapstyle.json can actually
// produce.
constexpr int kMarkerBoxSize = 64;

// Every (mode, rung) combination the compiled style table can produce must fit
// inside kMarkerBoxSize, or a move smears a trail across the map (the patch
// save/restore only ever covers that box) -- and past a certain size it would
// overrun the fixed-size patch buffer itself. This used to be true by
// construction, because scaling only ever shrinks a single global full size.
// Since 2026-09-19 the full size is data (`layers.marker`, with `when`
// overrides per mode), so a maintainer could set hike_dot_px past what fits --
// this loop is what turns that into a build failure naming the exact
// (mode, rung) instead of a runtime memory corruption on the device.
//
// Only rung 0-2's scale8 (8, the ladder's max -- MapViewport.h's kZoomLadder)
// can produce the biggest box for a given mode, since markerScale8 is
// monotonically non-increasing along the ladder; every rung is still checked
// rather than assuming that, because a future ladder edit should not have to
// remember this proof.
constexpr bool markerFitsEveryStyleVariant() {
  for (uint8_t mode = 0; mode < kMapRideModeCount; ++mode) {
    for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
      const MapStyle& style = kMapStyleVariants[kMapStyleIndex[mode][step]];
      const MarkerMetrics m = markerMetricsFor(style, MapViewport::kZoomLadder[step].markerScale8);
      if (m.box > kMarkerBoxSize) return false;
      // hikeHandReach is checked for every mode, not only Hike: it is also
      // the uncertainty wedge's reach (MapFixTrust::MarkerStyle::Head::Wedge),
      // which any mode can be asked to draw -- its two edges run out to
      // hikeHandReach in the directions one heading step either side of the
      // fix, so every vertex sits on the same circle the hand's tip does.
      if (m.hikeHandReach + m.hikeHandHalfW > m.box / 2) return false;
      // Cycle/Ride's heading arrow, by contrast, is read only when THAT mode
      // is the one being drawn (MapActivity::drawPositionMarker's mode
      // branches never reach the arrow for Hike) -- so cycleTipLen is only
      // ever combined with Cycle's own box, never Hike's or Ride's. Checking
      // it against every mode's box the way hikeHandReach is checked would be
      // wrong now that box varies per mode (`layers.marker`'s `when`): a
      // hiker's `hike` block shrinking ring/halo_margin must not fail the
      // build over a ride/cycle tip length it will never draw at that size.
      //
      // Its farthest vertex (kMarkerArrowMaxReachPermille, from
      // marker-ride.svg) scaled by tipLen must stay inside the patch box, or a
      // move leaves it behind on the map.
      if (mode == static_cast<uint8_t>(MapRideMode::Cycle) &&
          m.cycleTipLen * kMarkerArrowMaxReachPermille / 1000 > m.box / 2) {
        return false;
      }
      if (mode == static_cast<uint8_t>(MapRideMode::Ride) &&
          m.rideTipLen * kMarkerArrowMaxReachPermille / 1000 > m.box / 2) {
        return false;
      }
    }
  }
  return true;
}
static_assert(markerFitsEveryStyleVariant(),
              "the marker (ring, hand or arrow) does not fit the patch box at every mode and rung data/"
              "mapstyle.json's layers.marker can produce -- shrink the offending style field, or raise "
              "kMarkerBoxSize and say what RAM it costs");
