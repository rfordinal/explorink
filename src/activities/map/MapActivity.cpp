#include "MapActivity.h"

#include <BlePositionServer.h>
#include <I18n.h>
#include <Memory.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "GfxRendererCanvas.h"
#include "LastHeldTiles.h"
#include "MapFollow.h"
#include "MapHatch.h"
#include "MapMarkerMetrics.h"
#include "MapPowerStatsProvider.h"
#include "MapRenderer.h"
#include "MapRouteFit.h"
#include "MapStyleDefaults.h"
#include "MapViewport.h"
#include "MissingTilesConsoleSource.h"
#include "MissingTilesStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

namespace {

constexpr const char* kLogTag = "MAP";

// The clock MapRenderer's optional per-layer timing calls. A plain function
// pointer, because MapRenderer is compiled for the host too and cannot see
// millis() (MapRenderer.h, MapRenderTiming).
uint32_t renderClockMs() { return millis(); }

// Microseconds, for the card-time accounting: one 4 KB read is well under a
// millisecond, so a millisecond clock would round most reads to zero
// (MapTileReader::takeIoUs).
uint32_t cardClockUs() { return micros(); }

// docs/map-data-spec.md, "Layers as separate files".
constexpr const char* kTileRoot = "/trailink";

// How long the buttons must be quiet before the step that was landed on is
// rendered. docs/map-data-spec.md puts it at about half a second: long
// enough that three quick presses are one refresh, short enough that a
// single press does not feel dropped.
constexpr uint32_t kButtonSettleMs = 500;

// And how long before the step is written to the card. Deliberately well
// past the redraw, which itself takes the better part of two seconds: the
// save must never be the thing standing between a press and the picture.
constexpr uint32_t kSaveSettleMs = 4000;

// MissingTilesStore's own, much longer interval -- a coverage gap can keep
// producing new missing tiles for minutes at a stretch, and this is a rate
// cap, not a settle delay: at most one SD write per this many milliseconds,
// counted from the first new tile since the last flush (renderViewport()).
constexpr uint32_t kMissingTilesSaveIntervalMs = 10 * 60 * 1000;

// Autosync's rate cap: at most one `NEED_TILES` per this many milliseconds,
// counted from the ask that went out, not pushed further by more hatching.
// A rider crossing a coverage gap re-hatches on every viewport reset, and a
// reset can be seconds apart -- without a cap that is a stream of asks for a
// list that has barely changed.
constexpr uint32_t kAutoSyncIntervalMs = 60 * 1000;

// How long an ask may go **completely quiet** before the device gives up on it.
// Not a budget for the whole fetch: expireAutoSync() rearms this every time the
// receiver's byte counters move, so a slow transfer stays alive and only real
// silence -- a phone out of range, an app closed mid-push -- ends the ask.
//
// A flat whole-fetch budget was tried first and was wrong on the panel: 395 KB
// at 2.6 kB/s is 147 s for one tile, so three minutes expired mid-transfer
// (measured 2026-08-07).
constexpr uint32_t kAutoSyncQuietMs = 45 * 1000;

// Live freshness: how often the device may ask whether the tiles under the
// screen have been republished. Far longer than the autosync cap, because the
// answer changes when somebody rebuilds an area -- hours or weeks apart -- not
// when the rider moves. Ten minutes is already generous against that.
constexpr uint32_t kFreshnessIntervalMs = 10 * 60 * 1000;

// How long a `CHECK_TILES` may go unanswered before the device stops waiting.
// A check is one `have` reply and a handful of kB of HTTP; a phone that has not
// finished in this long is not going to.
constexpr uint32_t kFreshnessQuietMs = 40 * 1000;

// Rate cap on maybeCheckTileFreshness()'s own gate-reason logging -- that
// function runs every tick, so without this a blocked gate prints on every
// loop() instead of at a readable rate on the serial monitor.
constexpr uint32_t kFreshnessGateLogThrottleMs = 30 * 1000;

// How long after the last arrival the map is redrawn. A settle timer, not a
// rate cap: arrivals come in bursts and the frame worth spending is the one
// after the last tile, so each arrival pushes this out.
constexpr uint32_t kArrivalRedrawSettleMs = 5000;

// How often the header status row's state is looked at (updateHeaderStatus()).
// Bounds the rate of rssi() calls into the NimBLE host, and is still prompt for
// a link that has dropped.
constexpr uint32_t kHeaderPollMs = 2000;

// And the floor between two repaints caused by nothing but a moving bar count.
// RSSI sitting on a threshold flips the count back and forth, and each flip is
// a real waveform pass on e-ink. A link appearing or dropping ignores this --
// that one is not cosmetic.
constexpr uint32_t kHeaderBarsRepaintMs = 30 * 1000;

// Stateless view onto MISSING_TILES for the `missing` command, shared with the
// tile sync screen (MissingTilesConsoleSource.h).
MissingTilesConsoleSource g_missingTilesConsoleSource;

// Busy badge geometry: an hourglass just above the button hints, right-aligned.
// Out of the way of the debug readout (top-left) and the compass (top-right),
// and small enough that its windowed refresh is cheap to look at.
constexpr int kBusySize = 34;
constexpr int kBusyMarginRight = 10;
constexpr int kBusyMarginBottom = 50;  // clears GUI.drawButtonHints' band
constexpr int kBusyBorder = 2;
constexpr int kBusyGlassInset = 8;  // hourglass inset inside the badge box

// Scale bar geometry: five alternating black/white segments, bottom-left.
// kScaleMarginBottom is the same clearance line kBusyMarginBottom uses --
// both stacks bottom out flush with each other, one per side, clear of
// GUI.drawButtonHints' band.
// Right-edge width the theme's side-button hints occupy. Theme-owned geometry
// (GUI.drawSideButtonHints, BaseTheme's x4ButtonPositions), so this is a
// measurement rather than a shared constant: read off a panel screenshot
// 2026-08-12, the hint box spans the last ~26 px of the row. Rounded up, and
// used only to keep place labels out of that column -- map geometry still draws
// under it and gets painted over (GfxRendererCanvas).
constexpr int kSideHintReservedPx = 30;

constexpr int kScaleMarginLeft = 12;
constexpr int kScaleMarginBottom = 50;
constexpr int kScaleBarHeight = 6;
constexpr int kScaleTickHeight = 4;  // tick overshoot above/below the bar
constexpr int kScaleSegments = 5;
// Desired bar width before rounding down to a nice ground distance -- the
// actual width is whatever niceScaleValue() picks divided by mpp, which is
// always <= this.
constexpr int kScaleTargetPx = 130;
constexpr double kScaleNiceMultipliers[3] = {1.0, 2.0, 5.0};

// Largest value of the form {1, 2, 5} x 10^k not exceeding `raw` -- the
// rounding every classic map scale bar uses so its marks land on numbers a
// rider can subtract in their head.
double niceScaleValue(double raw) {
  if (raw < 1.0) return 1.0;
  double best = 1.0;
  double decade = 1.0;
  for (int k = 0; k < 6; ++k) {
    for (double mult : kScaleNiceMultipliers) {
      const double candidate = mult * decade;
      if (candidate <= raw) best = candidate;
    }
    decade *= 10.0;
  }
  return best;
}

// One decimal place only when `value` is not a whole number -- with
// kScaleSegments = 5 over the {1,2,5}x10^k sequence, that is only the two
// smallest km totals (1 km, 2 km go to 0.2 km steps); every other total
// (>= 5 km, or anything shown in metres) divides into whole numbers.
void formatScaleMark(double value, char* buf, size_t bufSize) {
  const long tenths = lround(value * 10.0);
  if (tenths % 10 == 0) {
    snprintf(buf, bufSize, "%ld", tenths / 10);
  } else {
    snprintf(buf, bufSize, "%ld.%ld", tenths / 10, tenths % 10);
  }
}

// Header status row: battery, BLE signal bars, a small Bluetooth logo, top
// right, above the compass. Battery is drawn through GUI.drawHeader() --
// BaseTheme.cpp:363 -- the same call every other screen makes, so its icon
// position (screenWidth - kHeaderMarginRight - batteryWidth) and text-left
// layout match this device's info-list screens exactly. The BLE half is this
// screen's own: no other activity has a wireless link worth showing, and no
// wifi radio exists on this device for the bars to ever mean anything else.
constexpr int kHeaderMarginTop = 6;
constexpr int kHeaderMarginRight = 12;  // matches BaseTheme::drawHeader's own literal
constexpr int kHeaderIconHeight = 14;
constexpr int kHeaderBleBarCount = 4;
constexpr int kHeaderBleBarWidth = 4;
constexpr int kHeaderBleBarGap = 2;
constexpr int kHeaderBleBarsWidth =
    kHeaderBleBarCount * kHeaderBleBarWidth + (kHeaderBleBarCount - 1) * kHeaderBleBarGap;
constexpr int kHeaderBtLogoWidth = 6;
constexpr int kHeaderBtToBarsGap = 4;
// The globe, left of the Bluetooth logo: data is moving over the link right
// now. A circle with an equator and a meridian -- at 14 px an ellipse for the
// meridian is one pixel wide either side of the centre line and reads as
// noise, so it is a straight line.
//
// The device has no radio that reaches the internet. The globe is honest
// anyway, and about the thing that matters to the rider: the phone is spending
// mobile data on their behalf.
constexpr int kHeaderGlobeDiameter = kHeaderIconHeight;
constexpr int kHeaderGlobeToBtGap = 6;
constexpr int kHeaderGroupGap = 10;  // BLE group to battery block, and logo to bars
// GUI.drawHeader() only clears its own 80px-wide battery box (BaseTheme.cpp:366);
// the BLE logo+bars sit further left, over live map lines like the compass and
// marker do, so they need the same opaque backing those give themselves.
constexpr int kHeaderBackingPad = 2;
// The full-width strip GUI.drawHeader() clears for the row above (its own
// Rect{0, kHeaderMarginTop, screenWidth, ...} in drawHeaderStatus()).
constexpr int kHeaderRowHeight = BaseMetrics::values.batteryHeight + 10;

// Debug readout geometry. Starts below the header status row above (battery,
// BLE bars, globe), not stuck at the top of the screen sharing its band --
// a debug line starting inside [kHeaderMarginTop, kHeaderMarginTop +
// kHeaderRowHeight) reads as glued to the status row instead of sitting
// under it, even though the two never overlap horizontally.
constexpr int kTextX = 8;
constexpr int kTextGapBelowHeader = 14;
constexpr int kTextTopY = kHeaderMarginTop + kHeaderRowHeight + kTextGapBelowHeader;
// Line-to-line spacing is derived from the font's own line height at each
// call site (renderer.getLineHeight(), not a hardcoded pixel count) --
// ubuntu_10_regular/bold's advanceY is 24 (EpdFontData: advanceY, ascender,
// descender = 24, 20, -4), so a fixed 18px gap left each line's backing box
// (drawDebugLine's own line height + 2*kDebugPad tall) overlapping the box
// above it, erasing the bottom few pixels of that line's text.
constexpr int kDebugPad = 3;

// Extra clearance below the whole header row (battery box and BLE strip
// alike): both of those clear-rects end right at the icon's own edge, so a
// map line (a road) drawn immediately below reads as touching the icon.
constexpr int kHeaderExtraMargin = 2;

// The header is now a fixed-height contract, not a set of independent
// clear-rects the map happens to get painted over: a single white strip,
// [0, kHeaderBarHeight), a 1px black separator at its bottom edge, and the
// map's own content starting only at kMapContentTop --
// GfxRendererCanvas's minY clips it there, so nothing above the line is
// drawn at all, not drawn-then-covered (docs/map-header-status.md).
//
// kHeaderBarHeight mirrors drawHeaderStatus()'s own former per-element
// clear-bottom math (kHeaderMarginTop + 5 + kHeaderRowHeight, the +5 being
// BaseTheme's own internal battery-rect offset) plus the same
// kHeaderExtraMargin breathing room it already used below the icons, plus
// 1 -- so the icon cluster's layout needs no retuning: everything it already
// draws (battery bottom ~28px, BLE strip backing bottom ~31px) fits inside
// with room to spare.
constexpr int kHeaderBarHeight = kHeaderMarginTop + 5 + kHeaderRowHeight + kHeaderExtraMargin + 1;  // 36
constexpr int kHeaderSeparatorY = kHeaderBarHeight;                                                 // the 1px black row
constexpr int kMapContentTop = kHeaderBarHeight + 1;  // first row the map may draw into
constexpr int kHeaderPlaceNameRightGap = 6;           // clearance before the icon cluster's own backing
// 2px past kTextX -- confirmed on hardware 2026-08-11 that the debug
// readout's own left margin read as too tight for this text specifically.
constexpr int kHeaderPlaceNameLeftX = kTextX + 2;

// North indicator geometry, top-right corner. Ported 1:1 (scale 1
// design-unit = 1 pixel) from the user's exact vector spec (2026-08-05): a
// 100x100 normalized canvas with "N" label, two open arcs (real angles, not
// GfxRenderer::drawArc's axis-aligned quadrants -- those can only start/end
// on 90 degree boundaries and a naive mirrored pair of half-circles meets
// into a closed ring, which is not this glyph), a plain solid center
// triangle (no concave cutout -- this spec dropped that from an earlier
// draft) and a small separate accent triangle.
//
// The map is drawn track-up, so the glyph rotates: the whole thing turns about
// the arc centre by the frame's heading, which puts its point at true north
// instead of up the screen. That makes the arc centre the anchor -- the glyph
// sweeps a circle around it, not a fixed bounding box -- and the halo a disc.
//
// Shrunk to 0.75x and moved down on 2026-08-06: the original 46+5 halo (102px
// across) left too much white margin once the header row above needed room.
// Every design-space point below is the original scaled by 0.75 around the
// arc centre (50,47), which stays fixed -- it is the rotation pivot, not a
// point being scaled. kCompassCenterTop clears the header row (bottom at
// kHeaderMarginTop + 23 -- drawHeaderStatus()'s batteryIconTop comment has the
// real battery-icon offset this counts from) plus an 8px gap plus the halo
// (radius 39): 6 + 23 + 8 + 40 = 77, then nudged another 10px down
// (2026-08-08) for more air above the compass.
constexpr int kCompassCenterMarginRight = 56;  // arc centre, in from the right edge
constexpr int kCompassCenterTop = 87;          // arc centre, down from the top edge
// Design-space distance from the arc centre to the furthest thing drawn: the
// "N" glyph's top stroke sits at y=16, 31 above the arc centre (47), so 36
// clears it with a few px of slack. The accent triangle's tip (~21) and the
// arcs (18) are well inside.
constexpr int kCompassGlyphRadius = 36;
constexpr int kCompassLabelLeftX = 46, kCompassLabelRightX = 54;
constexpr int kCompassLabelTopY = 16, kCompassLabelBottomY = 28;
constexpr int kCompassLabelStrokeWidth = 2;
constexpr int kCompassArcCx = 50, kCompassArcCy = 47;
constexpr int kCompassArcRadius = 18;
constexpr float kCompassLeftArcStartDeg = 130.0f, kCompassLeftArcEndDeg = 230.0f;
constexpr float kCompassRightArcStartDeg = 310.0f, kCompassRightArcEndDeg = 360.0f + 50.0f;
constexpr int kCompassArcSegments = 12;  // straight segments approximating each curve
constexpr int kCompassArcLineWidth = 2;
constexpr int kCompassTriTopX = 50, kCompassTriTopY = 33;
constexpr int kCompassTriLeftX = 44, kCompassTriLeftY = 61;
constexpr int kCompassTriRightX = 56, kCompassTriRightY = 61;
constexpr int kCompassAccentX1 = 66, kCompassAccentY1 = 45;
constexpr int kCompassAccentX2 = 66, kCompassAccentY2 = 51;
constexpr int kCompassAccentX3 = 71, kCompassAccentY3 = 48;
constexpr int kCompassHaloMargin = 3;  // white backing, past the glyph's own sweep (was 4, shrunk ~25%)

// Position marker: one family, three modes, "the higher the speed, the more
// directional" -- hike is a plain dot (position over direction), cycle is a
// small arrow (both matter), ride is a large arrow that fills the ring
// (direction over position). All three share the same outline ring.
// Worst-case bytes for that box in panel memory. readFramebufferRegion snaps
// the x extent outward to a multiple of 8, so a 64 px wide box can need 72 px
// (9 bytes) of columns; the +8 rows are slack against the same rounding after
// an orientation rotate. 720 bytes, allocated once -- a full frame is 48,000,
// and re-reading the tiles under the marker instead would cost an SD read and a
// MapRenderer pass, which is the cost this whole path exists to avoid.
constexpr size_t kMarkerPatchBytes = ((kMarkerBoxSize + 16) / 8) * (kMarkerBoxSize + 8);

// Same 16-step direction table as MapRenderer.cpp's kHeadingDir (dx/dy unit
// vectors scaled by 8, avoiding any per-frame trig) -- duplicated rather than
// shared because this marker is drawn straight through GfxRenderer, not
// IMapCanvas (see drawCompass() above: it needs no map data, so it does not
// belong in MapRenderer's pull-from-IMapSource pipeline). Keep the two in
// step if either changes; MapHeading's ordering is what pins the index.
struct HeadingVec {
  int dx, dy;
};
constexpr HeadingVec kMarkerHeadingDir[16] = {
    {0, -8},   // N
    {3, -7},   // NNE
    {6, -6},   // NE
    {7, -3},   // ENE
    {8, 0},    // E
    {7, 3},    // ESE
    {6, 6},    // SE
    {3, 7},    // SSE
    {0, 8},    // S
    {-3, 7},   // SSW
    {-6, 6},   // SW
    {-7, 3},   // WSW
    {-8, 0},   // W
    {-7, -3},  // WNW
    {-6, -6},  // NW
    {-3, -7},  // NNW
};

// Display strings for MapRideMode, indexed the same way as the enum itself
// (MapRideMode.h) -- mapRideModeName() gives the wire name for the console,
// this gives the translated label for the Mode popup.
constexpr StrId kMapModeIds[kMapRideModeCount] = {StrId::STR_RIDE, StrId::STR_HIKE, StrId::STR_CYCLE};

}  // namespace

void MapActivity::drawPositionMarker(int cx, int cy, uint8_t headingStep, MapRideMode mode) {
  // Sized for the rung on the panel right now -- MapViewport::ZoomStep::
  // markerScale8. markerRect() reads the same metrics, so the patch box the
  // move path saves always matches what this paints.
  const MarkerMetrics m = markerMetrics();
  // The box every partial operation is sized by from here on: recorded at the
  // moment the marker is painted, so markerRect() erases exactly what was drawn
  // even if the rung changed in between.
  markerBoxDrawn_ = static_cast<int16_t>(m.box);
  const int radius = m.ring / 2;
  // White halo first: the ring is only a 2px stroke, so without this the
  // map lines it sits over would show straight through its interior, and a
  // busy junction under the ring reads as clutter, not a marker.
  const int haloRadius = radius + m.haloMargin;
  renderer.fillRoundedRect(cx - haloRadius, cy - haloRadius, haloRadius * 2, haloRadius * 2, haloRadius, Color::White);
  renderer.drawRoundedRect(cx - radius, cy - radius, m.ring, m.ring, m.ringWidth, radius, true);

  if (mode == MapRideMode::Hike) {
    // Position over direction, but not direction *nowhere*: a dot for where the
    // hiker is, and a thin hand off it for which way they face -- a watch hand
    // against the ring's bezel, not a second arrow.
    //
    // The dot alone was right while the map turned track-up, because then the
    // whole picture carried the heading. With a route holding the frame the map
    // no longer turns (docs/route-navigation.md, "The decision"), so without this
    // a hiker would have no heading on screen at all.
    //
    // Drawn *before* the dot on purpose: the dot then covers the inner end, so
    // the hand grows out of a solid boss instead of meeting it at a seam.
    //
    // The hand's reach is the ring's inner edge, which keeps the whole hand
    // inside the box saveMarkerPatch() stores (MarkerMetrics::box). Anything
    // drawn past that box is not restored when the marker moves and smears a
    // trail across the map -- the reach is a correctness bound, not a style
    // choice.
    const HeadingVec& hand = kMarkerHeadingDir[headingStep < 16 ? headingStep : 0];
    const HeadingVec handPerp{-hand.dy, hand.dx};
    const int tipX = cx + hand.dx * m.hikeHandReach / 8;
    const int tipY = cy + hand.dy * m.hikeHandReach / 8;
    const int hx[4] = {
        cx + handPerp.dx * m.hikeHandHalfW / 8,
        tipX + handPerp.dx * m.hikeHandHalfW / 8,
        tipX - handPerp.dx * m.hikeHandHalfW / 8,
        cx - handPerp.dx * m.hikeHandHalfW / 8,
    };
    const int hy[4] = {
        cy + handPerp.dy * m.hikeHandHalfW / 8,
        tipY + handPerp.dy * m.hikeHandHalfW / 8,
        tipY - handPerp.dy * m.hikeHandHalfW / 8,
        cy - handPerp.dy * m.hikeHandHalfW / 8,
    };
    renderer.fillPolygon(hx, hy, 4, true);

    renderer.fillRoundedRect(cx - m.hikeDot / 2, cy - m.hikeDot / 2, m.hikeDot, m.hikeDot, m.hikeDot / 2, Color::Black);
    return;
  }

  // Cycle and ride both point at the real incoming heading, never the
  // forced-north display heading renderViewport() uses for map rotation
  // (kNoRouteDisplayHeading) -- direction of travel matters at riding speed
  // even though the map underneath stays north-up.
  const int tipLen = mode == MapRideMode::Ride ? m.rideTipLen : m.cycleTipLen;
  const int baseHalfW = mode == MapRideMode::Ride ? m.rideBaseHalfW : m.cycleBaseHalfW;
  const HeadingVec& dir = kMarkerHeadingDir[headingStep < 16 ? headingStep : 0];
  const HeadingVec perp{-dir.dy, dir.dx};

  const int tipX = cx + dir.dx * tipLen / 8;
  const int tipY = cy + dir.dy * tipLen / 8;
  const int baseCx = cx - dir.dx * (tipLen / 2) / 8;
  const int baseCy = cy - dir.dy * (tipLen / 2) / 8;
  const int baseLeftX = baseCx + perp.dx * baseHalfW / 8;
  const int baseLeftY = baseCy + perp.dy * baseHalfW / 8;
  const int baseRightX = baseCx - perp.dx * baseHalfW / 8;
  const int baseRightY = baseCy - perp.dy * baseHalfW / 8;

  const int xs[3] = {tipX, baseLeftX, baseRightX};
  const int ys[3] = {tipY, baseLeftY, baseRightY};
  renderer.fillPolygon(xs, ys, 3, true);
}

namespace {

// One design-space point, rotated about the arc centre and dropped on screen.
//
// The map is track-up, so a bearing b appears on screen at (b - heading) from
// straight up: north (b = 0) sits at -heading, and the glyph -- drawn pointing
// north in design space -- has to turn by that much. Screen y is down, so a
// visually clockwise turn by angle a maps (dx, dy) to (dx cos a - dy sin a,
// dx sin a + dy cos a); with a = -theta that is the pair below. Callers pass
// cos/sin of theta so the trig is paid once per frame, not per point.
void compassPoint(int centreX, int centreY, int designX, int designY, float cosTheta, float sinTheta, int& outX,
                  int& outY) {
  const float dx = static_cast<float>(designX - kCompassArcCx);
  const float dy = static_cast<float>(designY - kCompassArcCy);
  outX = centreX + static_cast<int>(std::lround(dx * cosTheta + dy * sinTheta));
  outY = centreY + static_cast<int>(std::lround(-dx * sinTheta + dy * cosTheta));
}

// One open arc, start to end degrees, drawn as kCompassArcSegments straight
// chords -- GfxRenderer::drawArc only draws axis-aligned quarter-circles, and
// this glyph's arcs start/end at arbitrary angles (130/230, 310/50), so they
// have to be sampled by hand. Canvas angle convention: 0 = +x (right), 90 =
// +y (down), matching plain cos/sin with no axis flip since screen y is
// already down.
void drawCompassArc(GfxRenderer& renderer, int cx, int cy, int radius, float startDeg, float endDeg, int lineWidth) {
  constexpr float kDegToRad = 3.14159265f / 180.0f;
  int prevX = cx + static_cast<int>(std::lround(radius * std::cos(startDeg * kDegToRad)));
  int prevY = cy + static_cast<int>(std::lround(radius * std::sin(startDeg * kDegToRad)));
  for (int i = 1; i <= kCompassArcSegments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kCompassArcSegments);
    const float deg = startDeg + (endDeg - startDeg) * t;
    const int x = cx + static_cast<int>(std::lround(radius * std::cos(deg * kDegToRad)));
    const int y = cy + static_cast<int>(std::lround(radius * std::sin(deg * kDegToRad)));
    renderer.drawLine(prevX, prevY, x, y, lineWidth, true);
    prevX = x;
    prevY = y;
  }
}

// Static thresholds, no hysteresis: unlike the WiFi indicator's
// barsForRssi() (CrossPointWebServerActivity.cpp:53), this header redraws
// only on a map redraw (not continuously), so there is no per-frame flicker
// for hysteresis to guard against. Same dBm bands as that WiFi indicator --
// BLE and WiFi share the 2.4 GHz band, so the same signal-to-bars mapping
// reads right for both.
int bleBarsForRssi(int8_t rssi) {
  static constexpr int kThresholdsDbm[] = {-85, -75, -65, -55};
  int bars = 0;
  for (int threshold : kThresholdsDbm) {
    if (rssi >= threshold) ++bars;
  }
  return bars;
}

}  // namespace

void MapActivity::busyRect(int& x, int& y, int& w, int& h) const {
  w = kBusySize;
  h = kBusySize;
  x = renderer.getScreenWidth() - kBusyMarginRight - w;
  y = renderer.getScreenHeight() - kBusyMarginBottom - h;
}

// White box, black border, black hourglass: two triangles meeting at a waist.
// Deliberately not text -- it needs no translation and stays legible at 34 px.
void MapActivity::drawBusyBadge() {
  int x, y, w, h;
  busyRect(x, y, w, h);

  // Opaque, like the compass halo: this lands on live map lines, not margin.
  renderer.fillRect(x, y, w, h, false);
  renderer.drawRect(x, y, w, h, kBusyBorder, true);

  const int left = x + kBusyGlassInset;
  const int right = x + w - kBusyGlassInset;
  const int top = y + kBusyGlassInset;
  const int bottom = y + h - kBusyGlassInset;
  const int midX = (left + right) / 2;
  const int midY = (top + bottom) / 2;

  const int upperX[3] = {left, right, midX};
  const int upperY[3] = {top, top, midY};
  renderer.fillPolygon(upperX, upperY, 3, true);
  const int lowerX[3] = {left, right, midX};
  const int lowerY[3] = {bottom, bottom, midY};
  renderer.fillPolygon(lowerX, lowerY, 3, true);
}

// Ground distance the bar's five segments and the marks under them stand
// for, at the current zoom step's mpp (MapViewport::kZoomLadder). Bottom-left,
// the one corner GUI.drawButtonHints() leaves clear on this side of the
// screen -- its box row starts at x=25 (BaseTheme.cpp's x4ButtonPositions),
// so the bar sits left of that, and kScaleMarginBottom mirrors the busy
// badge's own margin to clear the same band from above.
void MapActivity::drawMapScale() {
  const double mpp = MapViewport::kZoomLadder[zoomStep()].mpp;
  const double niceMeters = niceScaleValue(kScaleTargetPx * mpp);
  const int totalPx = static_cast<int>(lround(niceMeters / mpp));
  const bool useKm = niceMeters >= 1000.0;

  // Stacked bottom-up from the same clearance line the busy badge uses
  // (kScaleMarginBottom, mirroring kBusyMarginBottom): label text bottom
  // lands exactly there, with the ticks and bar above it -- not below, or
  // the labels would draw into GUI.drawButtonHints' band.
  const int clearanceY = renderer.getScreenHeight() - kScaleMarginBottom;
  const int labelY = clearanceY - renderer.getLineHeight(UI_10_FONT_ID);
  const int tickBottom = labelY - 2;
  const int barBottom = tickBottom - kScaleTickHeight;
  const int barTop = barBottom - kScaleBarHeight;
  const int tickTop = barTop - kScaleTickHeight;

  // Edges computed from the same total rather than a fixed per-segment
  // pixel width, so five segments always add up to exactly totalPx with no
  // rounding gap or overlap at the last one.
  int edgeX[kScaleSegments + 1];
  for (int i = 0; i <= kScaleSegments; ++i) {
    edgeX[i] = kScaleMarginLeft + static_cast<int>(lround(static_cast<double>(i) * totalPx / kScaleSegments));
  }

  for (int i = 0; i < kScaleSegments; ++i) {
    const bool black = (i % 2) == 0;
    renderer.fillRect(edgeX[i], barTop, edgeX[i + 1] - edgeX[i], kScaleBarHeight, black);
  }
  renderer.drawRect(edgeX[0], barTop, totalPx, kScaleBarHeight, true);

  for (int i = 0; i <= kScaleSegments; ++i) {
    renderer.drawLine(edgeX[i], tickTop, edgeX[i], tickBottom, 1, true);
  }

  // Labels, built and measured before any is drawn: a narrow segment at a
  // loose zoom rung can make adjacent numbers wider than the gap between
  // their ticks -- confirmed on hardware 2026-08-11 (CMD:SCREENSHOT at
  // mpp=20), the marks ran together into unreadable overlap. 0 and the final
  // mark always draw -- they are the two numbers a rider actually needs, the
  // endpoints of what the bar covers. An interior mark draws only if it
  // clears both its drawn neighbour to the left and the final label's own
  // left edge; otherwise its tick stays but its number is dropped rather
  // than smeared into the next one.
  char marks[kScaleSegments + 1][16];
  int textX[kScaleSegments + 1];
  int textWidths[kScaleSegments + 1];
  for (int i = 0; i <= kScaleSegments; ++i) {
    if (i == kScaleSegments) {
      char value[12];
      formatScaleMark(useKm ? niceMeters / 1000.0 : niceMeters, value, sizeof(value));
      snprintf(marks[i], sizeof(marks[i]), "%s %s", value, useKm ? "km" : "m");
    } else {
      const double markValue = static_cast<double>(i) * niceMeters / kScaleSegments;
      formatScaleMark(useKm ? markValue / 1000.0 : markValue, marks[i], sizeof(marks[i]));
    }
    textWidths[i] = renderer.getTextWidth(UI_10_FONT_ID, marks[i]);
    textX[i] = edgeX[i] - textWidths[i] / 2;
  }
  // The two ends stay inside the bar's own span, not centred on their tick --
  // "0" cannot draw left of the margin and the final mark cannot run past
  // the bar's right edge.
  textX[0] = edgeX[0];
  textX[kScaleSegments] = edgeX[kScaleSegments] - textWidths[kScaleSegments];

  constexpr int kLabelGap = 3;
  renderer.drawText(UI_10_FONT_ID, textX[0], labelY, marks[0], true);
  int lastLabelRight = textX[0] + textWidths[0];
  for (int i = 1; i < kScaleSegments; ++i) {
    const bool clearsPrev = textX[i] >= lastLabelRight + kLabelGap;
    const bool clearsFinal = textX[i] + textWidths[i] + kLabelGap <= textX[kScaleSegments];
    if (!clearsPrev || !clearsFinal) continue;  // tick stays; number would overlap a neighbour
    renderer.drawText(UI_10_FONT_ID, textX[i], labelY, marks[i], true);
    lastLabelRight = textX[i] + textWidths[i];
  }
  renderer.drawText(UI_10_FONT_ID, textX[kScaleSegments], labelY, marks[kScaleSegments], true);
}

void MapActivity::showBusy() {
  // One badge per burst. Three quick zoom presses are one redraw, so they must
  // also be one refresh -- the badge from the first press is still on screen
  // and says the same thing.
  if (busyShown_) return;

  int x, y, w, h;
  busyRect(x, y, w, h);
  drawBusyBadge();
  // Windowed: the rest of the panel keeps the map that is already on it. A
  // full refresh here would cost the same waveform time and throw the picture
  // away twice.
  if (!renderer.displayBufferWindow(x, y, w, h)) {
    LOG_ERR(kLogTag, "busy badge window rejected: %d,%d %dx%d", x, y, w, h);
    return;
  }
  busyShown_ = true;
}

void MapActivity::maybeAutoSyncTiles() {
  // Cleared unconditionally: this is a snapshot of one frame's hatching, and a
  // want kept across minutes would fire an ask for tiles the rider has since
  // ridden away from. The next frame over a gap hatches again and asks again --
  // there is nothing to remember.
  const uint32_t want = autoSyncWantCount_;
  autoSyncWantCount_ = 0;
  if (want == 0) return;

  // Stale tiles are NOT counted here, and that is the design rather than an
  // omission. A stale tile is one the phone found by reading the index, so the
  // phone already knows which tile and which content id to fetch -- it pushes
  // it unasked, on the transfer channel this screen already accepts pushes on
  // (docs/tile-freshness.md, ../../docs/ble-map-transfer-protocol.md). Asking
  // for it again would mean the device relaying back a list the phone wrote.

  if (SETTINGS.mapAutoSyncTiles == 0) return;
  // One ask at a time. A second ask while the first is still being answered
  // would double-count the settle arithmetic and ask for tiles already on the
  // wire.
  if (autoSyncPending_ > 0) return;

  const uint32_t now = millis();
  if (autoSyncNextAskMs_ != 0 && now < autoSyncNextAskMs_) return;

  // Nobody to ask. Not an error and not worth a line on screen: the rider
  // turned this on and rode out of range of their own phone, which is the
  // normal end of a ride.
  if (!freeink::BlePositionServer::getInstance().isCommandSubscribed()) return;

  askForViewportTiles(want);
}

void MapActivity::askForViewportTiles(uint32_t count) {
  // Unsolicited indication on the command channel, the same mechanism
  // TileSyncActivity::askForTiles() uses. Its return value is not evidence a
  // phone heard it -- NimBLE accepts a line into its one-slot queue with
  // nobody subscribed -- which is why the caller checks isCommandSubscribed()
  // first and why the ask expires (expireAutoSync()).
  //
  // `view` is what separates this from the sync screen's ask: answer from
  // `tiles`, the tiles on screen right now, not the whole `missing` list.
  char line[48];
  snprintf(line, sizeof(line), "NEED_TILES %lu fmt %u view", static_cast<unsigned long>(count),
           static_cast<unsigned>(MapTileReader::kFormatVersion));
  if (!freeink::BlePositionServer::getInstance().sendCommandReply(line)) {
    LOG_ERR(kLogTag, "autosync: NEED_TILES not delivered");
    return;
  }

  const uint32_t now = millis();
  autoSyncPending_ = count;
  autoSyncArrived_ = false;
  autoSyncDeadlineMs_ = now + kAutoSyncQuietMs;
  // The baseline the quiet timer measures against. Taken now, so a transfer
  // already in flight for something else does not read as this ask's progress.
  lastTransferProgress_ = transfer_.status().completedBytes + transfer_.status().received;
  // From the ask, not from when it settles: the cap is on how often the device
  // may start a conversation, and a slow transfer already blocks the next ask
  // through autoSyncPending_.
  autoSyncNextAskMs_ = now + kAutoSyncIntervalMs;
  LOG_INF(kLogTag, "autosync: asked for %lu tiles on screen", static_cast<unsigned long>(count));
}

void MapActivity::drainTransferredTiles() {
  const MapTransferReceiver::Status transfer = transfer_.status();
  if (!transfer.lastTileValid || transfer.tileSeq == lastClearedTileSeq_) return;
  lastClearedTileSeq_ = transfer.tileSeq;

  // Before the missing-list check below, because a stale tile was never on that
  // list: it opened fine, so nothing ever hatched it. This is also what arms
  // the ping-pong guard -- a tile reported stale again after arriving here is
  // one the fetch did not fix, and StaleTilesList gives up on it.
  const bool wasStale = staleTiles_.contains(transfer.lastTile.z, transfer.lastTile.col, transfer.lastTile.row);
  if (wasStale) {
    staleTiles_.onArrived(transfer.lastTile.z, transfer.lastTile.col, transfer.lastTile.row);
    LOG_INF(kLogTag, "freshness: z%u %lu/%lu replaced", static_cast<unsigned>(transfer.lastTile.z),
            static_cast<unsigned long>(transfer.lastTile.col), static_cast<unsigned long>(transfer.lastTile.row));
    // Every arrival owes a redraw -- the panel is showing the old tile.
    arrivalRedrawDueMs_ = millis() + kArrivalRedrawSettleMs;
    if (autoSyncPending_ > 0) {
      --autoSyncPending_;
      autoSyncArrived_ = true;
      if (autoSyncPending_ == 0) autoSyncDeadlineMs_ = 0;
    }
    return;
  }

  if (!MISSING_TILES.forget(transfer.lastTile.z, transfer.lastTile.col, transfer.lastTile.row)) {
    // A tile the device never hatched -- a corridor pushed ahead of a ride,
    // say. Nothing to clear, nothing this screen asked for, and not an error.
    return;
  }
  LOG_INF(kLogTag, "z%u %lu/%lu arrived, dropped from the list", static_cast<unsigned>(transfer.lastTile.z),
          static_cast<unsigned long>(transfer.lastTile.col), static_cast<unsigned long>(transfer.lastTile.row));

  // **Every arrival owes a redraw, whether or not an ask is outstanding.** The
  // panel is hatching a square the card now holds, which is the one thing hatch
  // must never mean. This used to be tied to an ask settling, so a tile that
  // arrived outside one -- pushed by hand, or landing after the ask had already
  // expired -- was filed away and never drawn. Seen on the panel 2026-08-07: a
  // 395 KB tile finished after its ask timed out, and the map kept the hatch.
  //
  // Coalesced on its own, longer settle rather than the button one: arrivals
  // come in bursts, each redraw is the better part of two seconds of waveform,
  // and the redraw that matters is the one after the last tile. Each arrival
  // pushes the deadline out, so a burst costs one frame, not one per tile.
  arrivalRedrawDueMs_ = millis() + kArrivalRedrawSettleMs;

  if (autoSyncPending_ == 0) return;
  --autoSyncPending_;
  autoSyncArrived_ = true;
  if (autoSyncPending_ > 0) return;

  autoSyncDeadlineMs_ = 0;
  LOG_INF(kLogTag, "autosync: settled");
}

void MapActivity::onTileSkipped(uint8_t z, uint32_t col, uint32_t row) {
  // A skip for a stale tile means the phone could not vouch for the replacement
  // -- no source, or a download that still did not match. Give up on it rather
  // than leave it on the list to be asked for on the next drain.
  if (staleTiles_.contains(z, col, row)) staleTiles_.giveUp(z, col, row);
  // The supplier does not have this tile. Remembered, so the next frame over
  // the same gap does not ask for it again -- see the class comment.
  // millis() here, not inside the store: the refusal schedule is the store's
  // (MissingTilesStore::refusalDelayMs), the clock is the firmware's.
  MISSING_TILES.markRefused(z, col, row, millis());
  LOG_DBG(kLogTag, "autosync: z%u %lu/%lu refused, not asking again", static_cast<unsigned>(z),
          static_cast<unsigned long>(col), static_cast<unsigned long>(row));

  if (autoSyncPending_ == 0) return;
  --autoSyncPending_;
  if (autoSyncPending_ > 0) return;

  autoSyncDeadlineMs_ = 0;
  // No redraw scheduled here even when tiles did land: an arrival already owes
  // one through arrivalRedrawDueMs_. A run where every answer was `skip` owes
  // nothing at all -- the panel's hatch is still the right picture, and a full
  // frame is two seconds of waveform for no pixel change.
}

void MapActivity::onTileStale(uint8_t z, uint32_t col, uint32_t row) {
  // Recorded only. The ask goes out from loop() through maybeAutoSyncTiles(),
  // where the rate cap and the link state live -- a BLE conversation must not
  // start from inside a console drain.
  if (staleTiles_.add(z, col, row)) {
    LOG_INF(kLogTag, "freshness: z%u %lu/%lu is out of date", static_cast<unsigned>(z), static_cast<unsigned long>(col),
            static_cast<unsigned long>(row));
    return;
  }
  // add() refused it: already listed, already given up on, or reported stale a
  // second time after a fetch that did not fix it. The last case is the one
  // worth a line -- it is the loop guard doing its job.
  if (staleTiles_.hasGivenUp(z, col, row)) {
    LOG_INF(kLogTag, "freshness: z%u %lu/%lu still wrong after a fetch, not asking again", static_cast<unsigned>(z),
            static_cast<unsigned long>(col), static_cast<unsigned long>(row));
  }
}

void MapActivity::onCheckFinished(bool known, uint16_t staleCount) {
  freshnessPending_ = false;
  freshnessDeadlineMs_ = 0;
  if (!known) {
    // The phone could not read the index. **Not the same as zero stale tiles.**
    // Nothing is claimed, nothing is marked, and the next check is the ordinary
    // cooldown away -- the phone backs itself off harder than that, so there is
    // no second timer needed here.
    LOG_INF(kLogTag, "freshness: phone could not check (no index)");
    return;
  }
  LOG_INF(kLogTag, "freshness: %u tile(s) out of date", static_cast<unsigned>(staleCount));
}

void MapActivity::maybeCheckTileFreshness() {
  if (SETTINGS.mapTileFreshnessMode != CrossPointSettings::MAP_TILE_FRESHNESS_LIVE) return;
  if (freshnessPending_) {
    if (freshnessDeadlineMs_ != 0 && millis() >= freshnessDeadlineMs_) {
      LOG_INF(kLogTag, "freshness: unanswered, giving up on this check");
      freshnessPending_ = false;
      freshnessDeadlineMs_ = 0;
    }
    return;
  }

  const uint32_t now = millis();
  // One line per gate, not one line at the end, so a silent device says which
  // condition is holding it rather than just staying silent -- found needing
  // this while chasing a Live-mode device that never asked for 12+ minutes
  // with every gate looking satisfied from the code alone. Throttled: this
  // function runs every tick, and a blocked gate would otherwise print on
  // every loop() instead of at a readable rate.
  const bool logGate = now - freshnessLastGateLogMs_ >= kFreshnessGateLogThrottleMs;
  if (freshnessNextAskMs_ != 0 && now < freshnessNextAskMs_) {
    if (logGate) {
      LOG_DBG(kLogTag, "freshness: cooling down, %lu ms left", static_cast<unsigned long>(freshnessNextAskMs_ - now));
      freshnessLastGateLogMs_ = now;
    }
    return;
  }
  // Nothing drawn yet means nothing to ask about.
  if (heldTiles_.count == 0) {
    if (logGate) {
      LOG_DBG(kLogTag, "freshness: no tiles held, nothing to ask about");
      freshnessLastGateLogMs_ = now;
    }
    return;
  }
  // A transfer already in flight owns the link. Asking now would put a `have`
  // reply's worth of indications into the middle of somebody's tile.
  if (autoSyncPending_ > 0) {
    if (logGate) {
      LOG_DBG(kLogTag, "freshness: autosync has %lu tile(s) in flight, waiting",
              static_cast<unsigned long>(autoSyncPending_));
      freshnessLastGateLogMs_ = now;
    }
    return;
  }
  if (!freeink::BlePositionServer::getInstance().isCommandSubscribed()) {
    if (logGate) {
      LOG_DBG(kLogTag, "freshness: no phone subscribed to the command channel");
      freshnessLastGateLogMs_ = now;
    }
    return;
  }

  // `fmt <version>` so the phone can pick the matching index tree without
  // needing a NEED_TILES to have told it first -- found needing this the hard
  // way: a device with nothing missing never sends NEED_TILES at all, so
  // FreshnessChecker's format defaulted to CdnTileSource's stale constant and
  // compared against the wrong /v<N>/ index tree, one version behind, for
  // every check.
  char line[48];
  snprintf(line, sizeof(line), "CHECK_TILES %lu fmt %u", static_cast<unsigned long>(heldTiles_.count),
           static_cast<unsigned>(MapTileReader::kFormatVersion));
  if (!freeink::BlePositionServer::getInstance().sendCommandReply(line)) {
    LOG_ERR(kLogTag, "freshness: CHECK_TILES not delivered");
    return;
  }
  freshnessPending_ = true;
  freshnessDeadlineMs_ = now + kFreshnessQuietMs;
  // From the ask, not from the answer: the cap is on how often the device may
  // start a conversation.
  freshnessNextAskMs_ = now + kFreshnessIntervalMs;
  LOG_INF(kLogTag, "freshness: asked about %lu tile(s) on screen", static_cast<unsigned long>(heldTiles_.count));
}

void MapActivity::expireAutoSync() {
  if (autoSyncPending_ == 0 || autoSyncDeadlineMs_ == 0) return;

  // **The deadline is on silence, not on the whole fetch.** Bytes still moving
  // means the phone is answering, however slowly, and cutting it off there
  // throws away everything already transferred.
  //
  // The first version put a flat three minutes on the whole ask, which a single
  // tile can exceed on its own: measured 2026-08-07, 395 KB at 2.6 kB/s took
  // 147 s, and detail tiles twice that size exist. It expired mid-transfer, and
  // the tile that landed afterwards had no ask left to settle.
  const uint32_t progress = transfer_.status().completedBytes + transfer_.status().received;
  if (progress != lastTransferProgress_) {
    lastTransferProgress_ = progress;
    autoSyncDeadlineMs_ = millis() + kAutoSyncQuietMs;
    return;
  }

  if (millis() < autoSyncDeadlineMs_) return;

  LOG_INF(kLogTag, "autosync: %lu tiles unanswered for %lus, giving up", static_cast<unsigned long>(autoSyncPending_),
          static_cast<unsigned long>(kAutoSyncQuietMs / 1000));
  autoSyncPending_ = 0;
  autoSyncDeadlineMs_ = 0;
}

void MapActivity::updateHeaderStatus() {
  // Nothing to update before there is a frame to update: the waiting banner
  // draws no header row at all, and painting one onto it would leave a floating
  // status row over a screen with no map.
  if (!viewportDrawn_) return;

  // Polled, not checked per tick: rssi() is a NimBLE host call
  // (ble_gap_conn_rssi), and asking it hundreds of times a second to answer a
  // question about a 14 px icon is not a trade worth making. Half a hertz is
  // still prompt for a link that dropped.
  const uint32_t now = millis();
  if (nextHeaderPollMs_ != 0 && now < nextHeaderPollMs_) return;
  nextHeaderPollMs_ = now + kHeaderPollMs;

  auto& ble = freeink::BlePositionServer::getInstance();
  const bool globe = autoSyncPending_ > 0;
  // Same test drawHeaderStatusStrip() uses, and the comment there says why it is
  // the interval rather than the MTU.
  const bool connected = ble.connIntervalMs() != 0;
  const int bars = connected ? bleBarsForRssi(ble.rssi()) : 0;

  // Two classes of change, and they earn different urgency.
  //
  // A link appearing or dropping, or a transfer starting or ending, changes
  // what the row *means* -- repaint at once. This is the one that was missing:
  // the row was drawn only by a full frame, so closing the phone's GPS app left
  // the bars on the panel until something else forced a redraw. Reported from a
  // real session, 2026-08-07.
  const bool structural = globe != transferIconShown_ || connected != drawnLinkConnected_;
  // A bar count moving while the link holds is the same story told slightly
  // differently, and RSSI sitting on a threshold flips it back and forth.
  // Every flip is a real waveform pass, so it is rate-capped.
  const bool barsMoved = connected && bars != drawnBleBars_;

  if (!structural && !barsMoved) return;
  if (!structural && now < nextBarsRepaintMs_) return;
  if (barsMoved) nextBarsRepaintMs_ = now + kHeaderBarsRepaintMs;

  int x, y, w, h;
  headerStatusRect(x, y, w, h);
  // The strip alone, not drawHeaderStatus(): that one also redraws the battery
  // block, which sits outside this window. Drawing outside what is refreshed
  // puts a battery in the framebuffer that the panel will not show until the
  // next full frame -- and if the charge has moved since, the two disagree.
  drawHeaderStatusStrip();  // records what it drew, for the comparisons above
  // Windowed, like the busy badge: the map on the rest of the panel is
  // untouched and a full refresh would cost a second and throw it away twice.
  if (!renderer.displayBufferWindow(x, y, w, h)) {
    LOG_ERR(kLogTag, "header status window rejected: %d,%d %dx%d", x, y, w, h);
  }
}

void MapActivity::drawCompass(uint8_t headingStep) {
  const int centreX = renderer.getScreenWidth() - kCompassCenterMarginRight;
  const int centreY = kCompassCenterTop;
  // One sin/cos pair per frame, same rule as MapProjection::reset() -- the
  // per-point maths below is plain arithmetic on these two.
  constexpr float kDegToRad = 3.14159265f / 180.0f;
  const float thetaDeg = static_cast<float>(headingStep & 0x0F) * 22.5f;
  const float cosTheta = std::cos(thetaDeg * kDegToRad);
  const float sinTheta = std::sin(thetaDeg * kDegToRad);

  // White halo first, for the same reason the marker gets one: this sits over
  // live map lines, not blank margin. A disc, not a box -- the glyph rotates
  // inside it, so the clearance has to be the same in every direction or the
  // backing would clip the label at some headings and not others.
  const int haloRadius = kCompassGlyphRadius + kCompassHaloMargin;
  renderer.fillRoundedRect(centreX - haloRadius, centreY - haloRadius, haloRadius * 2, haloRadius * 2, haloRadius,
                           Color::White);

  // 1. "N", drawn as a 3-stroke monoline glyph -- not a font. GfxRenderer's
  // text renderer only has two orientations (TextRotation::None and a fixed
  // Rotated90CW, GfxRenderer.cpp:301), neither of which tracks an arbitrary
  // heading, so a font glyph here can only ever sit upright. The letter is
  // simple enough (two verticals and a diagonal) to draw as three lines
  // instead, rotated the same way as the triangle and accent below -- so the
  // glyph itself turns with the bezel, not just its position.
  int leftTopX, leftTopY, leftBottomX, leftBottomY;
  int rightTopX, rightTopY, rightBottomX, rightBottomY;
  compassPoint(centreX, centreY, kCompassLabelLeftX, kCompassLabelTopY, cosTheta, sinTheta, leftTopX, leftTopY);
  compassPoint(centreX, centreY, kCompassLabelLeftX, kCompassLabelBottomY, cosTheta, sinTheta, leftBottomX,
               leftBottomY);
  compassPoint(centreX, centreY, kCompassLabelRightX, kCompassLabelTopY, cosTheta, sinTheta, rightTopX, rightTopY);
  compassPoint(centreX, centreY, kCompassLabelRightX, kCompassLabelBottomY, cosTheta, sinTheta, rightBottomX,
               rightBottomY);
  renderer.drawLine(leftTopX, leftTopY, leftBottomX, leftBottomY, kCompassLabelStrokeWidth, true);
  renderer.drawLine(rightTopX, rightTopY, rightBottomX, rightBottomY, kCompassLabelStrokeWidth, true);
  renderer.drawLine(leftTopX, leftTopY, rightBottomX, rightBottomY, kCompassLabelStrokeWidth, true);

  // 2. Left arc, 3. right arc -- both open, not a closed ring: each spans
  // 100 degrees, leaving a gap at the top (under "N") and at the bottom
  // (below the triangle) instead of meeting its mirror.
  //
  // Rotating an arc about its own centre only shifts its angles, and the design
  // angle convention (0 = +x, growing toward +y, which is down) turns clockwise
  // exactly like compassPoint()'s -- so a point at design angle phi lands at
  // phi - theta, and the arcs get the same subtraction.
  drawCompassArc(renderer, centreX, centreY, kCompassArcRadius, kCompassLeftArcStartDeg - thetaDeg,
                 kCompassLeftArcEndDeg - thetaDeg, kCompassArcLineWidth);
  drawCompassArc(renderer, centreX, centreY, kCompassArcRadius, kCompassRightArcStartDeg - thetaDeg,
                 kCompassRightArcEndDeg - thetaDeg, kCompassArcLineWidth);

  // 4. Main center pointer -- a plain solid triangle, drawn after the arcs
  // so it stays the dominant shape on top. This is the part that carries the
  // whole indicator's meaning once the map turns: it points at north.
  int mainXs[3], mainYs[3];
  compassPoint(centreX, centreY, kCompassTriTopX, kCompassTriTopY, cosTheta, sinTheta, mainXs[0], mainYs[0]);
  compassPoint(centreX, centreY, kCompassTriLeftX, kCompassTriLeftY, cosTheta, sinTheta, mainXs[1], mainYs[1]);
  compassPoint(centreX, centreY, kCompassTriRightX, kCompassTriRightY, cosTheta, sinTheta, mainXs[2], mainYs[2]);
  renderer.fillPolygon(mainXs, mainYs, 3, true);

  // 5. Small accent triangle on the right arc.
  int accentXs[3], accentYs[3];
  compassPoint(centreX, centreY, kCompassAccentX1, kCompassAccentY1, cosTheta, sinTheta, accentXs[0], accentYs[0]);
  compassPoint(centreX, centreY, kCompassAccentX2, kCompassAccentY2, cosTheta, sinTheta, accentXs[1], accentYs[1]);
  compassPoint(centreX, centreY, kCompassAccentX3, kCompassAccentY3, cosTheta, sinTheta, accentXs[2], accentYs[2]);
  renderer.fillPolygon(accentXs, accentYs, 3, true);
}

void MapActivity::headerStatusRect(int& x, int& y, int& w, int& h) const {
  // The strip the status row owns: the globe slot, the Bluetooth logo and the
  // signal bars, plus the opaque backing's padding. Deliberately excludes the
  // battery block -- GUI.drawHeader() clears and draws that itself.
  //
  // **The globe's slot is always part of this rect, whether or not the globe
  // is drawn.** A rect that shrank when the globe went away would leave the
  // globe's pixels on the panel with nothing to erase them.
  const int screenWidth = renderer.getScreenWidth();
  const int batteryX = screenWidth - kHeaderMarginRight - BaseMetrics::values.batteryWidth;
  const int worstCasePercentWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
  const int barsRight = batteryX - worstCasePercentWidth - BaseTheme::batteryPercentSpacing - kHeaderGroupGap;
  const int barsLeft = barsRight - kHeaderBleBarsWidth;
  const int logoLeft = barsLeft - kHeaderBtToBarsGap - kHeaderBtLogoWidth;
  const int globeLeft = logoLeft - kHeaderGlobeToBtGap - kHeaderGlobeDiameter;
  // Battery's real icon top is kHeaderMarginTop + 11, not +5: drawHeader()
  // hands drawBatteryRight() rect.y+5 (BaseTheme.cpp:374), and
  // drawBatteryRight() adds another +6 of its own (:99) before drawing the
  // outline. Missing that second +6 is what put this row 6px above the
  // battery instead of level with it.
  const int batteryIconTop = kHeaderMarginTop + 5 + 6;
  const int iconBottom = batteryIconTop + BaseMetrics::values.batteryHeight;
  const int iconTop = iconBottom - kHeaderIconHeight;

  x = globeLeft - kHeaderBackingPad;
  y = iconTop - kHeaderBackingPad;
  w = (barsRight - globeLeft) + kHeaderBackingPad * 2;
  h = kHeaderIconHeight + kHeaderBackingPad * 2;
}

void MapActivity::drawHeaderStatus() {
  const int screenWidth = renderer.getScreenWidth();

  // One clear for the whole fixed strip, not the old per-element clear-rects
  // this replaced (GUI.drawHeader()'s own battery box, headerStatusRect()'s
  // BLE backing, and a manual pad below both). A place-name string shorter
  // than last frame's leaves no stale tail behind, because everything in
  // [0, kHeaderBarHeight) is wiped before anything is drawn.
  renderer.fillRect(0, 0, screenWidth, kHeaderBarHeight, false);

  // Battery: same call every other screen makes (BaseTheme.cpp:363), with no
  // title/subtitle -- those draw nothing when null, leaving just the icon and
  // (setting-permitting) the percentage text this screen never had before.
  GUI.drawHeader(renderer, Rect{0, kHeaderMarginTop, screenWidth, kHeaderRowHeight}, nullptr, nullptr);

  drawHeaderStatusStrip();
  drawHeaderPlaceName();

  // The line the map's own content starts below -- GfxRendererCanvas's minY
  // (kMapContentTop) is what actually stops the map drawing above this, not
  // this line; this is only what a rider sees at the boundary.
  renderer.fillRect(0, kHeaderSeparatorY, screenWidth, 1, true);
}

// Left side of the header: the nearest named place to the marker, from the
// same places walk drawMapLayers() already does for the dots
// (MapRenderer.h, MapNearestPlaces) -- no separate lookup, no second SD read.
//
// "Fine, coarse" (e.g. "Karlova Ves, Bratislava") when both a nearby
// village/suburb-tier point and a nearby city/town-tier point are in the
// currently loaded tiles; whichever one alone when only one is; nothing when
// neither is -- the tile format carries no link between the two
// (mapbuilder/build_config.json's place_ranks is a flat rank, not a
// hierarchy), so this is the closest available reading of "where am I"
// rather than a guaranteed "suburb of city" pair.
void MapActivity::drawHeaderPlaceName() {
  char text[MapNearestPlaces::kNameBufferLen * 2 + 4];
  if (nearestPlaces_.hasFine && nearestPlaces_.hasCoarse) {
    snprintf(text, sizeof(text), "%s, %s", nearestPlaces_.fineName, nearestPlaces_.coarseName);
  } else if (nearestPlaces_.hasFine) {
    snprintf(text, sizeof(text), "%s", nearestPlaces_.fineName);
  } else if (nearestPlaces_.hasCoarse) {
    snprintf(text, sizeof(text), "%s", nearestPlaces_.coarseName);
  } else {
    return;  // nothing loaded near the marker -- left blank, not a placeholder
  }

  int stripX, stripY, stripW, stripH;
  headerStatusRect(stripX, stripY, stripW, stripH);
  // kHeaderPlaceNameLeftX, not kTextX -- confirmed on hardware 2026-08-11 that
  // this text wants 2px more air from the left edge than the debug readout's
  // own margin gives it.
  const int maxWidth = stripX - kHeaderPlaceNameLeftX - kHeaderPlaceNameRightGap;
  if (maxWidth <= 0) return;

  // Same truncate-until-fits loop drawDebugLine() uses just below --
  // GfxRenderer::drawText does not clip and drawPixel logs every off-panel
  // pixel, so an untruncated name running into the icon cluster would flood
  // the log as well as overlap it.
  for (size_t len = strlen(text); len > 0 && renderer.getTextWidth(UI_10_FONT_ID, text) > maxWidth; --len) {
    text[len - 1] = '\0';
  }

  // +3: confirmed on hardware 2026-08-11 that the plain centred value (0
  // here, integer division rounding down) put the glyphs' own top pixel
  // flush against row 0 with no air above them; +1 and +2 still read as too
  // tight.
  const int y = (kHeaderBarHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2 + 3;
  renderer.drawText(UI_10_FONT_ID, kHeaderPlaceNameLeftX, y, text, true);
}

void MapActivity::drawHeaderStatusStrip() {
  const int screenWidth = renderer.getScreenWidth();

  // BLE: bars right-anchored clear of the battery block (icon plus its own
  // worst-case text), then a small Bluetooth logo to their left. "100%" is
  // the widest string drawBatteryRight ever draws (BaseTheme.cpp:118-120) --
  // measured here, not guessed, after a guessed 32px allowance (2026-08-06)
  // turned out too tight for some percentages and let the bars run into the
  // text.
  //
  // Every horizontal position here is re-derived by headerStatusRect(), which
  // is what the windowed repaint refreshes. Keep the two in step -- a strip
  // narrower than what is drawn leaves half a glyph behind.
  const int batteryX = screenWidth - kHeaderMarginRight - BaseMetrics::values.batteryWidth;
  const int worstCasePercentWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
  const int barsRight = batteryX - worstCasePercentWidth - BaseTheme::batteryPercentSpacing - kHeaderGroupGap;
  const int barsLeft = barsRight - kHeaderBleBarsWidth;
  const int logoLeft = barsLeft - kHeaderBtToBarsGap - kHeaderBtLogoWidth;
  const int globeLeft = logoLeft - kHeaderGlobeToBtGap - kHeaderGlobeDiameter;
  const int batteryIconTop = kHeaderMarginTop + 5 + 6;
  const int iconBottom = batteryIconTop + BaseMetrics::values.batteryHeight;
  const int iconTop = iconBottom - kHeaderIconHeight;

  // White backing first, like the compass halo and the busy badge: this can
  // land on live map lines, not blank margin.
  int backingX, backingY, backingW, backingH;
  headerStatusRect(backingX, backingY, backingW, backingH);
  renderer.fillRect(backingX, backingY, backingW, backingH, false);

  // The globe, while a transfer this screen asked for is outstanding. Drawn
  // from autoSyncPending_ rather than from transferIconShown_: this function
  // paints what is true, and transferIconShown_ only records what the panel
  // was last told -- which this call is about to make current.
  if (autoSyncPending_ > 0) {
    const int radius = kHeaderGlobeDiameter / 2;
    const int cx = globeLeft + radius;
    const int cy = iconTop + kHeaderIconHeight / 2;
    // Four quadrants make the closed ring. drawArc() can only start and end on
    // 90-degree boundaries (see drawCompass()'s note), which is exactly right
    // here -- a full circle is what is wanted, not an open arc.
    renderer.drawArc(radius, cx, cy, +1, +1, 1, true);
    renderer.drawArc(radius, cx, cy, +1, -1, 1, true);
    renderer.drawArc(radius, cx, cy, -1, +1, 1, true);
    renderer.drawArc(radius, cx, cy, -1, -1, 1, true);
    renderer.drawLine(cx - radius, cy, cx + radius, cy, 1, true);  // equator
    renderer.drawLine(cx, cy - radius, cx, cy + radius, 1, true);  // meridian
  }
  transferIconShown_ = autoSyncPending_ > 0;

  // Logo: a small hand-drawn Bluetooth rune -- a vertical spine (the actual
  // Bluetooth glyph's ascender/descender) plus two chevron wings crossing it,
  // same "vector glyph via GfxRenderer primitives" approach as the compass.
  // No bitmap asset exists at this size; the 32x32 menu icon
  // (components/icons/bluetooth.h) is for the home-screen launcher, not a
  // 14px status row. logoLeft is the spine's x -- the wings reach right of it
  // by kHeaderBtLogoWidth, crossing the spine at top, mid and bottom.
  const int logoQuarter = iconTop + kHeaderIconHeight / 4;
  const int logoMid = iconTop + kHeaderIconHeight / 2;
  const int logoThreeQuarter = iconTop + (kHeaderIconHeight * 3) / 4;
  const int logoTipX = logoLeft + kHeaderBtLogoWidth;
  renderer.drawLine(logoLeft, iconTop, logoLeft, iconBottom, 1, true);           // spine
  renderer.drawLine(logoLeft, iconTop, logoTipX, logoQuarter, 1, true);          // upper wing, down to tip
  renderer.drawLine(logoTipX, logoQuarter, logoLeft, logoMid, 1, true);          // upper wing, back to spine
  renderer.drawLine(logoLeft, logoMid, logoTipX, logoThreeQuarter, 1, true);     // lower wing, down to tip
  renderer.drawLine(logoTipX, logoThreeQuarter, logoLeft, iconBottom, 1, true);  // lower wing, back to spine

  auto& ble = freeink::BlePositionServer::getInstance();
  // The connection interval, **not** the MTU.
  //
  // This row answers one question for the rider: is my phone there. The
  // interval is set in `onConnect` and zeroed in `onCentralDisconnect`, so it
  // means exactly that and nothing else.
  //
  // The MTU does not. The *central* initiates the ATT exchange and many never
  // do: measured 2026-08-07, a BlueZ client connected, subscribed, ran `tiles`
  // and read every reply line while `negotiatedMtu()` stayed 0 -- and this row,
  // which used to test the MTU, drew the "no link" X through the whole session.
  // `info` omitting its `mtu` line was what proved it.
  const bool connected = ble.connIntervalMs() != 0;
  // Recorded on both paths, so updateHeaderStatus() compares against what is
  // actually on the panel rather than against the last thing it decided.
  drawnLinkConnected_ = connected;
  if (!connected) {
    // X across the bar slot, same "not present" convention as the WiFi
    // indicator (CrossPointWebServerActivity.cpp:483-486).
    renderer.drawLine(barsLeft, iconTop, barsLeft + kHeaderBleBarsWidth, iconBottom, 2, true);
    renderer.drawLine(barsLeft, iconBottom, barsLeft + kHeaderBleBarsWidth, iconTop, 2, true);
    drawnBleBars_ = 0;
    return;
  }

  const int bars = bleBarsForRssi(ble.rssi());
  drawnBleBars_ = bars;
  for (int i = 0; i < kHeaderBleBarCount; ++i) {
    const int barHeight = (i + 1) * kHeaderIconHeight / kHeaderBleBarCount;
    const int x = barsLeft + i * (kHeaderBleBarWidth + kHeaderBleBarGap);
    const int y = iconBottom - barHeight;
    if (i < bars) {
      renderer.fillRect(x, y, kHeaderBleBarWidth, barHeight, true);
    } else {
      renderer.drawRect(x, y, kHeaderBleBarWidth, barHeight, true);
    }
  }
}

void MapActivity::drawZoomSideHints() {
  // GUI.drawSideButtonHints(), not a hand-drawn box: every theme (Lyra,
  // RoundedRaff, ...) overrides it with its own rounded, correctly margined
  // box (e.g. LyraTheme.cpp:395-445), and hand-copying one theme's private
  // layout constants here (tried 2026-08-06, reverted) drifts the moment
  // that theme's numbers change and only matches the one theme copied.
  //
  // Symbols, not words: every override hardcodes SMALL_FONT_ID with no size
  // parameter, and "Zoom In"/"Zoom Out" rotated into a ~30px-wide box reads
  // as a blur. A single glyph is legible at that size and needs no font
  // control to prove it -- same "symbol over word in a tight space" call as
  // KeyboardEntryActivity.cpp:947's ">"/"<". Plain literals, not tr(): a
  // plus/minus is not language-dependent, matching that same precedent.
  // "-" alone rendered as barely a dot: smallFontFamily has no bold face
  // (main.cpp:102-103, single-glyph constructor) and the hyphen is a short,
  // thin stroke even before the 90-degree rotation shrinks it further.
  // Doubled, it survives -- same glyph, twice the ink, no new font needed.
  GUI.drawSideButtonHints(renderer, "+", "--");
}

void MapActivity::drawPanSideHints() {
  // Same box as drawZoomSideHints(), same plain-literal-glyph reasoning --
  // Up/Down pan instead of zooming while Observe is active. Real arrow
  // glyphs (U+2190-U+2193), not ASCII stand-ins: the interval was added to
  // ubuntu_10_regular.h/ubuntu_10_bold.h from OpenDyslexic-Bold.otf, the only
  // builtin source face that actually has them (Ubuntu/NotoSans do not, and
  // OpenDyslexic's Regular cut of the same glyphs read too thin on the
  // panel) -- convert-builtin-fonts.sh's fontstack-fallback pattern, same as
  // the Hebrew/Arabic supplement two faces down the same stack.
  //
  // UI_10_FONT_ID, not the side-hint default SMALL_FONT_ID: two points
  // bigger and the only one of the two with the arrow interval at all.
  // SMALL_FONT_ID is shared by every other screen's side/button hints
  // (boot, sleep, file browser, wifi, keyboard...) -- sizing it up for
  // legible arrows here would resize hints everywhere else too, for no
  // reason those screens asked for. drawSideButtonHints()'s fontId
  // parameter (BaseTheme.h/LyraTheme.h) exists so this screen can pick a
  // different one without touching any of them.
  //
  // Fed sideways on purpose: every drawSideButtonHints() caller's text goes
  // through drawTextRotated90CW() (BaseTheme.cpp), a 90-degree *clockwise*
  // rotation -- fine for "+"/"--" (rotation-symmetric enough to still read),
  // wrong for a directional glyph. Confirmed on hardware: right-arrow
  // rotated CW 90 deg lands pointing up, left-arrow lands pointing down. So
  // the physical Up button (topBtn) gets "->" and Down (bottomBtn) gets
  // "<-" -- what is actually wired to which glyph only makes sense after
  // you account for the rotation, not before.
  GUI.drawSideButtonHints(renderer, "→", "←", UI_10_FONT_ID);
}

void MapActivity::drawDebugLine(int y, char* text) {
  // GfxRenderer::drawText does not clip, and GfxRenderer::drawPixel answers
  // every off-panel pixel with a LOG_ERR -- one overlong readout line is
  // several hundred error lines over USB CDC. Trim to what fits instead.
  const int maxWidth = renderer.getScreenWidth() - kTextX * 2;
  for (size_t len = strlen(text); len > 0 && renderer.getTextWidth(UI_10_FONT_ID, text) > maxWidth; --len) {
    text[len - 1] = '\0';
  }
  // White backing sized to this line's own text, not a fixed strip: the
  // header status row (battery/BLE/globe, top right, drawHeaderStatus())
  // already drew into this same frame by the time this runs, and a backing
  // wider than the text it is behind would paint white over it. Same idiom
  // as that row's own backing (headerStatusRect()) -- just tight to this
  // line instead of a shared rect for a whole icon group.
  //
  // Height is getLineHeight() (the font's full advanceY), not
  // getTextHeight() (ascender only) -- ascender alone stops short of
  // descenders ("g", "y", the "j" in a route name), leaving their bottom
  // few pixels sitting on whatever the map drew, not the backing.
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, text);
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.fillRect(kTextX - kDebugPad, y - kDebugPad, textWidth + kDebugPad * 2, textHeight + kDebugPad * 2, false);
  renderer.drawText(UI_10_FONT_ID, kTextX, y, text, true);
}

MapActivity::MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* routePath)
    : Activity("Map", renderer, mappedInput), transfer_(kTileRoot) {
  if (routePath != nullptr && routePath[0] != '\0') {
    // Truncation would open the wrong file or none, so a path that does not fit
    // is refused outright rather than shortened.
    const size_t len = std::strlen(routePath);
    if (len < sizeof(routePath_)) {
      std::memcpy(routePath_, routePath, len + 1);
    } else {
      LOG_ERR(kLogTag, "route path too long, ignored: %s", routePath);
    }
  }
}

void MapActivity::onEnter() {
  Activity::onEnter();
  LOG_DBG(kLogTag, "onEnter start");

  freeink::BlePositionServer::getInstance().begin();
  LOG_DBG(kLogTag, "BlePositionServer.begin() returned");
  // After begin(), so the characteristics exist before anything can be
  // written to them.
  transfer_.attach();
  hasReceivedAny_ = false;
  lastDrawnSeq_ = 0;
  redrawDueMs_ = 0;
  saveDueMs_ = 0;
  showingPersistedFix_ = false;
  viewportDrawn_ = false;
  markerPatchValid_ = false;
  partialMoves_ = 0;
  screenMode_ = MapScreenMode::Follow;

  // Autosync starts from nothing every time this screen opens. The rate cap in
  // particular is per session on purpose: a rider who left the map and came
  // back is asking for the picture again, and making them wait out a cap armed
  // before they left would look like the feature is off.
  autoSyncWantCount_ = 0;
  autoSyncPending_ = 0;
  autoSyncArrived_ = false;
  autoSyncNextAskMs_ = 0;
  autoSyncDeadlineMs_ = 0;
  lastClearedTileSeq_ = 0;
  arrivalRedrawDueMs_ = 0;
  lastTransferProgress_ = 0;
  transferIconShown_ = false;
  drawnLinkConnected_ = false;
  drawnBleBars_ = -1;
  nextHeaderPollMs_ = 0;
  nextBarsRepaintMs_ = 0;

  // Ladder state comes back off the card exactly as it was left, per mode.
  mode_ = static_cast<MapRideMode>(SETTINGS.mapMode < kMapRideModeCount ? SETTINGS.mapMode : 0);
  for (uint8_t mode = 0; mode < kMapRideModeCount; ++mode) {
    zoomStep_[mode] = SETTINGS.mapZoomStep[mode] < MapViewport::kZoomStepCount ? SETTINGS.mapZoomStep[mode]
                                                                               : kDefaultZoomStepForMode[mode];
    markerStep_[mode] = SETTINGS.mapMarkerStep[mode] < MapViewport::kMarkerStepCount ? SETTINGS.mapMarkerStep[mode]
                                                                                     : kDefaultMarkerStepForMode[mode];
  }

  publishLadders();
  // The `missing` command's list, read straight out of the store rather than
  // copied in -- MISSING_TILES was loaded from the card in setup() and keeps
  // growing while this screen is open, so a copy would go stale mid-session.
  consoleState_.setMissingTilesSource(&g_missingTilesConsoleSource);
  // `skip` has to reach this screen per tile, not as a tally: two skips
  // between two loop() ticks would settle one ask and leave the other tile
  // unmarked, and an unmarked refusal is asked for again on the next frame.
  // Cleared in onExit() -- the console outlives nothing here, but a dangling
  // observer is not a thing to leave lying around either.
  consoleState_.setSkipObserver(this);
  // The freshness half of the same conversation: `tiles` flags a stale tile off
  // this list, and `stale`/`checked` land on this activity.
  consoleState_.setStaleTiles(&staleTiles_);
  consoleState_.setStaleObserver(this);
  // Constant for the build, so once here rather than per reset. `info` reports
  // it; the tile sync screen quotes the same number in NEED_TILES.
  consoleState_.setTileFormatVersion(MapTileReader::kFormatVersion);
  consoleState_.setLinkMtuProvider(
      +[]() -> uint16_t { return freeink::BlePositionServer::getInstance().negotiatedMtu(); });
  consoleState_.setLinkIntervalProvider(
      +[]() -> uint16_t { return freeink::BlePositionServer::getInstance().connIntervalMs(); });
  // `stats`: the power meter. Same numbers the SD card's power.csv carries
  // (src/PowerLog.cpp), answered live over whichever channel asked -- which on
  // a ride is BLE, because a device measuring its own draw cannot have USB
  // plugged in (VBUS charges the cell, and the reading stops meaning anything).
  consoleState_.setPowerStatsProvider(&fillMapPowerStats);
  // So `info` answers with real numbers before the first fix arrives rather
  // than reporting a 0 m/px viewport that has simply never been drawn.
  consoleState_.setZoomInfo(zoomStep(), MapViewport::kZoomLadder[zoomStep()].z,
                            MapViewport::kZoomLadder[zoomStep()].mpp);

  // The whole streaming path's RAM cost, paid once here rather than per tile
  // or per way. Logged as a before/after pair so the resident half of the
  // O(1) claim is a measured number and not an assertion about sizeof.
  const uint32_t heapBeforeAlloc = ESP.getFreeHeap();
  file_ = makeUniqueNoThrow<HalFileSource>();
  if (!file_) {
    LOG_ERR(kLogTag, "OOM: HalFileSource");
  } else {
    source_ = makeUniqueNoThrow<MapTileSource>(*file_, proj_);
    if (!source_) LOG_ERR(kLogTag, "OOM: MapTileSource (%u bytes)", static_cast<unsigned>(sizeof(MapTileSource)));
  }
  // The marker's background patch, in the same one-allocation-per-session
  // bracket as the tile source: a marker move must not allocate (CLAUDE.md's
  // heap-fragmentation rule), and this is far too big for a stack local. On OOM
  // follow is simply off and every fix redraws in full -- correct picture, slow
  // picture, no crash.
  // The place-name pass's working set, in the same bracket and for the same
  // reason: ~3.2 KB is far past the 256-byte stack rule, and a per-frame
  // allocation of it would fragment the heap on every viewport reset
  // (MapLabels.h). Allocated only when the compiled style actually draws labels,
  // so a style with max_labels 0 costs nothing. On OOM the map keeps its place
  // dots and loses the names -- a correct picture, one layer poorer, no crash.
  if (kDefaultMapStyle.placeMaxLabels > 0 &&
      (kDefaultMapStyle.placeLabelPx > 0 || kDefaultMapStyle.placeLabelMinorPx > 0)) {
    labels_ = makeUniqueNoThrow<MapLabelScratch>();
    if (!labels_) LOG_ERR(kLogTag, "OOM: label scratch (%u bytes)", static_cast<unsigned>(sizeof(MapLabelScratch)));
  }
  markerPatch_ = makeUniqueNoThrow<uint8_t[]>(kMarkerPatchBytes);
  markerPatchCapacity_ = markerPatch_ ? kMarkerPatchBytes : 0;
  if (!markerPatch_) LOG_ERR(kLogTag, "OOM: marker patch (%u bytes)", static_cast<unsigned>(kMarkerPatchBytes));
  const uint32_t heapAfterAlloc = ESP.getFreeHeap();
  LOG_DBG(kLogTag, "heap: %lu before source alloc, %lu after, delta %ld (sizeof MapTileSource = %u)",
          static_cast<unsigned long>(heapBeforeAlloc), static_cast<unsigned long>(heapAfterAlloc),
          static_cast<long>(heapBeforeAlloc) - static_cast<long>(heapAfterAlloc),
          static_cast<unsigned>(sizeof(MapTileSource)));

  // The route the rider picked, if any. Its own file handle next to the tile
  // source's: both stream during a render (MapRouteSource.h). About 1.2 KB, and
  // only when a route was actually chosen -- a skipped picker allocates neither.
  overviewShown_ = false;
  if (routePath_[0] != '\0') {
    routeFile_ = makeUniqueNoThrow<HalFileSource>();
    if (!routeFile_) {
      LOG_ERR(kLogTag, "OOM: HalFileSource for the route");
    } else {
      route_ = makeUniqueNoThrow<MapRouteSource>(*routeFile_, proj_);
      if (!route_) {
        LOG_ERR(kLogTag, "OOM: MapRouteSource (%u bytes)", static_cast<unsigned>(sizeof(MapRouteSource)));
      } else if (!route_->load(routePath_)) {
        // Header or point crc failed. Nothing is drawn rather than part of a
        // route: half a route ends somewhere it does not, which is the same class
        // of lie as drawing a missing tile white.
        LOG_ERR(kLogTag, "route refused: %s", routePath_);
        route_.reset();
        routeFile_.reset();
      } else {
        LOG_INF(kLogTag, "route \"%s\" loaded: %lu points, %lu bytes", route_->name(),
                static_cast<unsigned long>(route_->pointCount()), static_cast<unsigned long>(route_->bytesRead()));
      }
    }
  }

  // A route that loaded gets the overview as its first frame, whether or not
  // there is a fix: the rider just chose it and the whole point is seeing where
  // it goes. Without a route this is unchanged -- last known fix, or the waiting
  // banner.
  if (route_) {
    // Remembered so a later button press can re-render around it, exactly as the
    // no-route path does.
    if (SETTINGS.mapHasLastFix) {
      hasReceivedAny_ = true;
      showingPersistedFix_ = true;
      lastLatE7_ = SETTINGS.mapLastLatE7;
      lastLonE7_ = SETTINGS.mapLastLonE7;
      lastHeading_ = SETTINGS.mapLastHeading;
      updateManualHeadingCapture(lastHeading_);
    }
    renderRouteOverview();
    LOG_DBG(kLogTag, "onEnter done");
    return;
  }

  // Show where the rider was last seen instead of a blank screen, if the
  // card has one -- a real fix still latches in over the next loop() and
  // clears the banner (see the BLE/console branches in loop()).
  LOG_DBG(kLogTag, "onEnter: mapHasLastFix=%d", (int)SETTINGS.mapHasLastFix);
  if (SETTINGS.mapHasLastFix) {
    hasReceivedAny_ = true;
    showingPersistedFix_ = true;
    lastLatE7_ = SETTINGS.mapLastLatE7;
    lastLonE7_ = SETTINGS.mapLastLonE7;
    lastHeading_ = SETTINGS.mapLastHeading;
    updateManualHeadingCapture(lastHeading_);
    LOG_DBG(kLogTag, "onEnter: rendering persisted fix %d,%d", (int)lastLatE7_, (int)lastLonE7_);
    // Before the read, not after: this is the only viewport reset with no
    // feedback of any kind in front of it (a zoom or menu redraw gets the busy
    // badge through showBusy()). See renderLoadingTiles().
    renderLoadingTiles();
    renderViewport(lastLatE7_, lastLonE7_, lastHeading_, lastDrawnSeq_);
  } else {
    renderWaiting();
  }
  LOG_DBG(kLogTag, "onEnter done");
}

void MapActivity::onExit() {
  Activity::onExit();

  // A step landed on in the last few seconds before leaving must survive.
  // This is the one save that is not debounced, because there is no next
  // loop() to debounce into -- and it is still guarded by the value check,
  // so leaving the map without touching a button writes nothing.
  saveLaddersIfChanged();

  // An autosync still being answered has to be called off. The device cannot
  // stop the phone from its end -- the transfer protocol's abort opcode (0x03)
  // is a frame the *central* writes -- so the cancel is a word on the command
  // channel, the same one TileSyncActivity::leave() sends.
  if (autoSyncPending_ > 0 && freeink::BlePositionServer::getInstance().isCommandSubscribed()) {
    if (!freeink::BlePositionServer::getInstance().sendCommandReply("FETCH_CANCEL")) {
      LOG_ERR(kLogTag, "autosync: FETCH_CANCEL not delivered");
    }
  }
  autoSyncPending_ = 0;
  consoleState_.setSkipObserver(nullptr);
  consoleState_.setStaleObserver(nullptr);
  consoleState_.setStaleTiles(nullptr);
  MISSING_TILES.flushIfDirty();

  // Before end(): the hooks point at a member of this activity, and this
  // activity is about to be deleted (main.cpp's exitActivity). A transfer
  // still in flight loses its .part file here rather than surviving into a
  // screen that has no BLE link.
  transfer_.detach();
  freeink::BlePositionServer::getInstance().end();

  // Release order is the reverse of onEnter(): the source holds a reference
  // to the file source, so it goes first. HalFileSource's destructor closes
  // the member HalFile -- DESTRUCTOR_CLOSES_FILE only covers locals.
  source_.reset();
  file_.reset();
  // Same order for the route: the source holds a reference to its file source.
  route_.reset();
  routeFile_.reset();
  markerPatch_.reset();
  labels_.reset();
  markerPatchCapacity_ = 0;
  markerPatchValid_ = false;

  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void MapActivity::loop() {
  Activity::loop();

  // Menu owns input while open, same idiom as every other OptionPopup
  // consumer (TextSettingsActivity.cpp:179) -- the callback draws the popup
  // directly rather than going through requestUpdate(), because MapActivity
  // never uses the render task (see optionPopup_'s comment in MapActivity.h).
  //
  // OptionPopup closes on Back's *press* edge (OptionPopup.h), but the exit
  // check below fires on Back's *release* edge -- two different frames for
  // the same physical press. Left alone, the press closes the menu and the
  // release that follows it a frame or two later then also leaves the map.
  // Latch that one release so it is swallowed once, not treated as a second,
  // independent Back.
  //
  // Select has the identical problem on CONFIRM: it fires on the *press*
  // edge too, and handleButtons() opens the menu on CONFIRM's *release* edge.
  // gpio.update() only runs once per outer loop() (main.cpp), so a Select
  // whose row spends the better part of two seconds rendering never sees the
  // release until this activity's next loop() call -- by which point the
  // popup is already closed, so handleButtons() runs and reopens it. No
  // extra redraw needed here the way Back's case gets one: every Select
  // branch (openMapMenu()) already renders the map itself.
  const bool popupWasActive = optionPopup_.isActive();
  if (optionPopup_.handleInput(mappedInput, [this] { optionPopup_.processRender(renderer, mappedInput); })) {
    if (popupWasActive && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      suppressBackRelease_ = true;
      // handleInput() already set active=false and fired the redraw callback
      // above, but that callback is optionPopup_.processRender(), which is a
      // no-op once inactive -- nothing repaints the map underneath, and the
      // panel just keeps showing the popup's last pixels.
      //
      // A dismiss changed nothing, so the frame the menu covered is still the
      // right one: put the saved pixels back and refresh that window only
      // (captureMenuBackdrop()). Milliseconds, no card read. Only when there
      // is no backdrop does this cost a full redraw.
      if (!restoreMenuBackdrop()) {
        redrawDueMs_ = 0;
        showBusy();  // the popup's pixels are still up; say the redraw started
        renderCurrent();
      }
    }
    if (popupWasActive && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      suppressConfirmRelease_ = true;
    }
    // Dismissed by a tap outside the dialog (touch panels): no row callback
    // ran and no button edge lands in either branch above, so a backdrop still
    // held here is the only sign the map is sitting under the popup's pixels.
    if (popupWasActive && !optionPopup_.isActive() && menuBackdrop_) restoreMenuBackdrop();
    return;
  }

  freeink::PositionUpdate update;
  if (freeink::BlePositionServer::getInstance().getLatest(update)) {
    if (!hasReceivedAny_ || update.seq != lastDrawnSeq_) {
      hasReceivedAny_ = true;
      showingPersistedFix_ = false;
      lastDrawnSeq_ = update.seq;
      // heading is 0-15 straight off the wire now: the 19-byte packet
      // carries a MapHeading value, so the *2 fudge the old 8-step packet
      // needed is gone (BlePositionServer.h).
      //
      // speed, utc and altitude are carried and stored, and nothing reads
      // them yet -- auto zoom, the on-screen fix time and hike mode are
      // later phases. Logged so the fields can be seen arriving before
      // anything depends on them.
      char altStr[8];
      if (update.hasAltitude) {
        snprintf(altStr, sizeof(altStr), "%d", static_cast<int>(update.altitudeM));
      } else {
        snprintf(altStr, sizeof(altStr), "unset");
      }
      LOG_DBG(kLogTag, "ble fix: seq %u, heading %u, speed %u km/h, utc %lu, accuracy %u m, alt %s",
              static_cast<unsigned>(update.seq), static_cast<unsigned>(update.heading),
              static_cast<unsigned>(update.speedKmh), static_cast<unsigned long>(update.utc),
              static_cast<unsigned>(update.accuracyM), altStr);
      applyFix(update.lat, update.lon, update.heading, update.seq);
      // Debounced into the same save this fires for zoom/marker/mode --
      // CLAUDE.md rule 8 rules out a per-fix SD write just as much as a
      // per-press one.
      armSave();
    }
  }

  // The command console, over both channels. Same parser, same state, same
  // replies -- only the transport differs (MapCommandConsole.h). poll()
  // returns true only for a command that changed something, so every true
  // here is a real redraw request.
  const bool serialWants = serial_.poll();
  const bool bleWants = ble_.poll();
  if (serialWants || bleWants) {
    syncLaddersFromConsole();
    // Console commands redraw immediately rather than through the button
    // coalescer: they arrive one at a time from a script that is waiting for
    // the reply, so there is nothing to coalesce and a delay would only make
    // `mapcmd.py pos ...` feel broken.
    redrawDueMs_ = 0;
    if (consoleState_.hasPosition()) {
      const bool moved = consoleState_.latE7() != lastLatE7_ || consoleState_.lonE7() != lastLonE7_;
      hasReceivedAny_ = true;
      showingPersistedFix_ = false;
      if (moved) {
        // A `pos` goes through the same follow decision as a BLE fix: a metre
        // away must cost what a real fix a metre away costs, or the console
        // stops being a way to exercise this path. A `pos` that is skipped as
        // too small says so in the log and leaves the panel alone -- that is
        // the behaviour under test, not a dropped command.
        applyFix(consoleState_.latE7(), consoleState_.lonE7(), consoleState_.heading(),
                 static_cast<uint8_t>(consoleState_.seq()));
      } else {
        // `zoom`/`marker`/`mode`/`heading`/`redraw` with the position unchanged.
        // Every one of them is an explicit instruction to change the picture, so
        // none of them goes through the follow decision -- a `heading` command
        // that only nudged the marker's arrow would look like a dead console.
        lastHeading_ = consoleState_.heading();
        renderCurrent();
      }
      armSave();
    } else {
      // `zoom`/`marker`/`mode` before any fix: the step is taken and stored,
      // there is simply nothing to draw it around yet.
      renderCurrent();
    }
  }

  handleButtons();

  // Autosync, in the order the state moves: land what arrived, settle what the
  // phone refused (that happens in ble_.poll() above, through onTileSkipped),
  // give up on what neither, then ask for whatever the last frame hatched, and
  // finally put the globe on or off to match.
  //
  // All of it is a handful of integer compares per tick when the feature is
  // off or idle -- the same cost class as the redraw and save deadlines below.
  drainTransferredTiles();
  expireAutoSync();
  maybeAutoSyncTiles();
  maybeCheckTileFreshness();
  // Also the link state and the signal bars, which have nothing to do with
  // autosync -- this is simply the one place that repaints that row.
  updateHeaderStatus();

  const uint32_t now = millis();
  if (redrawDueMs_ != 0 && now >= redrawDueMs_) {
    redrawDueMs_ = 0;
    renderCurrent();
  }
  // Reuses `now` above -- this is one more integer compare per loop() tick,
  // same cost class as the redraw/save checks either side of it, not a new
  // per-tick expense.
  if (missingTilesSaveDueMs_ != 0 && now >= missingTilesSaveDueMs_) {
    missingTilesSaveDueMs_ = 0;
    MISSING_TILES.flushIfDirty();
  }
  // A tile landed and the panel is still hatching where it goes. Deliberately
  // its own deadline rather than armRedraw()'s: this settles on the last
  // arrival, and a button press must not be made to wait behind it.
  if (arrivalRedrawDueMs_ != 0 && now >= arrivalRedrawDueMs_) {
    arrivalRedrawDueMs_ = 0;
    LOG_INF(kLogTag, "tiles arrived, redrawing");
    showBusy();
    renderCurrent();
  }
  // Checked after the redraw, never before it: the redraw is what the rider
  // is waiting for, and this is an SD write.
  if (saveDueMs_ != 0 && millis() >= saveDueMs_) {
    saveDueMs_ = 0;
    saveLaddersIfChanged();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (suppressBackRelease_) {
      suppressBackRelease_ = false;
    } else {
      onGoHome(HomeMenuItem::MAP);
    }
  }
}

void MapActivity::handleButtons() {
  // Logical buttons, never HalGPIO::BTN_* -- the front four are remappable
  // in settings and the mapping is orientation-aware (firmware CLAUDE.md).
  //
  // Follow: the ladder steps below. Observe: the same four buttons pan
  // instead (panBy()) -- see MapScreenMode's comment for why this is a
  // separate switch rather than folded into the ladder-step calls.
  switch (screenMode_) {
    case MapScreenMode::Follow:
      // Up is toward the closest rung: step 0 is 1 m/px, step 4 is 20
      // (MapViewport.h:61-68). Zooming in is going up the ladder.
      //
      // Worth keeping in mind when a rung looks broken: step 0 shows 480x800
      // **metres**, so over sparse countryside an empty panel there is the
      // honest answer, not a missing tile. Mistaken for a render bug once,
      // 2026-08-07.
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) stepZoom(-1);
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) stepZoom(+1);

      // Right increases look-ahead, which moves the marker *down* the screen
      // -- docs/architecture-plan.md. Read the pair as a look-ahead slider,
      // not as a marker position, or the direction reads backwards.
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) stepMarker(-1);
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) stepMarker(+1);
      break;
    case MapScreenMode::Observe:
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) panBy(PanDirection::Up);
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) panBy(PanDirection::Down);
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) panBy(PanDirection::Left);
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) panBy(PanDirection::Right);
      break;
  }

  // Opens the map menu: Refresh, Mode (ride/hike/cycle) and Observation
  // mode/Follow mode -- none of that changes with screenMode_, CONFIRM always
  // opens the same menu.
  //
  // suppressConfirmRelease_ swallows the one release that belongs to the
  // press a Select already consumed (loop()) -- without it, every Select
  // reopens the menu it just acted from. Same idiom as suppressBackRelease_
  // just below it in loop().
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (suppressConfirmRelease_) {
      suppressConfirmRelease_ = false;
    } else {
      openMapMenu();
    }
  }
}

void MapActivity::drawMapButtonHints() {
  // Follow: Up/Down zoom, Left/Right step the marker ladder -- words, since
  // "zoom in/out" and "look further ahead/back" have no obvious single glyph.
  // Observe: the four buttons are a pure direction pad, and a direction pad
  // reads faster as arrows than as the words "Left"/"Right" -- same
  // symbol-over-word call as drawZoomSideHints()'s "+"/"--"; plain literals,
  // not tr(), for the same reason (an arrow is not language-dependent).
  // CONFIRM says "Options", not "Select": it opens a menu, it does not pick
  // anything. "Select" belongs inside the popup, where a row really is picked.
  // Exhaustive switch, no default, so a third mode cannot land here silently
  // unlabeled (control-flow-clarity).
  switch (screenMode_) {
    case MapScreenMode::Follow: {
      const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_MAP_OPTIONS), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      drawZoomSideHints();
      break;
    }
    case MapScreenMode::Observe: {
      const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_MAP_OPTIONS), "←", "→");
      // btn3/btn4 only (Exit/Options stay at the theme's normal size): the
      // arrow glyphs need drawPanSideHints()'s UI_10_FONT_ID (10pt) to match
      // the side hints, but Exit/Options are ordinary words that should look
      // like every other screen's -- found on hardware 2026-08-08, first as
      // "all four boxes grew" (a shared fontId argument), then as "why is
      // the whole row bigger, only the arrows should be."
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, 0, UI_10_FONT_ID,
                          UI_10_FONT_ID);
      drawPanSideHints();
      break;
    }
  }
}

bool MapActivity::captureMenuBackdrop() {
  dropMenuBackdrop();
  const Rect rect = optionPopup_.frameRect(renderer);
  const size_t size = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
  if (size == 0) return false;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buffer) {
    // Not fatal: every close path still has renderCurrent() behind it.
    LOG_ERR(kLogTag, "menu backdrop unavailable: %u bytes, free heap %u", static_cast<unsigned>(size),
            static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  if (!renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height, buffer.get(), size)) {
    LOG_ERR(kLogTag, "menu backdrop read rejected: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
    return false;
  }
  menuBackdrop_ = std::move(buffer);
  menuBackdropSize_ = size;
  menuBackdropRect_ = rect;
  return true;
}

void MapActivity::dropMenuBackdrop() {
  menuBackdrop_.reset();
  menuBackdropSize_ = 0;
  menuBackdropRect_ = Rect{0, 0, 0, 0};
}

bool MapActivity::restoreMenuBackdrop() {
  if (!menuBackdrop_) return false;
  const Rect rect = menuBackdropRect_;
  const bool written =
      renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, menuBackdrop_.get(), menuBackdropSize_);
  dropMenuBackdrop();
  if (!written) {
    LOG_ERR(kLogTag, "menu backdrop write rejected: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
    return false;
  }
  // The popup drew its own four hints over the map's, in the band below the
  // dialog. Repaint ours, and refresh from the dialog's top down to the bottom
  // of the panel so the one window covers both.
  drawMapButtonHints();
  const int x = 0;
  const int y = rect.y;
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight() - rect.y;
  if (!renderer.displayBufferWindow(x, y, w, h)) {
    LOG_ERR(kLogTag, "menu close window rejected: %d,%d %dx%d", x, y, w, h);
    return false;
  }
  // The panel now holds exactly the frame that was up before the menu, marker
  // included: the backdrop was taken after that frame was composited, so
  // follow state (markerPatchValid_, viewportDrawn_, busyShown_) still
  // describes what is on the glass and none of it is touched here.
  return true;
}

void MapActivity::openMapMenu() {
  // Labels and values are separate columns now, not one "Label: value" string:
  // the popup left-aligns the labels and boxes the value on the selected row,
  // the same "this is the changeable part" cue the Settings list gives
  // (OptionPopup::showWithValues(), BaseTheme::drawOptionPopup()). A row with
  // an empty value is a plain action.
  std::vector<std::string> options;
  std::vector<std::string> values;
  options.reserve(8);
  values.reserve(8);
  options.push_back(tr(STR_REFRESH));
  values.emplace_back();
  options.push_back(tr(STR_MAP_MODE));
  values.push_back(I18N.get(kMapModeIds[static_cast<uint8_t>(mode_)]));
  // Only once a fix has actually drawn a frame -- same "no row that cannot do
  // anything" rule as Whole route below. A rider with nothing on screen yet
  // has nothing to look around in.
  int observeIdx = -1;
  if (hasReceivedAny_) {
    observeIdx = static_cast<int>(options.size());
    options.push_back(
        I18N.get(screenMode_ == MapScreenMode::Observe ? StrId::STR_MAP_FOLLOW_MODE : StrId::STR_MAP_OBSERVE_MODE));
    values.emplace_back();
  }
  // Only with a route loaded. A row that cannot do anything is worse than no
  // row: it reads as a feature that is broken rather than one that needs a route
  // picked at the door.
  const bool hasRoute = route_ != nullptr;
  int wholeRouteIdx = -1;
  if (hasRoute) {
    wholeRouteIdx = static_cast<int>(options.size());
    options.push_back(tr(STR_MAP_WHOLE_ROUTE));
    values.emplace_back();
  }
  // Quick toggles for settings the rider wants to flip mid-ride without
  // leaving the map -- zoom/rotation/heading mode. The Settings screen
  // entries for the same three fields (SettingsList.h) decide what the map
  // opens with, next time; this menu changes the same CrossPointSettings
  // fields live, so the two never disagree about the current value.
  const int zoomModeIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_ZOOM_MODE));
  values.push_back(
      I18N.get(SETTINGS.mapZoomMode == CrossPointSettings::MAP_ZOOM_AUTO ? StrId::STR_AUTO : StrId::STR_MANUAL));
  const int rotationIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_ROTATION_MODE));
  values.push_back(I18N.get(SETTINGS.mapRotationMode == CrossPointSettings::MAP_ROTATION_NORTH_UP
                                ? StrId::STR_MAP_ROTATION_NORTH_UP
                                : StrId::STR_MAP_ROTATION_HEADING_UP));
  const int headingIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_HEADING_MODE));
  values.push_back(I18N.get(SETTINGS.mapHeadingMode == CrossPointSettings::MAP_HEADING_MANUAL ? StrId::STR_MANUAL
                                                                                              : StrId::STR_AUTO));
  const int debugInfoIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_DEBUG_INFO));
  values.push_back(I18N.get(SETTINGS.mapDebugInfo ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF));
  optionPopup_.showWithValues(
      StrId::STR_MAP, options, values, 0,
      [this, observeIdx, wholeRouteIdx, zoomModeIdx, rotationIdx, headingIdx, debugInfoIdx](int idx) {
        // Rows that redraw the map do not need the backdrop; rows
        // that change nothing on it (zoom mode) put it back
        // instead of re-rendering, and so does a plain dismiss
        // (loop()). Freed first for every redraw row, so the
        // buffer is not held across a tile read.
        if (idx != zoomModeIdx) dropMenuBackdrop();
        if (idx == 0) {
          redrawDueMs_ = 0;
          showBusy();  // Refresh is the slowest thing on this screen; acknowledge it
          renderCurrent();
        } else if (idx == 1) {
          // One Select steps ride->hike->cycle->ride and closes, same as every
          // other row -- picking a mode is a deliberate, one-shot choice, not the
          // start of a cycling gesture. A rider who wants to step again presses
          // CONFIRM again. mapRideModeName()'s array order.
          const uint8_t next = (static_cast<uint8_t>(mode_) + 1) % kMapRideModeCount;
          switchMode(static_cast<MapRideMode>(next));
        } else if (idx == observeIdx) {
          toggleObserveMode();
        } else if (idx == wholeRouteIdx) {
          // Back to the whole route, at any point in a ride. Costs one full refresh
          // and one pass over the route file, the same as the frame the picker drew.
          redrawDueMs_ = 0;
          showBusy();
          renderRouteOverview();
        } else if (idx == zoomModeIdx) {
          // No new frame: the setting has no runtime effect yet (auto zoom is
          // not wired up, docs/map-data-spec.md), so the map underneath is
          // still correct. Put the saved pixels back instead of re-reading
          // tiles for a picture that would come out identical.
          SETTINGS.mapZoomMode = SETTINGS.mapZoomMode == CrossPointSettings::MAP_ZOOM_MANUAL
                                     ? CrossPointSettings::MAP_ZOOM_AUTO
                                     : CrossPointSettings::MAP_ZOOM_MANUAL;
          SETTINGS.saveToFile();
          if (!restoreMenuBackdrop()) {
            // Without a backdrop the popup's pixels are still up and only a
            // real frame clears them.
            redrawDueMs_ = 0;
            showBusy();
            renderCurrent();
          }
        } else if (idx == rotationIdx) {
          SETTINGS.mapRotationMode = SETTINGS.mapRotationMode == CrossPointSettings::MAP_ROTATION_HEADING_UP
                                         ? CrossPointSettings::MAP_ROTATION_NORTH_UP
                                         : CrossPointSettings::MAP_ROTATION_HEADING_UP;
          SETTINGS.saveToFile();
          redrawDueMs_ = 0;
          showBusy();
          renderCurrent();
        } else if (idx == headingIdx) {
          SETTINGS.mapHeadingMode = SETTINGS.mapHeadingMode == CrossPointSettings::MAP_HEADING_AUTO
                                        ? CrossPointSettings::MAP_HEADING_MANUAL
                                        : CrossPointSettings::MAP_HEADING_AUTO;
          // Freeze on the heading the frame is showing right now, not a stale
          // or default one -- same capture updateManualHeadingCapture() does
          // from a fresh fix, called here because a menu pick is not a fix.
          updateManualHeadingCapture(lastHeading_);
          SETTINGS.saveToFile();
          redrawDueMs_ = 0;
          showBusy();
          renderCurrent();
        } else if (idx == debugInfoIdx) {
          SETTINGS.mapDebugInfo = SETTINGS.mapDebugInfo ? 0 : 1;
          SETTINGS.saveToFile();
          redrawDueMs_ = 0;
          showBusy();
          renderCurrent();
        }
      });
  // After show() (the layout the rect comes from needs the rows) and before
  // the first draw (the framebuffer still holds the map).
  captureMenuBackdrop();
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::toggleObserveMode() {
  if (screenMode_ == MapScreenMode::Follow) {
    // Nothing on screen to look around in yet -- same "cost nothing" rule as
    // switchMode() picking the mode already on screen.
    if (!hasReceivedAny_) return;
    screenMode_ = MapScreenMode::Observe;
    // The fix in effect right now, so "Follow mode" later renders around the
    // rider's actual position and not wherever panning left off. Not
    // lastLatE7_/lastLonE7_ themselves -- panBy() repoints those at the pan
    // target on every step.
    observeReturnLatE7_ = lastLatE7_;
    observeReturnLonE7_ = lastLonE7_;
    observeReturnHeading_ = lastHeading_;
    observeReturnSeq_ = lastDrawnSeq_;
    LOG_DBG(kLogTag, "observation mode on, return point %d,%d", static_cast<int>(observeReturnLatE7_),
            static_cast<int>(observeReturnLonE7_));
    redrawDueMs_ = 0;
    showBusy();
    // Redraws the same frame that is already up -- the point is the button
    // hints switching to the pan labels, not a new picture.
    renderCurrent();
    return;
  }

  screenMode_ = MapScreenMode::Follow;
  LOG_DBG(kLogTag, "observation mode off, returning to %d,%d", static_cast<int>(observeReturnLatE7_),
          static_cast<int>(observeReturnLonE7_));
  redrawDueMs_ = 0;
  showBusy();
  renderViewport(observeReturnLatE7_, observeReturnLonE7_, observeReturnHeading_, observeReturnSeq_);
}

void MapActivity::panBy(PanDirection direction) {
  // Guards renderViewport()'s own OOM fallback (renderWaiting() when
  // source_ is null) rather than viewportDrawn_: that flag is about whether
  // an *incoming fix* may move the marker incrementally, not about whether a
  // frame is on screen -- the persisted-fix banner frame draws real tiles
  // with viewportDrawn_ left false, and panning is just as valid there.
  if (!source_) return;

  const int16_t markerY = MapViewport::markerYForStep(markerStep());
  const int16_t halfWidth = static_cast<int16_t>(renderer.getScreenWidth() / 2);
  const int16_t halfHeight = static_cast<int16_t>(renderer.getScreenHeight() / 2);
  int16_t targetX = MapViewport::kAnchorScreenX;
  int16_t targetY = markerY;
  switch (direction) {
    case PanDirection::Left:
      targetX -= halfWidth;
      break;
    case PanDirection::Right:
      targetX += halfWidth;
      break;
    case PanDirection::Up:
      targetY -= halfHeight;
      break;
    case PanDirection::Down:
      targetY += halfHeight;
      break;
  }

  // Inverse-project through proj_ -- the frame actually on screen, whether it
  // was drawn by the last real fix or the previous pan step -- so each press
  // moves half a screen from wherever the rider last panned to.
  double mercX = 0.0, mercY = 0.0;
  proj_.screenToMerc(targetX, targetY, mercX, mercY);
  double lat = 0.0, lon = 0.0;
  MapProjection::mercToLonLat(mercX, mercY, lat, lon);

  LOG_DBG(kLogTag, "pan: half-screen step, new anchor %.5f,%.5f", lat, lon);
  showBusy();
  // Not coalesced on the settle timer stepZoom/stepMarker use: a pan step's
  // target is computed from the frame the *previous* step drew (proj_), which
  // does not exist until that render actually runs, so batching bursts would
  // either collapse them onto the same target or need its own accumulator for
  // no real benefit -- loop() cannot poll another press until this blocking
  // render returns anyway (single-threaded, no coalescing to be had).
  renderViewport(static_cast<int32_t>(lat * 1e7), static_cast<int32_t>(lon * 1e7), anchorHeading_, lastDrawnSeq_);
}

void MapActivity::switchMode(MapRideMode newMode) {
  if (newMode == mode_) return;
  mode_ = newMode;
  LOG_DBG(kLogTag, "menu: mode -> %s", mapRideModeName(mode_));
  publishLadders();
  // A deliberate menu pick, not a ladder step -- redraw now, same as the
  // console's `mode` command (syncLaddersFromConsole()), not coalesced.
  redrawDueMs_ = 0;
  showBusy();
  renderCurrent();
  armSave();
}

void MapActivity::stepZoom(int delta) {
  const int next = static_cast<int>(zoomStep()) + delta;
  // Ends of the ladder are hard stops, not wraps. A press that changes
  // nothing must also cost nothing: no redraw, no SD write.
  if (next < 0 || next >= MapViewport::kZoomStepCount) return;
  zoomStep_[static_cast<uint8_t>(mode_)] = static_cast<uint8_t>(next);
  LOG_DBG(kLogTag, "zoom step %u (%.1f m/px, LOD z%u)", static_cast<unsigned>(zoomStep()),
          MapViewport::kZoomLadder[zoomStep()].mpp, static_cast<unsigned>(MapViewport::kZoomLadder[zoomStep()].z));
  publishLadders();
  armRedraw();
  armSave();
}

void MapActivity::stepMarker(int delta) {
  const int next = static_cast<int>(markerStep()) + delta;
  if (next < 0 || next >= MapViewport::kMarkerStepCount) return;
  markerStep_[static_cast<uint8_t>(mode_)] = static_cast<uint8_t>(next);
  LOG_DBG(kLogTag, "marker step %u (y=%d)", static_cast<unsigned>(markerStep()),
          static_cast<int>(MapViewport::markerYForStep(markerStep())));
  publishLadders();
  armRedraw();
  armSave();
}

void MapActivity::armRedraw() {
  redrawDueMs_ = millis() + kButtonSettleMs;
  // Before the settle, not after: the whole point is that the press is
  // acknowledged now rather than when the map is ready.
  showBusy();
}

void MapActivity::armSave() { saveDueMs_ = millis() + kSaveSettleMs; }

void MapActivity::publishLadders() { consoleState_.setLadders(zoomStep(), markerStep(), mode_); }

void MapActivity::syncLaddersFromConsole() {
  const MapRideMode requestedMode = consoleState_.mode();
  if (requestedMode != mode_) {
    // Switching mode brings that mode's own steps back -- they were never
    // lost, they were sitting in their own slot while another mode was
    // current. Nothing is read back off the card here (see MapActivity.h).
    mode_ = requestedMode;
    LOG_DBG(kLogTag, "mode %s: zoom step %u, marker step %u, class mask 0x%08lx", mapRideModeName(mode_),
            static_cast<unsigned>(zoomStep()), static_cast<unsigned>(markerStep()),
            static_cast<unsigned long>(modeMasks_.forMode(mode_)));
  } else {
    const uint8_t index = static_cast<uint8_t>(mode_);
    if (consoleState_.zoomStep() < MapViewport::kZoomStepCount) zoomStep_[index] = consoleState_.zoomStep();
    if (consoleState_.markerStep() < MapViewport::kMarkerStepCount) markerStep_[index] = consoleState_.markerStep();
  }
  // Push the resolved values back: after a mode switch the console's own
  // copy is a step behind, and `info` must report what is on screen.
  publishLadders();
  armSave();
}

void MapActivity::saveLaddersIfChanged() {
  // Every mode's slot is written, not just the current one: a rider who set
  // hike's steps and then switched to ride must not lose hike's on the next
  // save.
  bool changed = SETTINGS.mapMode != static_cast<uint8_t>(mode_);
  for (uint8_t mode = 0; mode < kMapRideModeCount && !changed; ++mode) {
    changed = SETTINGS.mapZoomStep[mode] != zoomStep_[mode] || SETTINGS.mapMarkerStep[mode] != markerStep_[mode];
  }
  // Only a fix this *session* actually produced, never the one onEnter()
  // just bootstrapped off the card -- otherwise every re-entry would write
  // the same fix straight back at itself. showingPersistedFix_ is exactly
  // that distinction (cleared the moment a real fix lands, see loop()).
  const bool fixChanged = hasReceivedAny_ && !showingPersistedFix_ &&
                          (!SETTINGS.mapHasLastFix || SETTINGS.mapLastLatE7 != lastLatE7_ ||
                           SETTINGS.mapLastLonE7 != lastLonE7_ || SETTINGS.mapLastHeading != lastHeading_);
  // CLAUDE.md rule 8: never write the settings file on every interaction.
  // The presses (and now fixes) already coalesced into one deadline; this is
  // the second guard, and the one that makes leaving and re-entering the map
  // free when nothing actually moved.
  if (!changed && !fixChanged) return;

  SETTINGS.mapMode = static_cast<uint8_t>(mode_);
  for (uint8_t mode = 0; mode < kMapRideModeCount; ++mode) {
    SETTINGS.mapZoomStep[mode] = zoomStep_[mode];
    SETTINGS.mapMarkerStep[mode] = markerStep_[mode];
  }
  if (fixChanged) {
    SETTINGS.mapHasLastFix = true;
    SETTINGS.mapLastLatE7 = lastLatE7_;
    SETTINGS.mapLastLonE7 = lastLonE7_;
    SETTINGS.mapLastHeading = lastHeading_;
  }
  if (!SETTINGS.saveToFile()) {
    LOG_ERR(kLogTag, "failed to persist map ladder state");
    return;
  }
  LOG_DBG(kLogTag, "saved ladder state: mode %s, zoom %u, marker %u", mapRideModeName(mode_),
          static_cast<unsigned>(zoomStep()), static_cast<unsigned>(markerStep()));
}

bool MapActivity::preventAutoSleep() { return freeink::BlePositionServer::getInstance().isRunning(); }

void MapActivity::renderWaiting() {
  // Same reason as renderViewport(): whatever asked for this frame asked for the
  // ordinary map, not the overview.
  overviewShown_ = false;
  renderer.clearScreen();
  renderer.drawText(UI_10_FONT_ID, 8, 8, tr(STR_MAP_WAITING_BLE), true);
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_MAP_OPTIONS), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  drawZoomSideHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  busyShown_ = false;  // this frame painted over the badge
  // No map and no marker on this frame: there is nothing for a fix to move
  // inside, so the next one draws a real viewport (applyFix()).
  viewportDrawn_ = false;
  markerPatchValid_ = false;
}

void MapActivity::renderLoadingTiles() {
  // Same centred logo layout as BootActivity/SleepActivity, not a top-left
  // status line: this is the same kind of "device is busy, wait" screen they
  // are, and should look like one rather than like debug text.
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // "Safe explorink" -- a pun on the brand name, so it stays untranslated on
  // purpose (like the coordinates on the location sleep screen): the joke
  // only lands in English, and a fallback in the other 30 languages would
  // just be this same string anyway.
  constexpr int kLogoTop = -60;         // logo is 120px, centred on pageHeight / 2
  constexpr int kPunToLogoMargin = 20;  // minimum requested gap between the two
  const int punLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 + kLogoTop - kPunToLogoMargin - punLineHeight,
                            "Safe explorink", true, EpdFontFamily::BOLD);
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_MAP_LOADING_TILES), true, EpdFontFamily::BOLD);
  // The rung, because "reading tiles" alone does not say how long this will
  // take and the rung is what decides it (docs/optimization/01-render-pipeline.md
  // has the per-rung times).
  char line[48];
  snprintf(line, sizeof(line), "z%u  %.0f m/px", MapViewport::kZoomLadder[zoomStep()].z,
           MapViewport::kZoomLadder[zoomStep()].mpp);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, line);
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_MAP_OPTIONS), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  drawZoomSideHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  // Deliberately does not touch viewportDrawn_ or markerPatchValid_: the
  // viewport reset that follows sets both from its own frame, and claiming a
  // marker patch for this text screen would let a fix restore it over the map.
  busyShown_ = false;  // this frame painted over the badge
}

void MapActivity::renderCurrent() {
  if (!hasReceivedAny_) {
    renderWaiting();
    return;
  }
  renderViewport(lastLatE7_, lastLonE7_, lastHeading_, lastDrawnSeq_);
}

MarkerMetrics MapActivity::markerMetrics() const {
  return markerMetricsFor(MapViewport::zoomStepAt(zoomStep()).markerScale8);
}

void MapActivity::markerRect(int cx, int cy, int& x, int& y, int& w, int& h) const {
  // The box the marker on the panel was drawn with, not the box the current
  // rung would draw: a rung change re-anchors (stepZoom -> renderCurrent), but
  // reading the live rung here would size an erase against a marker painted at
  // another scale if that order ever changed, and the failure would be a
  // smeared ring nobody could trace back.
  const int box = markerBoxDrawn_ > 0 ? markerBoxDrawn_ : markerMetrics().box;
  w = box;
  h = box;
  x = cx - box / 2;
  y = cy - box / 2;
}

bool MapActivity::saveMarkerPatch(int cx, int cy) {
  if (!markerPatch_) return false;
  int x, y, w, h;
  markerRect(cx, cy, x, y, w, h);
  return renderer.readFramebufferRegion(x, y, w, h, markerPatch_.get(), markerPatchCapacity_) != 0;
}

void MapActivity::moveMarker(int16_t sx, int16_t sy, uint8_t headingStep) {
  // A rung change re-anchors (stepZoom -> renderCurrent), so the marker on the
  // panel is always the current rung's size when a fix arrives here. If that
  // order ever changes, the erase below would restore a box of the wrong size
  // and leave a ring behind, so check rather than trust: a full redraw is the
  // correct answer and costs what a rung change costs anyway.
  if (markerBoxDrawn_ > 0 && markerBoxDrawn_ != static_cast<int16_t>(markerMetrics().box)) {
    LOG_DBG(kLogTag, "marker box %d -> %d without a re-anchor -- full redraw", (int)markerBoxDrawn_,
            (int)markerMetrics().box);
    renderCurrent();
    return;
  }
  int oldX, oldY, oldW, oldH;
  markerRect(markerDrawnX_, markerDrawnY_, oldX, oldY, oldW, oldH);
  // Erase: the map, compass, readout and hints under the marker exist nowhere
  // but this patch (single-buffer mode has no shadow copy of the frame), which
  // is why applyFix() re-anchors instead of coming here when it is not valid.
  renderer.writeFramebufferRegion(oldX, oldY, oldW, oldH, markerPatch_.get());

  // Save the new spot *after* the restore, so an overlapping move saves real
  // background rather than the marker it is about to erase.
  markerPatchValid_ = saveMarkerPatch(sx, sy);
  if (!markerPatchValid_) {
    // The frame now has no marker on it at all. Draw a full one rather than
    // refresh a marker-less picture.
    LOG_ERR(kLogTag, "marker patch save failed at %d,%d -- falling back to a full redraw", (int)sx, (int)sy);
    renderCurrent();
    return;
  }

  // Relative to the frame's heading, not the raw fix: the map is track-up, so
  // "up" on this frame means anchorHeading_ (MapActivity.h).
  drawPositionMarker(sx, sy, MapFollow::relativeHeadingStep(headingStep, anchorHeading_), mode_);

  int newX, newY, newW, newH;
  markerRect(sx, sy, newX, newY, newW, newH);

  // **One window over both boxes, always.** Measured on the X4 2026-08-05: a
  // windowed refresh takes the same 500 ms as a whole-panel one, whatever its
  // area -- the waveform is a fixed cost and the window only narrows what it
  // touches (62 moves over a replayed ride, every refresh 500 ms, identical to
  // the 26 full-frame refreshes in the same run; docs/map-follow.md). So the
  // thing to minimise is the *number* of refreshes, not their area: splitting a
  // far-apart pair into two windows would double both the latency and the panel
  // current for no gain.
  //
  // The union is taken over the two rectangles as they are, not over one box
  // size twice: the marker's box is per rung now (MarkerMetrics::box), and the
  // old and new boxes are the same size only because a rung change re-anchors
  // instead of coming through here.
  const int unionX = newX < oldX ? newX : oldX;
  const int unionY = newY < oldY ? newY : oldY;
  const int oldRight = oldX + oldW, newRight = newX + newW;
  const int oldBottom = oldY + oldH, newBottom = newY + newH;
  const int unionW = (newRight > oldRight ? newRight : oldRight) - unionX;
  const int unionH = (newBottom > oldBottom ? newBottom : oldBottom) - unionY;
  const bool shown = renderer.displayBufferWindow(unionX, unionY, unionW, unionH);
  if (!shown) {
    // The framebuffer is already correct, so a full refresh shows the right
    // picture; only the cheap path was unavailable.
    LOG_ERR(kLogTag, "marker window rejected (%d,%d -> %d,%d) -- full refresh", (int)markerDrawnX_, (int)markerDrawnY_,
            (int)sx, (int)sy);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    partialMoves_ = 0;
  } else {
    ++partialMoves_;
  }

  markerDrawnX_ = sx;
  markerDrawnY_ = sy;
  // busyShown_ is deliberately left alone. It latches "the badge is on the
  // panel", and it still is -- a marker move repaints nothing else. Clearing it
  // would only make the next press redraw and re-refresh a badge that is already
  // there. Whatever redraw the badge was announcing clears it (renderViewport()).
  LOG_DBG(kLogTag, "marker move to %d,%d (h%u rel %u), %u/%u before a clean frame", (int)sx, (int)sy,
          (unsigned)headingStep, (unsigned)MapFollow::relativeHeadingStep(headingStep, anchorHeading_),
          (unsigned)partialMoves_, (unsigned)partialMoveBudget());
}

void MapActivity::applyFix(int32_t latE7, int32_t lonE7, uint8_t headingStep, uint8_t seq) {
  updateManualHeadingCapture(headingStep);
  if (screenMode_ == MapScreenMode::Observe) {
    // A rider looking around must not have the frame snatched away from under
    // them by the next fix -- same reasoning as overviewShown_ below, just
    // recorded into observeReturnLatE7_ etc. rather than lastLatE7_/
    // lastLonE7_: those are what panBy() is repointing at the pan target, and
    // clobbering them here would make "Follow mode" render around the wrong
    // spot at the end of a pan.
    observeReturnLatE7_ = latE7;
    observeReturnLonE7_ = lonE7;
    observeReturnHeading_ = headingStep;
    observeReturnSeq_ = seq;
    LOG_DBG(kLogTag, "fix held: observation mode is up");
    return;
  }

  if (overviewShown_) {
    // The rider asked for the whole route and is still looking at it. A fix
    // arrives every five seconds, so redrawing here would snatch the overview
    // away before it could be read -- and cost a full refresh to do it. Record
    // it; the button that leaves the overview renders around this.
    lastLatE7_ = latE7;
    lastLonE7_ = lonE7;
    lastHeading_ = headingStep;
    lastDrawnSeq_ = seq;
    LOG_DBG(kLogTag, "fix held: route overview is up");
    return;
  }

  // No followable frame: the waiting banner, the persisted-fix banner, or a
  // patch that never got saved. All three need the whole picture rebuilt.
  if (!viewportDrawn_ || !markerPatchValid_ || !source_) {
    renderViewport(latE7, lonE7, headingStep, seq);
    return;
  }

  double mercX = 0.0, mercY = 0.0;
  MapProjection::lonLatToMerc(static_cast<double>(latE7) / 1e7, static_cast<double>(lonE7) / 1e7, mercX, mercY);
  int16_t fixX = 0, fixY = 0;
  // Through the projection the frame on the panel was drawn with -- deliberately
  // not a fresh one. The question being asked is "where does this fix fall in
  // the picture already on screen".
  proj_.projectMerc(mercX, mercY, fixX, fixY);

  MapFollow::Request request;
  request.fixX = fixX;
  request.fixY = fixY;
  request.drawnX = markerDrawnX_;
  request.drawnY = markerDrawnY_;
  request.screenWidth = static_cast<int16_t>(renderer.getScreenWidth());
  request.screenHeight = static_cast<int16_t>(renderer.getScreenHeight());
  request.anchorHeadingStep = anchorHeading_;
  request.fixHeadingStep = headingStep;
  request.partialMoves = partialMoves_;
  // With a route loaded the frame is the route's and a heading change is not a
  // reason to redraw it, so the budget is the only thing left that can interrupt
  // a leg -- and it gets the bigger one (docs/route-navigation.md). North-up
  // rotation and a frozen Manual heading are the same situation: the frame's
  // "up" does not track the fix, so a heading drift is not a reason to
  // ReAnchor either (frameOrientationLocked()).
  request.routeHoldsFrame = frameOrientationLocked();
  request.partialMoveBudget = partialMoveBudget();
  // Both off the rung on the panel, not off MapFollow's fallback constants:
  // what a pixel is worth in ground metres, and how big the marker is, are the
  // two things that change down the ladder (MapViewport::ZoomStep::minMovePx,
  // MarkerMetrics::ring).
  request.minMovePx = static_cast<int16_t>(MapViewport::zoomStepAt(zoomStep()).minMovePx);
  request.keepInMarginPx = static_cast<int16_t>(markerMetrics().ring + MapFollow::kKeepInSlackPx);

  switch (MapFollow::decide(request)) {
    case MapFollow::Action::Skip:
      // The panel is not touched. The fix is still the newest one, so a later
      // ladder step re-anchors around it and not around the stale one.
      lastLatE7_ = latE7;
      lastLonE7_ = lonE7;
      lastHeading_ = headingStep;
      LOG_DBG(kLogTag, "fix #%u skipped: %d,%d is under %d px from the marker", (unsigned)seq, (int)fixX, (int)fixY,
              (int)MapFollow::kMinMovePx);
      return;
    case MapFollow::Action::MoveMarker:
      lastLatE7_ = latE7;
      lastLonE7_ = lonE7;
      lastHeading_ = headingStep;
      moveMarker(fixX, fixY, headingStep);
      return;
    case MapFollow::Action::ReAnchor:
      LOG_DBG(kLogTag, "fix #%u re-anchors: at %d,%d, heading %u vs frame's %u, %u moves in", (unsigned)seq, (int)fixX,
              (int)fixY, (unsigned)headingStep, (unsigned)anchorHeading_, (unsigned)partialMoves_);
      renderViewport(latE7, lonE7, headingStep, seq);
      return;
  }
}

void MapActivity::renderRouteOverview() {
  if (!source_ || !route_) {
    renderWaiting();
    return;
  }

  const uint32_t startMs = millis();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  MapRouteFit::Result fit;
  if (!route_->computeFit(screenWidth, screenHeight, fit)) {
    // The point array read short, so there is no honest frame to draw around a
    // fragment of a route. Fall back to the ordinary map rather than showing an
    // overview of half a route.
    LOG_ERR(kLogTag, "route fit failed, falling back to the follow map");
    overviewShown_ = false;
    if (hasReceivedAny_) {
      renderCurrent();
    } else {
      renderWaiting();
    }
    return;
  }

  // The fit's answer is the frame the rest of this route is ridden with, not
  // just the frame being drawn now -- docs/route-navigation.md, "The decision".
  // Both halves of it are kept:
  //
  // - **The heading**, so every later reset draws the route the same way up
  //   instead of re-orienting to the rider.
  // - **The rung**, because the fit picked the one this route fits on, and the
  //   first fix arriving used to throw that away and re-render at whatever was
  //   persisted from the last ride. Measured on the panel: an overview at 3 m/px
  //   became 1 m/px on the next fix, one bend of a pass on screen. The rider can
  //   still step the ladder afterwards; that is an instruction, this is a default.
  routeFrameHeading_ = fit.heading;
  routeFrameHeadingValid_ = true;
  if (fit.zoomStep < MapViewport::kZoomStepCount) {
    zoomStep_[static_cast<uint8_t>(mode_)] = fit.zoomStep;
    publishLadders();
  }

  // The anchor is the screen centre, not the marker ladder's rung: an overview
  // has no rider position in it, so there is no look-ahead to reserve
  // (MapRouteFit.h).
  const int16_t anchorX = static_cast<int16_t>(screenWidth / 2);
  const int16_t anchorY = static_cast<int16_t>(screenHeight / 2);
  proj_.reset(fit.anchorLat, fit.anchorLon, anchorX, anchorY, fit.heading,
              MapViewport::mppMercFor(fit.zoomStep, fit.anchorLat));

  const uint8_t tileZ = MapViewport::kZoomLadder[fit.zoomStep].z;
  const MapViewport::TileRange range = MapViewport::tileRangeFor(proj_, tileZ, screenWidth, screenHeight);
  if (range.count() > MapViewport::kMaxTiles) {
    LOG_ERR(kLogTag, "overview tile range %u..%u x %u..%u = %u tiles, over the 3x3 worst case", range.col0, range.col1,
            range.row0, range.row1, range.count());
  }

  renderer.clearScreen();
  // The two reserved bands keep place labels clear of the button-hint row and
  // the side hints, both of which are drawn after the map and would cover a name
  // (GfxRendererCanvas). kScaleMarginBottom is the same clearance line the scale
  // bar and the busy badge already bottom out on.
  GfxRendererCanvas canvas(renderer, kMapContentTop, kScaleMarginBottom, kSideHintReservedPx);

  MapViewState view;
  view.markerX = anchorX;
  view.markerY = anchorY;
  view.heading = static_cast<MapHeading>(fit.heading & 0x0F);
  // Same rule as the follow frame, from the rung the fit chose.
  view.drawBuildings = MapViewport::kZoomLadder[fit.zoomStep].buildings;
  view.drawBuiltUp = MapViewport::kZoomLadder[fit.zoomStep].builtUp;
  view.maxLabels = MapViewport::kZoomLadder[fit.zoomStep].maxLabels;

  const uint32_t missing = drawMapLayers(range, canvas, view, nullptr, {}, &nearestPlaces_);
  // North still rotates with the frame -- the overview is drawn at the fit's
  // heading, not north-up, so the compass is the only thing that says which way
  // the picture is turned.
  drawCompass(fit.heading);
  drawHeaderStatus();
  drawMapScale();

  if (SETTINGS.mapDebugInfo) {
    const int linePitch = renderer.getLineHeight(UI_10_FONT_ID) + kDebugPad * 2;
    const int line1Y = kTextTopY;
    const int line2Y = line1Y + linePitch;
    char line[80];
    snprintf(line, sizeof(line), "%s", route_->name());
    drawDebugLine(line1Y, line);
    // Says out loud when the ladder could not hold the whole route, because a
    // frame showing the middle of a route looks exactly like one showing all
    // of a shorter route.
    if (fit.fits) {
      snprintf(line, sizeof(line), "%lu pts  z%u %.0fm/px  h%u", static_cast<unsigned long>(route_->pointCount()),
               static_cast<unsigned>(fit.zoomStep), MapViewport::kZoomLadder[fit.zoomStep].mpp,
               static_cast<unsigned>(fit.heading));
    } else {
      snprintf(line, sizeof(line), "%lu pts  z%u  %s", static_cast<unsigned long>(route_->pointCount()),
               static_cast<unsigned>(fit.zoomStep), tr(STR_MAP_ROUTE_PARTIAL));
    }
    drawDebugLine(line2Y, line);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_MAP_OPTIONS), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  drawZoomSideHints();

  // No marker and no follow state. There is no fix in this frame, and a marker
  // at the screen centre would claim the rider is standing in the middle of
  // their own route. viewportDrawn_ stays false, so the next fix that arrives
  // after the rider leaves the overview does a full reset rather than trying to
  // move a marker that was never drawn.
  viewportDrawn_ = false;
  markerPatchValid_ = false;
  overviewShown_ = true;

  LOG_INF(kLogTag, "route overview: heading %u, zoom step %u, %lu tiles, %lu missing, %lu ms, %s",
          static_cast<unsigned>(fit.heading), static_cast<unsigned>(fit.zoomStep),
          static_cast<unsigned long>(source_->tilesOpened()), static_cast<unsigned long>(source_->tilesUnavailable()),
          static_cast<unsigned long>(millis() - startMs), fit.fits ? "whole route" : "too long for the ladder");
  (void)missing;

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  busyShown_ = false;
}

uint32_t MapActivity::drawMapLayers(const MapViewport::TileRange& range, IMapCanvas& canvas, const MapViewState& view,
                                    MapRenderTiming* timing, MapLayerBits knownBadLayers,
                                    MapNearestPlaces* nearestOut) {
  MapTileSource::Config config;
  config.rootDir = kTileRoot;
  config.z = range.z;
  config.col0 = range.col0;
  config.row0 = range.row0;
  config.col1 = range.col1;
  config.row1 = range.row1;
  // The mode filter. Same tiles, same bytes off the card, different classes
  // drawn -- never a different tile set (docs/map-data-spec.md, "Mode is a
  // render-time filter").
  config.classMask = modeMasks_.forMode(mode_);
  // The screen test: geometry whose bbox cannot reach the panel is dropped in
  // the source, before its points are projected (MapTileSource::Config). The
  // margin comes off the compiled style, so a wider road in mapstyle.json
  // widens it with no code change.
  config.screenWidth = static_cast<int16_t>(renderer.getScreenWidth());
  config.screenHeight = static_cast<int16_t>(renderer.getScreenHeight());
  config.rejectMarginPx = mapStyleMaxStrokePx(kDefaultMapStyle);
  // Card time, so a slow frame can be split into "the card was slow" and "the
  // arithmetic was slow" -- the two have entirely different fixes.
  config.nowUs = &cardClockUs;
  // Layers a previous attempt at this same frame found corrupt. Empty on the
  // first attempt (MapTileSource::Config::knownBadLayers).
  config.knownBadLayers = knownBadLayers;
  source_->begin(config);

  // kDefaultMapStyle is the compiled data/mapstyle.json (MapStyleDefaults.h,
  // generated by scripts/gen_mapstyle.py). Nothing overrides it at runtime;
  // the device reads no style file off the card.
  //
  // The route rides along as a second source, re-read from the card on every
  // reset and never held in RAM (IMapRouteSource.h). nullptr when the rider
  // skipped the picker, and then the route pass costs nothing at all.
  //
  // Place names ride along on the same walk. `labels_` is allocated once, in
  // onEnter(), and is null when the style draws no labels -- in which case
  // MapRenderer skips the whole pass rather than allocating anything here
  // (MapLabels.h).
  MapRenderer::render(canvas, *source_, view, kDefaultMapStyle, route_.get(), timing, nearestOut, labels_.get());

  // Hatch after the geometry, because which tiles are missing is only known
  // once the source has tried to open them, and asking up front would cost a
  // third read of every tile in the range. A missing tile's own area carries
  // no geometry -- tiles do not overlap -- so the only thing hatch can cover
  // is the marker, which goes back on top afterwards.
  const uint32_t missing = source_->unavailableMask();
  if (missing != 0) {
    // Tiles worth asking a phone for: hatched here, and not already refused by
    // the supplier. Counted in the same pass that hatches them -- the loop
    // already has each tile's real (z, col, row) in hand, and a second walk to
    // work out the same thing would be a second walk for nothing.
    uint32_t fetchable = 0;
    for (uint32_t index = 0; index < range.count() && index < 32; ++index) {
      if ((missing & (1u << index)) == 0) continue;
      const uint32_t col = range.colAt(index);
      const uint32_t row = range.rowAt(index);
      MapHatch::drawTile(canvas, proj_, range.z, col, row);
      // Before record(), never after: record() adds the tile at count 1 with
      // refused false, so asking afterwards would count a tile the supplier
      // refused ten minutes ago as fresh and beg for it again.
      // Refusals expire on a per-tile schedule, so this is a question about now,
      // not a permanent verdict: a tile the CDN has built since the phone said
      // `skip` counts as fetchable again (MissingTilesStore, `refusals`).
      if (!MISSING_TILES.isRefused(range.z, col, row, millis())) ++fetchable;
      MISSING_TILES.record(range.z, col, row);
    }
    // Published, not acted on here: the ask goes out from loop(), which is
    // where the rate cap and the link state live. A render must not start a
    // BLE conversation half-way through drawing a frame.
    autoSyncWantCount_ = fetchable;
    // No marker restore here: the caller draws the marker after this returns, so
    // the hatch cannot bury it. Drawing the style's puck here as well would only
    // leave it peeking out from under a smaller mode marker.

    // Arm only once: a re-hatch of tiles already on the list leaves isDirty()
    // false (MissingTilesStore's own account of a count-only change), and
    // re-arming on every one of those would mean a coverage gap the rider
    // sits in for ten minutes never actually saves. The first genuinely new
    // tile starts the clock; it is not pushed out further after that.
    if (MISSING_TILES.isDirty() && missingTilesSaveDueMs_ == 0) {
      missingTilesSaveDueMs_ = millis() + kMissingTilesSaveIntervalMs;
    }
  }
  return missing;
}

uint8_t MapActivity::frameHeadingFor(uint8_t fixHeadingStep) const {
  if (routeHoldsFrame()) return routeFrameHeading_;
  if (SETTINGS.mapRotationMode == CrossPointSettings::MAP_ROTATION_NORTH_UP) return 0;
  if (SETTINGS.mapHeadingMode == CrossPointSettings::MAP_HEADING_MANUAL) return frozenManualHeading_;
  return fixHeadingStep;
}

void MapActivity::updateManualHeadingCapture(uint8_t fixHeadingStep) {
  if (SETTINGS.mapHeadingMode != CrossPointSettings::MAP_HEADING_MANUAL) {
    manualHeadingCaptured_ = false;
    return;
  }
  if (manualHeadingCaptured_) return;
  frozenManualHeading_ = fixHeadingStep;
  manualHeadingCaptured_ = true;
}

void MapActivity::renderViewport(int32_t latE7, int32_t lonE7, uint8_t headingStep, uint8_t seq) {
  LOG_DBG(kLogTag, "renderViewport start: lat=%d lon=%d heading=%u seq=%u", (int)latE7, (int)lonE7,
          (unsigned)headingStep, (unsigned)seq);
  // Whatever got here -- a button, a console command, the menu's Refresh -- asked
  // for the ordinary map, so the overview is over.
  overviewShown_ = false;
  if (!source_) {
    renderWaiting();
    return;
  }

  // Remembered so a ladder step can re-render around the same fix. A zoom
  // step re-anchors the viewport on the marker, which is exactly this call
  // with a different mpp -- otherwise zooming out shows more of wherever the
  // marker has drifted to instead of more of the road ahead
  // (docs/map-data-spec.md).
  lastLatE7_ = latE7;
  lastLonE7_ = lonE7;
  lastHeading_ = headingStep;

  const uint32_t startMs = millis();
  const uint32_t heapBefore = ESP.getFreeHeap();

  const double lat = static_cast<double>(latE7) / 1e7;
  const double lon = static_cast<double>(lonE7) / 1e7;

  const uint8_t tileZ = MapViewport::kZoomLadder[zoomStep()].z;
  const int16_t markerY = MapViewport::markerYForStep(markerStep());
  // Track-up: the fix's heading is "up" on this frame. That is what makes the
  // marker ladder mean look-ahead at all -- with the map pinned north-up, the
  // road ahead only lands in the space above the marker when the rider happens
  // to be heading north. It is also what decides which way the map looks for
  // the whole life of this frame: nothing rotates it again until the next
  // viewport reset, and the marker's own arrow is drawn relative to it
  // (docs/map-follow.md, "The heading decides the frame, once").
  //
  // Assumed track-up throughout the design (docs/roadmap.md's "Map rotation
  // model", firmware-implementation-plan.md's follow-up list) and confirmed
  // with the user 2026-08-05.
  //
  // **Track-up means the route, not the rider, once a route is loaded**
  // (docs/route-navigation.md, "The decision"). The rider's heading still
  // reaches the marker's arrow through relativeHeadingStep() below; what it no
  // longer does is turn the map. Without this a keep-in reset would quietly
  // re-orient to whatever the rider was doing at that moment, which is the
  // rotating map the frozen frame exists to stop.
  const uint8_t frameHeading = frameHeadingFor(headingStep);
  proj_.reset(lat, lon, MapViewport::kAnchorScreenX, markerY, frameHeading, MapViewport::mppMercFor(zoomStep(), lat));

  const MapViewport::TileRange range =
      MapViewport::tileRangeFor(proj_, tileZ, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (range.count() > MapViewport::kMaxTiles) {
    // A count above 3x3 is a bug in the range arithmetic, not a state.
    LOG_ERR(kLogTag, "tile range %u..%u x %u..%u = %u tiles, over the 3x3 worst case", range.col0, range.col1,
            range.row0, range.row1, range.count());
  }

  renderer.clearScreen();
  // The two reserved bands keep place labels clear of the button-hint row and
  // the side hints, both of which are drawn after the map and would cover a name
  // (GfxRendererCanvas). kScaleMarginBottom is the same clearance line the scale
  // bar and the busy badge already bottom out on.
  GfxRendererCanvas canvas(renderer, kMapContentTop, kScaleMarginBottom, kSideHintReservedPx);

  MapViewState view;
  view.markerX = MapViewport::kAnchorScreenX;
  view.markerY = markerY;
  // The same heading proj_ was rotated by. MapRenderer draws direction glyphs
  // in raw screen direction (MapRenderer.cpp's kHeadingDir, not
  // rotation-aware), so anything but agreement here has the two disagreeing
  // about which way is up.
  view.heading = static_cast<MapHeading>(frameHeading & 0x0F);
  // Buildings are a rung decision (MapViewport::ZoomStep::buildings): only the
  // closest rung draws them, and on every other rung the layer is never opened.
  view.drawBuildings = MapViewport::kZoomLadder[zoomStep()].buildings;
  view.drawBuiltUp = MapViewport::kZoomLadder[zoomStep()].builtUp;
  view.maxLabels = MapViewport::kZoomLadder[zoomStep()].maxLabels;

  // Per-layer timing, so a slow reset can be attributed to a layer rather than
  // to the frame (docs/optimization/01-render-pipeline.md, step 1). Costs one
  // millis() call per layer and changes no pixel.
  MapRenderTiming timing;
  timing.nowMs = &renderClockMs;
  uint32_t missing = drawMapLayers(range, canvas, view, &timing, {}, &nearestPlaces_);

  // A layer's checksum is now folded out of the record stream, so corruption is
  // found *after* its records have been drawn (MapTileReader::layerCheck). When
  // that happens the framebuffer holds geometry decoded from bad bytes, and the
  // only way to take it back off is to draw the frame again without that layer.
  //
  // One retry, not a loop: the second attempt hatches every pair the first one
  // failed, and a card corrupting a *different* layer on the very next read is
  // not worth a third pass. Expected never to fire -- corruption is rare, which
  // is the whole reason the read was halved
  // (docs/optimization/02-tile-io.md).
  if (source_->corruptLayers() > 0) {
    const MapLayerBits bad = source_->failedLayerMask();
    LOG_ERR(kLogTag, "%lu corrupt layer(s) drawn (mask 0x%llx%016llx) -- redrawing without them",
            static_cast<unsigned long>(source_->corruptLayers()), static_cast<unsigned long long>(bad.hi),
            static_cast<unsigned long long>(bad.lo));
    renderer.clearScreen();
    missing = drawMapLayers(range, canvas, view, &timing, bad, &nearestPlaces_);
  }

  // Outside IMapCanvas: screen furniture, not map data, so it lands on top
  // regardless of what the hatch above covered. Rotated to this frame's
  // heading, which is the only heading it is ever correct for.
  // The frame's heading, not the fix's: the compass says which way the picture
  // is turned, and with a route holding the frame that is the route's direction.
  drawCompass(frameHeading);
  drawHeaderStatus();
  drawMapScale();

  // Does not count the marker or its patch save, both of which happen after the
  // readout is composed -- this is the tile-and-geometry cost, which is the one
  // worth watching.
  const uint32_t elapsedMs = millis() - startMs;
  const uint32_t heapAfter = ESP.getFreeHeap();

  // Debug readout, kept from the BLE checkpoint: the raw values driving the
  // marker, plus what the viewport reset actually cost. Off by default
  // (SETTINGS.mapDebugInfo) -- diagnostic text, not something a rider needs.
  if (SETTINGS.mapDebugInfo) {
    const int linePitch = renderer.getLineHeight(UI_10_FONT_ID) + kDebugPad * 2;
    const int line1Y = kTextTopY;
    const int line2Y = line1Y + linePitch;
    const int line3Y = line2Y + linePitch;
    const int line4Y = line3Y + linePitch;
    char line[80];
    snprintf(line, sizeof(line), "%.5f %.5f h%u #%u", lat, lon, headingStep, seq);
    drawDebugLine(line1Y, line);
    snprintf(line, sizeof(line), "%s z%u m%u %lut %luw %lums", mapRideModeName(mode_), zoomStep(), markerStep(),
             static_cast<unsigned long>(source_->tilesOpened()), static_cast<unsigned long>(source_->waysEmitted()),
             static_cast<unsigned long>(elapsedMs));
    drawDebugLine(line2Y, line);
    if (routePath_[0] != '\0' && !route_) {
      // The rider picked a route and there is none on screen. The picker only
      // checks each file's header, so a route whose point array fails its own
      // crc gets this far -- and an empty map with no explanation reads as a
      // bug in the route feature rather than as a broken file. Shares line 3
      // with the notice above, which is about a different session state and
      // cannot be up at the same time.
      snprintf(line, sizeof(line), "%s", tr(STR_MAP_ROUTE_REFUSED));
      drawDebugLine(line3Y, line);
    }

    // Enough to see that a file push happened and whether it landed. No new
    // screen and no refresh of its own: this rides whatever redraw the map
    // was going to do anyway, because an e-ink refresh costs the better part
    // of two seconds and a byte counter is not worth one.
    transfer_.formatStatus(line, sizeof(line));
    if (line[0] != '\0') drawDebugLine(line4Y, line);
  }

  LOG_DBG(kLogTag,
          "reset z%u col %u..%u row %u..%u: %lu tiles ok, %lu missing (mask 0x%lx), %lu ways, %lu filtered, "
          "%lu places, %lu bytes",
          range.z, range.col0, range.col1, range.row0, range.row1, static_cast<unsigned long>(source_->tilesOpened()),
          static_cast<unsigned long>(source_->tilesUnavailable()), static_cast<unsigned long>(missing),
          static_cast<unsigned long>(source_->waysEmitted()), static_cast<unsigned long>(source_->waysFiltered()),
          static_cast<unsigned long>(source_->placesEmitted()), static_cast<unsigned long>(source_->bytesRead()));
  // Where the frame went. `points` is every point handed to the projection this
  // frame, which is the render path's own unit of work; the per-layer times say
  // which layer spent it.
  LOG_DBG(kLogTag,
          "render %lu ms: landuse %lu, buildings %lu, water %lu, roads %lu, route %lu, places %lu, labels %lu; "
          "%lu points projected, %lu ways off screen, %lu ms in the card, %lu crc32 skipped, "
          "%lu cells skipped (%lu KB)",
          static_cast<unsigned long>(timing.landuseMs + timing.buildingsMs + timing.waterMs + timing.roadsMs +
                                     timing.routeMs + timing.placesMs + timing.labelsMs),
          static_cast<unsigned long>(timing.landuseMs), static_cast<unsigned long>(timing.buildingsMs),
          static_cast<unsigned long>(timing.waterMs), static_cast<unsigned long>(timing.roadsMs),
          static_cast<unsigned long>(timing.routeMs), static_cast<unsigned long>(timing.placesMs),
          static_cast<unsigned long>(timing.labelsMs), static_cast<unsigned long>(source_->pointsProjected()), static_cast<unsigned long>(source_->waysOffScreen()),
          static_cast<unsigned long>(source_->ioUs() / 1000u), static_cast<unsigned long>(source_->crc32Skipped()),
          static_cast<unsigned long>(source_->cellsSkipped()),
          static_cast<unsigned long>(source_->bytesSkippedByIndex() / 1024u));
  LOG_DBG(kLogTag, "heap: %lu before tile load, %lu after, delta %ld; framebuffer ready in %lu ms",
          static_cast<unsigned long>(heapBefore), static_cast<unsigned long>(heapAfter),
          static_cast<long>(heapBefore) - static_cast<long>(heapAfter), static_cast<unsigned long>(elapsedMs));

  // Pushed so the console's `info` and `tiles` commands report this reset's
  // real numbers. Nothing polls MapTileSource for this; it is only ever
  // pushed here, right after a reset, which is the only moment the numbers
  // are current.
  MapTileRangeSnapshot rangeSnapshot;
  rangeSnapshot.valid = true;
  rangeSnapshot.z = range.z;
  rangeSnapshot.col0 = range.col0;
  rangeSnapshot.row0 = range.row0;
  rangeSnapshot.col1 = range.col1;
  rangeSnapshot.row1 = range.row1;
  rangeSnapshot.unavailableMask = missing;
  consoleState_.setTileRange(rangeSnapshot);

  // The same walk, one field further: each tile's content identity, which the
  // header parse already put in RAM. This is the only moment on the device where
  // it is free, so the freshness check reads it off the render rather than
  // opening every tile again (docs/tile-freshness.md).
  heldTiles_ = MapHeldTiles{};
  heldTiles_.valid = true;
  for (uint32_t index = 0; index < range.count() && index < MapHeldTiles::kMaxEntries; ++index) {
    // A tile that did not open has no content to compare, and saying it is
    // held at content 0 would have the phone report it stale forever. It is
    // already on the missing path, which is where it belongs.
    if ((missing & (1u << index)) != 0) continue;
    const uint32_t contentId = source_->contentIdAt(index);
    if (contentId == 0) continue;
    MapHeldTiles::Entry& e = heldTiles_.entries[heldTiles_.count++];
    e.z = range.z;
    e.col = range.colAt(index);
    e.row = range.rowAt(index);
    e.contentId = contentId;
  }
  consoleState_.setHeldTiles(heldTiles_);
  // Left where the tile sync screen can find it: that screen is where a rider
  // deliberately spends data, and it has no viewport of its own to read a
  // content_id from (LastHeldTiles.h).
  g_lastHeldTiles = heldTiles_;
  consoleState_.setRenderStats(source_->tilesOpened(), source_->tilesUnavailable(), source_->waysEmitted(),
                               source_->bytesRead(), source_->waysFiltered());
  consoleState_.setZoomInfo(zoomStep(), range.z, MapViewport::kZoomLadder[zoomStep()].mpp);

  // Composited last, over the map's own bottom-edge pixels rather than into
  // reserved space -- same idea as the debug readout at the top of the
  // screen (drawDebugLine() above): the map fills the whole viewport, there
  // is no margin set aside for chrome, so UI text overlays whatever tiles
  // were there.
  drawMapButtonHints();

  // The marker goes on **last**, and its patch is taken immediately before it.
  // Everything the marker can sit over -- map, hatch, compass, readout, button
  // hints -- is already in the framebuffer, so the patch holds the real
  // background and restoring it later erases the marker with nothing left
  // behind. Draw it any earlier and a marker low on the screen would come back
  // with the hints painted through it.
  anchorHeading_ = frameHeading;
  markerDrawnX_ = view.markerX;
  markerDrawnY_ = markerY;
  partialMoves_ = 0;
  markerPatchValid_ = saveMarkerPatch(markerDrawnX_, markerDrawnY_);
  if (!markerPatchValid_) {
    // Follow is off until the next reset gets one; every fix redraws in full.
    // Correct picture, expensive picture.
    LOG_ERR(kLogTag, "marker patch unavailable -- fixes will redraw in full");
  }
  // Not in Observe: the anchor here is a pan target the rider chose to look
  // at, not a GPS fix, and a marker glyph on it would claim otherwise.
  // Relative heading 0: this frame is drawn track-up for this very fix, so the
  // arrow points straight up by construction.
  if (screenMode_ != MapScreenMode::Observe) {
    drawPositionMarker(markerDrawnX_, markerDrawnY_, 0, mode_);
  }

  // The persisted-fix frame carries a banner only a full redraw can clear, so it
  // is deliberately not followable (applyFix()).
  viewportDrawn_ = !showingPersistedFix_;

  // Timed above, deliberately: the gate is how long the framebuffer takes to
  // be ready, not how long the panel takes to show it.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  busyShown_ = false;  // this frame painted over the badge
}
