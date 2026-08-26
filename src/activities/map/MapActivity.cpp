#include "MapActivity.h"

#include <BlePositionServer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MapPointMarks.h"
#include "MapPointShards.h"
// APP_STATE.showBootScreen: the quick-resume-sleep decision, read in onExit().
#include "CrossPointState.h"
#include "GfxRendererCanvas.h"
#include "HeldTilesStore.h"
#include "HikeIcons.h"
#include "MapFollow.h"
#include "MapHatch.h"
// missingTileAnchorFromLastFix(), for `fake` -- it seeds around the same origin
// the sync screen's fetch order uses, so the seeded tiles land where that
// screen's grid window will look for them.
#include "MapMarkerMetrics.h"
#include "MapMissingAnchor.h"
#include "MapPowerStatsProvider.h"
#include "MapRenderer.h"
#include "MapRouteFit.h"
#include "MapStyleDefaults.h"
#include "MapStyleTable.h"
#include "MapViewport.h"
#include "MissingTilesConsoleSource.h"
#include "MissingTilesStore.h"
#include "PinGeo.h"
#include "PinIcons.h"
#include "PinLabels.h"
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

// How long Up/Down must be held in Observe before the press stops meaning a
// pan and starts meaning a zoom step. The map screen has no spare button
// (MapActivity.h, "There is no spare button"), so the zoom ladder Observe
// gave up to the direction pad comes back on a hold of the same two buttons
// that carry it in Follow -- Up in, Down out, in both modes.
//
// 600 ms sits between the two long-press thresholds this firmware already
// uses -- KeyboardEntryActivity.h:122's 500 ms and RecentBooksActivity.cpp:18's
// 1000 ms. Picked, not measured: open until a hand on the device says whether
// a pan feels sticky at this value or a zoom fires by accident.
constexpr uint32_t kObserveZoomHoldMs = 600;

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

// A starvation bound on the deferral below (loop()) was considered and
// deliberately rejected. Reason, found after this file's first version of
// the deferral was written: a render landing mid-transfer is not merely a
// wasted waveform, it is **reproducibly fatal to the fetch** -- forcing a
// redraw mid-transfer on real hardware (zoom pressed on purpose to provoke
// one) killed the transfer and the phone reported `link lost`. A bound that
// eventually forces that same render is a scheduled kill, not a safety net,
// so there is no bound: the redraw waits for transfer_.status().active to
// go false, however long that takes. See docs/missing-tiles.md, "The settle
// can expire mid-transfer" for the starvation case this leaves open.
//
// A bound belongs back here once a mid-transfer render is no longer fatal.
// What kills the link, measured 2026-08-14: 57/57 disconnects on a live ride
// came back as gatt_status 8 (supervision timeout), against the 4 s the
// transfer-active parameter set asked for. Raised to 20 s -- see
// kConnParamsFastTimeoutUnits, BlePositionServer.h, and
// docs/power-management.md, "T6.2". A shared SPI bus between panel and SD
// card was the first hypothesis and was rejected: the HAL serialises SD
// access, and the multi-second waveform wait holds no SPI transaction. What
// is still open is *why* the render stalls the link layer at all, given the
// controller task outranks the activity task -- so the deferral stays until
// the 20 s timeout is confirmed on hardware to end the kills.

// How often the header status row's state is looked at (updateHeaderStatus()).
// Bounds the rate of rssi() calls into the NimBLE host, and is still prompt for
// a link that has dropped.
constexpr uint32_t kHeaderPollMs = 2000;

// And the floor between two repaints caused by nothing but a moving bar count.
// RSSI sitting on a threshold flips the count back and forth, and each flip is
// a real waveform pass on e-ink. A link appearing or dropping ignores this --
// that one is not cosmetic.
constexpr uint32_t kHeaderBarsRepaintMs = 30 * 1000;

// Floor between two repaints of Hike mode's elevation/lat-lon line caused by
// nothing but the fix drifting. Without it, a fix every second or two at
// hiking pace would still spend a waveform pass every poll -- the lat/lon
// digits move on almost every fix, unlike the bars above which sit still most
// of the time.
constexpr uint32_t kHikeLineRepaintMs = 10 * 1000;

// Observe mode's clock granularity, in minutes. The minute tick is the only
// thing that repaints the header while nothing else on the device is happening,
// and run 5 priced it: 60 windowed refreshes an hour, 9.4 s/h of panel time, in
// a state whose whole point is that it is cheap (docs/power-plan.md, run 5).
// Ten minutes makes that six an hour.
//
// The units digit is then wrong nine minutes in ten, so it is not printed --
// `12:5_` rather than `12:57`. docs/map-header-status.md already refuses the
// other trade ("a clock that is confidently wrong ... is worse than no clock");
// withholding the digit is the same rule one step further.
constexpr uint16_t kClockCoarseMinutes = 10;

// How long after a button press the clock shows the exact minute. A rider who
// pressed something is looking at the screen, and the saving only exists during
// the hours nobody is. It also makes the mask self-explaining: the first press
// turns 12:5_ into 12:57.
constexpr uint32_t kClockFineAfterInputMs = 30 * 1000;

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

// The clock sits leftmost in the status row, between the place name and the
// globe slot. Its width is measured from "00:00" at draw time, not guessed --
// same rule the battery percentage's allowance already follows below.
//
// **The slot is reserved whether or not a time is known**, exactly like the
// globe's: a rect that shrank when the clock went away would leave the clock's
// pixels on the panel with nothing to erase them. Before the phone has sent a
// packet carrying a non-zero utc the device has no clock at all (the X4 has no
// RTC), and the slot simply stays blank rather than showing a placeholder --
// "--:--" reads as a fault, blank reads as "not a thing this screen has".
constexpr int kHeaderClockToGlobeGap = 8;

// Top y of text in this row, matching the battery percentage exactly.
// GUI.drawHeader() hands drawBatteryRight() rect.y + 5 and that function draws
// its percentage string at that same y (BaseTheme.cpp:119) -- the +6 it adds is
// for the icon only (:114). So text sits 4px above the icon row, and a clock
// aligned to the icons would sit 4px below the percentage it stands next to.
constexpr int kHeaderTextTopY = kHeaderMarginTop + 5;

// Width the clock slot reserves. Derived from the widest *digit*, not measured
// from a sample string like "00:00": SMALL_FONT is proportional, so "23:59"
// can be wider than "00:00". The clock is right-aligned in its slot, so a
// string wider than the slot overflows to the **left** -- past the opaque
// backing and outside headerStatusRect(), which is what the windowed repaint
// refreshes. Those pixels would then never be erased.
//
// Sum of parts, no kerning: kerning between digits shortens a real string more
// often than it lengthens one, so this is an over-estimate, which is the safe
// direction. Four digit cells plus a colon covers "23:59"; a single-digit hour
// simply sits further right in the same slot.
int headerClockSlotWidth(const GfxRenderer& renderer) {
  int widestDigit = 0;
  for (char digit = '0'; digit <= '9'; ++digit) {
    const char sample[2] = {digit, '\0'};
    widestDigit = std::max(widestDigit, renderer.getTextWidth(SMALL_FONT_ID, sample));
  }
  return widestDigit * 4 + renderer.getTextWidth(SMALL_FONT_ID, ":");
}

// One coordinate, degrees-minutes-seconds -- the format picked over decimal
// degrees, 2026-08-26. Rounds to the nearest whole second through integer
// arithmetic on total seconds, not by rounding degrees/minutes/seconds
// separately: the naive way can print "60" seconds or "60" minutes on a
// carry (e.g. 47.999999... -> "47°59'60\"" instead of "48°00'00\""), and
// integer division/modulo on a single rounded total cannot.
//
// "\xC2\xB0" is U+00B0 (degree sign) as its literal UTF-8 bytes, not a
// compiler escape -- GfxRenderer::drawText expects UTF-8, and spelling the
// bytes out here does not depend on this source file's own encoding or on
// the toolchain's handling of \u in a narrow string literal.
void formatDms(double deg, bool isLat, char* buf, size_t bufSize) {
  const char hemisphere = isLat ? (deg >= 0 ? 'N' : 'S') : (deg >= 0 ? 'E' : 'W');
  const long totalSeconds = std::lround(std::fabs(deg) * 3600.0);
  const long wholeDegrees = totalSeconds / 3600;
  const long remainderSeconds = totalSeconds % 3600;
  const long wholeMinutes = remainderSeconds / 60;
  const long wholeSeconds = remainderSeconds % 60;
  snprintf(buf, bufSize, "%ld\xC2\xB0%02ld'%02ld\"%c", wholeDegrees, wholeMinutes, wholeSeconds, hemisphere);
}

// Hike mode's second header line's text, shared between the draw and the
// windowed-repaint's change check so the two can never format it differently.
// Altitude first, when known -- it is the number a hiker actually asked for;
// the coordinate is always there because lastLatE7_/lastLonE7_ always are,
// once a fix has landed.
void formatHikeLineText(char* buf, size_t bufSize, bool hasAltitude, int16_t altitudeM, double lat, double lon) {
  char latStr[16];
  char lonStr[16];
  formatDms(lat, /*isLat=*/true, latStr, sizeof(latStr));
  formatDms(lon, /*isLat=*/false, lonStr, sizeof(lonStr));
  // Coordinate first, altitude after -- maintainer's order, 2026-08-26: the
  // fix is always known once a fix has landed, altitude is not.
  if (hasAltitude) {
    snprintf(buf, bufSize, "%s %s   %d m", latStr, lonStr, static_cast<int>(altitudeM));
  } else {
    snprintf(buf, bufSize, "%s %s", latStr, lonStr);
  }
}
// GUI.drawHeader() does not clear anything the strip can rely on: BaseTheme
// wipes only an 80px battery box (BaseTheme.cpp:383), but the shipped Lyra
// theme wipes the **full width** of the rect it is handed (LyraTheme.cpp:112).
// Either way the BLE logo+bars sit over live map lines like the compass and
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
// [0, headerBarHeight()), a 1px black separator at its bottom edge
// (headerSeparatorY()), and the map's own content starting only at
// mapContentTop() -- GfxRendererCanvas's minY clips it there, so nothing
// above the line is drawn at all, not drawn-then-covered
// (docs/map-header-status.md).
//
// kHeaderBarHeight mirrors drawHeaderStatus()'s own former per-element
// clear-bottom math (kHeaderMarginTop + 5 + kHeaderRowHeight, the +5 being
// BaseTheme's own internal battery-rect offset) plus the same
// kHeaderExtraMargin breathing room it already used below the icons, plus
// 1 -- so the icon cluster's layout needs no retuning: everything it already
// draws (battery bottom ~28px, BLE strip backing bottom ~31px) fits inside
// with room to spare. This is the single-row height, used unconditionally by
// row one's own layout (place-name centring, the icon strip); the mode-aware
// total lives in MapActivity::headerBarHeight()/headerSeparatorY()/
// mapContentTop(), which add Hike's second line on top of this.
constexpr int kHeaderBarHeight = kHeaderMarginTop + 5 + kHeaderRowHeight + kHeaderExtraMargin + 1;  // 36
constexpr int kHeaderPlaceNameRightGap = 6;  // clearance before the icon cluster's own backing
// 2px past kTextX -- confirmed on hardware 2026-08-11 that the debug
// readout's own left margin read as too tight for this text specifically.
constexpr int kHeaderPlaceNameLeftX = kTextX + 2;

// Hike mode's second header line: elevation and lat/lon, both of which answer
// "where am I" better than a place name at hiking pace, and both of which the
// place-name walk already fails to answer off the road network. Ride and Cycle
// keep the single-row header above untouched -- MapActivity::headerBarHeight()
// is the only thing that reads mode_ here, and it is what
// MapActivity::mapContentTop()/headerSeparatorY() are derived from, so the map
// viewport, the separator and the marker-bounds check all move together.
//
// Exactly one more kHeaderRowHeight, not a new magic number: that row already
// proved it holds a small-font text line plus its own backing pad, so the
// second line reuses it instead of inventing a second constant to retune.
constexpr int kHikeElevationLineHeight = kHeaderRowHeight;
constexpr int kHikeElevationLeftX = kHeaderPlaceNameLeftX;
// Clearance before the mountain icon, separating it from the coordinates --
// judged on the panel 2026-08-26 to want more air than kHeaderPlaceNameRightGap
// gives the icon cluster, hence its own doubled figure rather than that one
// reused. No gap after the icon: it sits directly against the altitude value
// it belongs to, same day's judgement.
constexpr int kHikeIconGapBefore = kHeaderPlaceNameRightGap * 2;

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
  const HeadingVec& dir = kMarkerHeadingDir[headingStep < 16 ? headingStep : 0];
  const HeadingVec perp{-dir.dy, dir.dx};

  // Vertices come from marker-ride.svg (MapMarkerShape.generated.h, baked by
  // scripts/gen_marker_shape.py), scaled by tipLen and rotated into the
  // current heading here -- see that generated file's own comment for why
  // this shape is computed at runtime rather than a baked icon.
  int xs[kMarkerArrowVertexCount];
  int ys[kMarkerArrowVertexCount];
  for (int i = 0; i < kMarkerArrowVertexCount; ++i) {
    const int forward = tipLen * kMarkerArrowForwardPermille[i] / 1000;
    const int right = tipLen * kMarkerArrowRightPermille[i] / 1000;
    xs[i] = cx + dir.dx * forward / 8 + perp.dx * right / 8;
    ys[i] = cy + dir.dy * forward / 8 + perp.dy * right / 8;
  }
  renderer.fillPolygon(xs, ys, kMarkerArrowVertexCount, true);
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
  kickFullClock();
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
  uint32_t want = autoSyncWantCount_;
  autoSyncWantCount_ = 0;
  if (want == 0) {
    // The common case: MapFollow::Action::MoveMarker slid the marker with no
    // re-render (applyFix()), so nothing hatched this tick. Fall back to the
    // last reset's own mask re-checked against the clock, instead of waiting
    // for the next full re-render -- see recheckHatchedTiles() for why that
    // wait can run to 15+ minutes on a straight leg with a route loaded.
    want = recheckHatchedTiles();
    if (want == 0) return;
  }

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

uint32_t MapActivity::recheckHatchedTiles() const {
  // Same scope as a fresh hatch: an overview or the observation screen has no
  // autosync, and lastTileRange_ is left stale rather than cleared while
  // either is up (only renderViewport() writes it), so it must not be trusted
  // here just because valid is still true.
  if (screenMode_ != MapScreenMode::Follow || overviewShown_) return 0;
  if (!lastTileRange_.valid || lastTileRange_.unavailableMask == 0) return 0;

  const MapViewport::TileRange range{lastTileRange_.z, lastTileRange_.col0, lastTileRange_.row0, lastTileRange_.col1,
                                     lastTileRange_.row1};
  const uint32_t now = millis();
  // Same walk and the same 32-tile cap as drawMapLayers()'s hatch loop -- the
  // mask is one bit per index and never carried more than that to begin with.
  uint32_t fetchable = 0;
  for (uint32_t index = 0; index < range.count() && index < 32; ++index) {
    if ((lastTileRange_.unavailableMask & (1u << index)) == 0) continue;
    if (!MISSING_TILES.isRefused(range.z, range.colAt(index), range.rowAt(index), now)) ++fetchable;
  }
  return fetchable;
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

void MapActivity::sendViewportDiagonalIfChanged() {
  const bool subscribed = freeink::BlePositionServer::getInstance().isCommandSubscribed();
  if (!subscribed) {
    // Not an error -- the rider may not have the app open. The reconnect
    // edge below is what makes sure the phone gets a fresh value once it is.
    lastDiagSubscribed_ = false;
    return;
  }

  const uint8_t step = zoomStep();
  const bool justSubscribed = !lastDiagSubscribed_;
  lastDiagSubscribed_ = true;
  if (step == lastSentDiagZoomStep_ && !justSubscribed) return;

  // mpp is ground metres per pixel at this rung; the diagonal in pixels is
  // fixed by the panel's own resolution, orientation-aware on purpose so a
  // future landscape or higher-res device (X4 Pro, X3, ...) needs no change
  // here -- see MapActivity.cpp's other renderer.getScreenWidth()/Height()
  // call in renderViewport(), same reasoning.
  const double mpp = MapViewport::kZoomLadder[step].mpp;
  const double widthPx = static_cast<double>(renderer.getScreenWidth());
  const double heightPx = static_cast<double>(renderer.getScreenHeight());
  const double diagonalM = mpp * std::sqrt(widthPx * widthPx + heightPx * heightPx);

  char line[32];
  snprintf(line, sizeof(line), "DIAG_M %.1f", diagonalM);
  if (!freeink::BlePositionServer::getInstance().sendCommandReply(line)) {
    LOG_ERR(kLogTag, "DIAG_M not delivered");
    return;
  }
  lastSentDiagZoomStep_ = step;
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

void MapActivity::seedFakeTiles(uint16_t missing, uint16_t held, uint16_t& seededMissing, uint16_t& seededHeld) {
  seededMissing = 0;
  seededHeld = 0;

  // Anchored on the persisted last fix, the same origin the sync screen's fetch
  // order uses, so the seeded tiles land where that screen's grid window will
  // look for them (MapMissingAnchor.h). No fix ever means no sensible place to
  // put them.
  const MissingTileAnchor anchor = missingTileAnchorFromLastFix();
  if (!anchor.valid) {
    LOG_ERR(kLogTag, "fake: no last fix, nothing to anchor on");
    return;
  }

  // **Both lists are emptied first, and that is destructive on purpose.**
  // Without it `fake` only adds, so a second run draws its own tiles on top of
  // the first run's and two runs with the same counts give different pictures
  // -- which defeats the whole point of comparing one layout against another.
  // Worse, MissingTilesStore persists to the card, so leftovers survive a
  // reboot and a reflash: a stale seeding bug was still on screen two flashes
  // later (seen on the panel 2026-08-13).
  //
  // The cost is that a rider's real missing-tile list goes with it. Acceptable
  // for a command whose only purpose is dressing the screen for a photograph,
  // and the list rebuilds itself the next time the map hatches anything.
  //
  // forget() one at a time because MissingTilesStore has no clear(): its
  // eviction is by hit count and nothing else ever wanted the whole list gone.
  while (!MISSING_TILES.hits().empty()) {
    const MissingTileHit& h = MISSING_TILES.hits().front();
    MISSING_TILES.forget(h.z, h.col, h.row);
  }
  g_heldTiles.clear();

  // Deterministic, not random: two runs with the same counts must produce the
  // same picture, or comparing one dot size against another means comparing two
  // different layouts as well. The clear above is what makes that true.
  //
  // Spread over all three LODs in rotation, because the dot size is derived
  // from the tile's LOD and a grid of one LOD says nothing about whether the
  // three are still distinguishable (TileSyncActivity::kDotDivisor).
  static constexpr uint8_t kSeedZ[] = {11, 12, 13};
  // Everything is placed in **z11 parent units and then converted down**, not
  // offset in each LOD's own tile units. The first version did the latter, and
  // the picture came back scattered: `+1` at z11 is a whole parent of ground
  // while `+1` at z13 is a sixteenth of one, so the three LODs spread over
  // wildly different areas and the grid was not representative of anything a
  // ride would produce (seen on the panel 2026-08-13).
  const uint8_t z11rank = missingTileTierRank(11);
  const uint32_t baseCol = anchor.col[z11rank];
  const uint32_t baseRow = anchor.row[z11rank];

  // **A ride, not a block.** A rider does not collect a rectangle of tiles;
  // they collect a corridor along a road, and the grid window is placed and
  // shrunk around whatever shape that is (chooseWindow). A block never
  // exercised that, and the picture it produced said nothing about what the
  // screen looks like after an actual trip.
  //
  // A bending corridor of z11 parents, inside the 6x8 window cap
  // (kMaxWindowCols/kMaxWindowRows) so the whole ride stays drawable. Hand-laid
  // rather than computed: a formula that looks like a road is more code than
  // sixteen pairs, and these can be read off the page.
  static constexpr uint8_t kRideCol[] = {0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 5, 4, 4, 3, 3, 2};
  static constexpr uint8_t kRideRow[] = {0, 0, 1, 1, 1, 2, 2, 3, 4, 5, 6, 6, 7, 7, 6, 6};
  static constexpr uint32_t kRideLen = sizeof(kRideCol);

  // **The road is split, not shared.** The near stretch is ground the rider
  // covered and the card holds; the far stretch is where they rode off the edge
  // of what was synced. A fixed split rather than one derived from the counts:
  // the first version started the missing tiles at `held / 3` waypoints in,
  // which wrapped straight back to 0 as soon as the held count covered the
  // whole road -- so `--held 48` put every frame on top of the first dots
  // instead of past the last (seen on the panel 2026-08-13).
  constexpr uint32_t kHeldWaypoints = kRideLen * 2 / 3;

  // Three tiles per waypoint, one per LOD. A count therefore packs the stretch
  // denser rather than stretching it thinner, and the two stretches cannot
  // reach each other whatever the counts are.
  auto seedAt = [&](uint16_t i, uint32_t fromWaypoint, uint32_t span, uint8_t& z, uint32_t& col, uint32_t& row) {
    z = kSeedZ[i % 3];
    const uint32_t wp = fromWaypoint + ((i / 3) % span);
    const uint32_t pc = baseCol + kRideCol[wp];
    const uint32_t pr = baseRow + kRideRow[wp];
    // z11 is the parent itself; z12 and z13 sit inside it, so shift down and
    // pick a sub-cell that varies with i, or every tile of one LOD would land
    // on the same spot and hide the others.
    const uint32_t down = z - 11;
    col = (pc << down) + (down ? (i % (1u << down)) : 0u);
    row = (pr << down) + (down ? ((i / 2) % (1u << down)) : 0u);
  };

  for (uint16_t i = 0; i < held; ++i) {
    uint8_t z = 0;
    uint32_t col = 0;
    uint32_t row = 0;
    seedAt(i, 0, kHeldWaypoints, z, col, row);
    // A content_id that is never 0 -- record() drops those, correctly, because
    // a tile that did not open has no content to vouch for.
    g_heldTiles.record(z, col, row, 0xF0000000u + i);
    ++seededHeld;
  }

  // The far end of the same road. A ride that ran off the edge of what was
  // synced is the realistic way both marks appear at once -- the near stretch
  // is on the card and being checked, the far stretch is not there at all --
  // and it puts them on neighbouring ground rather than in two unrelated
  // corners.
  for (uint16_t i = 0; i < missing; ++i) {
    uint8_t z = 0;
    uint32_t col = 0;
    uint32_t row = 0;
    seedAt(i, kHeldWaypoints, kRideLen - kHeldWaypoints, z, col, row);
    MISSING_TILES.record(z, col, row);
    ++seededMissing;
  }

  LOG_INF(kLogTag, "fake: seeded %u missing, %u held (%lu held total)", static_cast<unsigned>(seededMissing),
          static_cast<unsigned>(seededHeld), static_cast<unsigned long>(g_heldTiles.size()));
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
  // Nothing drawn yet, or everything already answered for. The second case is
  // the steady state on a ride that stays in one place: the store drains as the
  // phone answers, so a device with nothing new to ask about stops asking
  // instead of re-sending the same screenful every ten minutes.
  if (g_heldTiles.pendingCount() == 0) {
    if (logGate) {
      LOG_DBG(kLogTag, "freshness: nothing pending, nothing to ask about");
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
  //
  // The count is advisory, not a contract: the phone trusts `have_total` and
  // the lines it actually receives, and a render landing between this line and
  // the phone's `have` can add a pending tile (FreshnessChecker's HaveReader).
  // One round's worth, matching what `have` will actually list -- a listing
  // runs its blocks back to back on this task and each waits on the peer's
  // confirm (HeldTilesStore::kMaxPerListing). The rest keeps for the next
  // cooldown, which is what the store draining is for.
  const unsigned long pending = static_cast<unsigned long>(g_heldTiles.pendingCount());
  const unsigned long round = pending < HeldTilesStore::kMaxPerListing ? pending : HeldTilesStore::kMaxPerListing;
  char line[48];
  snprintf(line, sizeof(line), "CHECK_TILES %lu fmt %u", round, static_cast<unsigned>(MapTileReader::kFormatVersion));
  if (!freeink::BlePositionServer::getInstance().sendCommandReply(line)) {
    LOG_ERR(kLogTag, "freshness: CHECK_TILES not delivered");
    return;
  }
  freshnessPending_ = true;
  freshnessDeadlineMs_ = now + kFreshnessQuietMs;
  // From the ask, not from the answer: the cap is on how often the device may
  // start a conversation.
  freshnessNextAskMs_ = now + kFreshnessIntervalMs;
  LOG_INF(kLogTag, "freshness: asked about %lu of %lu pending, %lu held", round, pending,
          static_cast<unsigned long>(g_heldTiles.size()));
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

  // Also release any arrival redraw still waiting out loop()'s
  // transfer_.status().active check (the comment block right after
  // kArrivalRedrawSettleMs's definition, and docs/missing-tiles.md, "No
  // bound on that deferral, on purpose"). The check just above this line is
  // exactly the proof that render needs: kAutoSyncQuietMs of the
  // byte counters not moving at all, re-armed on every real byte
  // (the `progress != lastTransferProgress_` branch above), so reaching
  // here means the transfer -- there is only ever one at a time,
  // MapTransferReceiver.h:77 -- is not merely slow, it is not moving.
  //
  // Rendered directly here rather than by clearing the receiver's stuck
  // `active_`: doing that from the activity task would mean writing
  // MapTransferReceiver's state without the host-task handshake detach()
  // uses (waiting on inFrame_, MapTransferReceiver.cpp's detach()) --
  // exactly the two-tasks-mutate-the-same-state shape T3.2 exists to
  // prevent. This only ever reads transfer_.status() (the established
  // pattern every other call site here already uses) and renders on its
  // own account -- the same call loop()'s branch would make once `active`
  // happened to read false there.
  //
  // Covers autosync asks only. An unsolicited push -- a stale tile the
  // phone sends on its own after a freshness check, with no ask
  // outstanding -- never reaches this function: the guard at the top of
  // expireAutoSync() returns immediately when autoSyncPending_ == 0. That
  // path can still leave arrivalRedrawDueMs_ armed forever if its transfer
  // goes silent the same way. Open; not closed here.
  if (arrivalRedrawDueMs_ != 0) {
    arrivalRedrawDueMs_ = 0;
    LOG_INF(kLogTag, "autosync: releasing the arrival redraw stuck behind it");
    showBusy();
    renderCurrent();
  }
}

// rssi() answers 0 on failure (BlePositionServer.cpp:601-605), not a real
// reading -- BlePositionServer.h:325 says 0 dBm never happens on a live link
// -- and bleBarsForRssi(0) clears every negative threshold, so a failed read
// used to draw full signal (4 bars) instead of no signal. Hold the last bar
// count a real reading produced instead of remapping the failure value.
// lastKnownBleBars_ starts at, and is reset to, 0 -- "no reading has
// succeeded yet" draws identically to "0 bars", which is the right picture
// either way.
int MapActivity::resolveBleBars(int8_t rssi) {
  if (rssi != 0) lastKnownBleBars_ = bleBarsForRssi(rssi);
  return lastKnownBleBars_;
}

bool MapActivity::clockIsCoarse() const {
  // Observe only. In Follow the panel is already repainting for the marker, so a
  // per-minute clock costs nothing extra there -- and a rider following a route
  // has more use for the exact minute.
  if (screenMode_ != MapScreenMode::Observe) return false;
  if (clockFineUntilMs_ == 0) return true;
  return static_cast<int32_t>(millis() - clockFineUntilMs_) >= 0;
}

int16_t MapActivity::clockTick(uint32_t& localNowOut) const {
  // One function for both callers, because the repaint decision and the string
  // have to quantise identically. If they disagree, the header either repaints
  // every minute while showing a coarse string, or shows a stale one.
  if (!freeink::BlePositionServer::getInstance().localTimeNow(localNowOut)) return -1;
  const uint16_t minuteOfDay = static_cast<uint16_t>((localNowOut % 86400u) / 60u);
  if (!clockIsCoarse()) return static_cast<int16_t>(minuteOfDay);
  return static_cast<int16_t>(minuteOfDay / kClockCoarseMinutes * kClockCoarseMinutes);
}

void MapActivity::updateHeaderStatus() {
  // Nothing to update before there is a frame to update: the waiting banner
  // draws no header row at all, and painting one onto it would leave a floating
  // status row over a screen with no map.
  //
  // Gated on headerRowDrawn_, not viewportDrawn_. Those two used to be the same
  // flag, but they answer different questions: viewportDrawn_ is "may an
  // incoming fix move the marker on this frame" (followability), and a
  // persisted-fix frame is deliberately not followable while still carrying a
  // real header row. Reusing viewportDrawn_ here froze the row for the entire
  // time the persisted-fix frame is up, which is exactly the case where the
  // rider most wants to know whether the phone is still there -- see
  // docs/map-header-status.md, "The row freezes while the map shows a
  // persisted fix".
  if (!headerRowDrawn_) return;

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
  const int bars = connected ? resolveBleBars(ble.rssi()) : 0;

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

  // The minute rolling over. No rate cap of its own -- a minute *is* the cap,
  // and it is 30x slower than the bars' floor. This is the one condition that
  // fires while nothing else on the device is happening, which is exactly the
  // case a clock has to survive: the phone sends a fix only when the rider
  // moves, so without this the row would freeze at the time of the last
  // movement and quietly lie (docs/map-header-status.md, "The clock").
  uint32_t localNow = 0;
  // Quantised in Observe (see clockTick()), so in that mode this fires six times
  // an hour rather than sixty. Leaving or entering the coarse window also changes
  // the value, which is how a button press repaints the exact minute and how it
  // goes back to coarse afterwards -- one repaint each, both caused by the rider.
  const int16_t nowMinute = clockTick(localNow);
  const bool minuteMoved = nowMinute != drawnClockMinute_;

  // The destination readout. Two classes of change again, and the same split:
  // a destination appearing or going away changes what the row *means* and
  // repaints at once, while the printed value moving is capped at once per 30 s.
  // Pin distance prints in 10 m steps, which at 30 km/h would change about once
  // a second -- and every change is a real waveform pass
  // (../../docs/nearby-menu.md, "The header readout").
  const bool destPresent = hasDestination() && hasReceivedAny_;
  const bool destAppeared = destPresent != drawnDestPresent_;
  const bool destMoved = destPresent && drawnDestPresent_ &&
                         (destQuantisedDistance() != drawnDestDistance_ || destSector() != drawnDestSector_);

  if (!structural && !barsMoved && !minuteMoved && !destAppeared && !destMoved) return;
  if (!structural && !minuteMoved && !destAppeared && !destMoved && now < nextBarsRepaintMs_) return;
  // The quantised value moved and nothing more urgent did: hold the row until
  // the floor is up. This is the whole cap -- without it a rider at road speed
  // repaints the header every second or two.
  if (!structural && !barsMoved && !minuteMoved && !destAppeared && destMoved && now < nextDestRepaintMs_) return;
  if (barsMoved) nextBarsRepaintMs_ = now + kHeaderBarsRepaintMs;
  if (destMoved || destAppeared) nextDestRepaintMs_ = now + kDestRepaintMs;

  int x, y, w, h;

  // The battery rides the minute tick, and only it. Charge moves slowly enough
  // that it has never earned a repaint of its own, and a minute is a fine rate
  // to notice it at. But the battery belongs to GUI.drawHeader(), and **the
  // active theme's drawHeader() clears the full width of the rect it is given
  // before it draws anything** -- LyraTheme.cpp:112 opens with
  // fillRect(rect.x, rect.y, rect.width, rect.height), i.e. all 480 columns of
  // [kHeaderMarginTop, kHeaderMarginTop + kHeaderRowHeight). Only BaseTheme
  // confines itself to an 80px battery box; Lyra is what ships
  // (CrossPointSettings.h, uiTheme = LYRA).
  //
  // Found on hardware 2026-08-15, the expensive way. The first cut of this
  // called drawHeaderStatusStrip() and *then* GUI.drawHeader(), so the clear
  // wiped the clock, the BLE icons and the place name a moment after drawing
  // them, leaving exactly one scanline of each -- the fill's bottom edge (28)
  // sits one row under the shared text baseline (29). See
  // docs/map-header-status.md, "The clock".
  //
  // So the minute tick redraws the whole row through the one function that
  // already gets the order right, and refreshes the whole band. It costs a
  // 480x36 window once a minute (480x58 in Hike, headerBarHeight()) instead of
  // a strip; the alternative is a second copy of drawHeaderStatus()'s
  // ordering that has to be kept in step with it forever.
  // A destination change is a change to the place-name slot, and
  // drawHeaderStatusStrip() does not draw that slot -- only the whole-row path
  // does (and it is the one that gets the clear-then-draw order right).
  if (minuteMoved || destAppeared || destMoved) {
    drawHeaderStatus();  // clear, battery, strip, place name, separator
    x = 0;
    y = 0;
    w = renderer.getScreenWidth();
    // headerBarHeight(), not the file-scope kHeaderBarHeight: in Hike mode
    // drawHeaderStatus() moved the separator down a line, and a window sized
    // to the base height would refresh the framebuffer without ever sending
    // the new separator position to the panel.
    h = headerBarHeight();
  } else {
    headerStatusRect(x, y, w, h);
    drawHeaderStatusStrip();  // records what it drew, for the comparisons above
  }

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
  // The strip the status row owns: the clock, the globe slot, the Bluetooth
  // logo and the signal bars, plus the opaque backing's padding. Deliberately
  // excludes the battery block -- GUI.drawHeader() clears and draws that
  // itself.
  //
  // **The globe's and the clock's slots are always part of this rect, whether
  // or not either is drawn.** A rect that shrank when one went away would
  // leave its pixels on the panel with nothing to erase them.
  const int screenWidth = renderer.getScreenWidth();
  const int batteryX = screenWidth - kHeaderMarginRight - BaseMetrics::values.batteryWidth;
  const int worstCasePercentWidth = renderer.getTextWidth(SMALL_FONT_ID, "100%");
  const int barsRight = batteryX - worstCasePercentWidth - BaseTheme::batteryPercentSpacing - kHeaderGroupGap;
  const int barsLeft = barsRight - kHeaderBleBarsWidth;
  const int logoLeft = barsLeft - kHeaderBtToBarsGap - kHeaderBtLogoWidth;
  const int globeLeft = logoLeft - kHeaderGlobeToBtGap - kHeaderGlobeDiameter;
  const int clockLeft = globeLeft - kHeaderClockToGlobeGap - headerClockSlotWidth(renderer);
  // Battery's real icon top is kHeaderMarginTop + 11, not +5: drawHeader()
  // hands drawBatteryRight() rect.y+5 (BaseTheme.cpp:374), and
  // drawBatteryRight() adds another +6 of its own (:99) before drawing the
  // outline. Missing that second +6 is what put this row 6px above the
  // battery instead of level with it.
  const int batteryIconTop = kHeaderMarginTop + 5 + 6;
  const int iconBottom = batteryIconTop + BaseMetrics::values.batteryHeight;
  const int iconTop = iconBottom - kHeaderIconHeight;

  // The clock's text sits 4px above the icon row (kHeaderTextTopY), so the
  // rect starts at whichever of the two is higher. Content drawn above the
  // rect would be outside what the windowed repaint refreshes.
  //
  // Safe to raise even though the place name shares those rows: this rect is
  // bounded horizontally to clockLeft..barsRight, and drawHeaderPlaceName()
  // truncates the name against clockLeft before drawing it. A box that reaches
  // further than its own glyphs is what erased this row twice already
  // (2026-08-08, see docs/map-header-status.md) -- this one does not.
  const int contentTop = std::min(iconTop, kHeaderTextTopY);
  x = clockLeft - kHeaderBackingPad;
  y = contentTop - kHeaderBackingPad;
  w = (barsRight - clockLeft) + kHeaderBackingPad * 2;
  h = (iconBottom - contentTop) + kHeaderBackingPad * 2;
}

int MapActivity::headerBarHeight() const {
  return kHeaderBarHeight + (mode_ == MapRideMode::Hike ? kHikeElevationLineHeight : 0);
}

int MapActivity::headerSeparatorY() const { return headerBarHeight(); }

int MapActivity::mapContentTop() const { return headerBarHeight() + 1; }

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
  // (mapContentTop()) is what actually stops the map drawing above this, not
  // this line; this is only what a rider sees at the boundary.
  //
  // headerSeparatorY(), not the file-scope kHeaderBarHeight/kHeaderSeparatorY:
  // in Hike mode the bar is one line taller, and the separator belongs at its
  // bottom edge, below the elevation line drawHikeElevationLine() owns.
  renderer.fillRect(0, headerSeparatorY(), screenWidth, 1, true);
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
  // A destination takes this slot from the place name, and the priority is
  // right: a rider who just chose a spring cares less about being near
  // Solosnica than about which way it is and how far
  // (../../../docs/safety-concept.md, "The header readout").
  if (destHeaderText(text, sizeof(text))) {
    drawnDestDistance_ = destQuantisedDistance();
    drawnDestSector_ = destSector();
    drawnDestPresent_ = true;
  } else if (nearestPlaces_.hasFine && nearestPlaces_.hasCoarse) {
    drawnDestPresent_ = false;
    snprintf(text, sizeof(text), "%s, %s", nearestPlaces_.fineName, nearestPlaces_.coarseName);
  } else if (nearestPlaces_.hasFine) {
    drawnDestPresent_ = false;
    snprintf(text, sizeof(text), "%s", nearestPlaces_.fineName);
  } else if (nearestPlaces_.hasCoarse) {
    drawnDestPresent_ = false;
    snprintf(text, sizeof(text), "%s", nearestPlaces_.coarseName);
  } else {
    drawnDestPresent_ = false;
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

// Hike mode's second header line, below drawHeaderPlaceName()'s row and above
// headerSeparatorY(): elevation, when a fix has ever carried one, and the
// rider's own lat/lon -- riderLatE7()/riderLonE7(), not lastLatE7_/lastLonE7_
// directly. Those track whatever is on screen, and panBy() repoints them at
// the pan target while Observe mode is up (riderLatE7()'s own comment); this
// line answers "where is the rider", which does not change just because they
// panned to look around. Ride and Cycle draw nothing here -- mode_ is the
// only gate, and a rider who switches mode picks the line up or loses it on
// the full frame switchMode() already forces.
void MapActivity::drawHikeElevationLine() {
  if (mode_ != MapRideMode::Hike) return;

  // Full text, coords-then-altitude, is still what drawnHikeLineText_ holds
  // for updateHikeElevationLine()'s change check -- unrelated to whether the
  // icon fits below, which is a drawing-layout question, not a "did anything
  // change" one.
  char fullText[sizeof(drawnHikeLineText_)];
  formatHikeLineText(fullText, sizeof(fullText), hasAltitudeReading_, lastAltitudeM_,
                     static_cast<double>(riderLatE7()) / 1e7, static_cast<double>(riderLonE7()) / 1e7);
  strncpy(drawnHikeLineText_, fullText, sizeof(drawnHikeLineText_) - 1);
  drawnHikeLineText_[sizeof(drawnHikeLineText_) - 1] = '\0';

  // Stays clear of the compass's own white halo (drawCompass(), drawn earlier
  // this frame) -- its left edge is the hard right bound for both this row's
  // backing and its content, not the screen edge. A full-width backing drawn
  // after the compass would erase the compass's halo and glyph underneath it
  // -- judged on the panel 2026-08-26. Same reasoning drawHeaderPlaceName()
  // already uses against the icon cluster's headerStatusRect(), just against
  // a different right-hand neighbour.
  const int compassHaloLeft =
      (renderer.getScreenWidth() - kCompassCenterMarginRight) - (kCompassGlyphRadius + kCompassHaloMargin);
  const int maxContentWidth = compassHaloLeft - kHikeElevationLeftX - kHeaderPlaceNameRightGap;
  if (maxContentWidth <= 0) return;

  // Backing first, same reasoning as the icon strip's and the place name's:
  // this rect can land over last frame's separator or last frame's own
  // shorter/longer content, not blank margin.
  renderer.fillRect(0, kHeaderBarHeight, compassHaloLeft, kHikeElevationLineHeight, false);

  // SMALL_FONT_ID, not UI_10_FONT_ID: this is secondary information, the same
  // weight as the clock/battery percentage in row one, not the primary
  // place-name label -- UI_10 judged too large on the panel, 2026-08-26.
  //
  // No +3 correction here, unlike drawHeaderPlaceName()'s: that offset was
  // tuned on hardware for UI_10_FONT_ID centred in row one
  // (kHeaderPlaceNameLeftX's own comment, 2026-08-11) and does not carry over
  // to a different font in a different row -- judged 2-3px too low on the
  // panel at +3, 2026-08-26. The plain centred value still read 1-2px low on
  // the simulator the same day, hence -2 here.
  const int textY = kHeaderBarHeight + (kHikeElevationLineHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2 - 2;

  // Coordinates first, maintainer's order, 2026-08-26. Own truncate-until-fits
  // loop, same shape as drawHeaderPlaceName()'s -- coordinates are the one
  // thing this line always has, so they are what survives when the icon and
  // altitude do not fit, not what gets chopped mid-string to make room.
  char coordsStr[32];
  {
    char latStr[16];
    char lonStr[16];
    formatDms(static_cast<double>(riderLatE7()) / 1e7, /*isLat=*/true, latStr, sizeof(latStr));
    formatDms(static_cast<double>(riderLonE7()) / 1e7, /*isLat=*/false, lonStr, sizeof(lonStr));
    snprintf(coordsStr, sizeof(coordsStr), "%s %s", latStr, lonStr);
  }
  for (size_t len = strlen(coordsStr); len > 0 && renderer.getTextWidth(SMALL_FONT_ID, coordsStr) > maxContentWidth;
       --len) {
    coordsStr[len - 1] = '\0';
  }
  renderer.drawText(SMALL_FONT_ID, kHikeElevationLeftX, textY, coordsStr, true);

  if (!hasAltitudeReading_) return;

  // Mountain icon before the altitude value it belongs to, same reading order
  // a rider already gets from a compass rose or a trailhead sign -- picked
  // over a bare "m" suffix, 2026-08-26 (docs/map-header-status.md, "Hike
  // mode's second line"). Dropped whole, not truncated, when the row is
  // already full of coordinates -- a half-drawn icon or a unit with no
  // number is worse than neither.
  char altStr[16];
  snprintf(altStr, sizeof(altStr), "%d m", static_cast<int>(lastAltitudeM_));
  const int coordsWidth = renderer.getTextWidth(SMALL_FONT_ID, coordsStr);
  const int altWidth = renderer.getTextWidth(SMALL_FONT_ID, altStr);
  const int iconSize = icon_mountain_16.w;
  // No gap between the icon and altWidth below -- the icon sits directly
  // against the value it belongs to.
  const int neededWidth = coordsWidth + kHikeIconGapBefore + iconSize + altWidth;
  if (neededWidth > maxContentWidth) return;

  const int iconX = kHikeElevationLeftX + coordsWidth + kHikeIconGapBefore;
  // -2: centred like the text above, then nudged up the same amount --
  // judged 2px low against the text baseline on the panel, 2026-08-26.
  const int iconY = kHeaderBarHeight + (kHikeElevationLineHeight - iconSize) / 2 - 2;
  // drawMono1bpp(), not drawIcon(): drawIcon() applies a quarter-turn meant
  // for forced-Portrait UI themes, which turned the map's own pin glyphs
  // rot270 on this screen (GfxRenderer.h, drawMono1bpp()'s own comment,
  // measured 2026-08-17). This icon sits over the plain white backing this
  // function already filled, not over live map lines, so the single ink pass
  // below is enough -- no silhouette-then-outline mask needed.
  renderer.drawMono1bpp(icon_mountain_16.bits, iconX, iconY, iconSize, iconSize, true);
  renderer.drawText(SMALL_FONT_ID, iconX + iconSize, textY, altStr, true);
}

// Keeps the hike line honest between full frames -- same windowed-repaint
// shape as updateHeaderStatus(): polled, not checked every tick, and the
// panel is only spent on a real change (drawnHikeLineText_ actually differs),
// rate-capped so a fix arriving every second or two at hiking pace does not
// spend a waveform pass on every one of them.
void MapActivity::updateHikeElevationLine() {
  // Same gate as updateHeaderStatus(): nothing to keep honest before a frame
  // exists that carries a header row at all.
  if (!headerRowDrawn_ || mode_ != MapRideMode::Hike) return;

  const uint32_t now = millis();
  if (nextHikeLinePollMs_ != 0 && now < nextHikeLinePollMs_) return;
  nextHikeLinePollMs_ = now + kHeaderPollMs;

  char candidate[sizeof(drawnHikeLineText_)];
  formatHikeLineText(candidate, sizeof(candidate), hasAltitudeReading_, lastAltitudeM_,
                     static_cast<double>(riderLatE7()) / 1e7, static_cast<double>(riderLonE7()) / 1e7);
  if (strcmp(candidate, drawnHikeLineText_) == 0) return;
  if (now < nextHikeLineRepaintMs_) return;
  nextHikeLineRepaintMs_ = now + kHikeLineRepaintMs;

  drawHikeElevationLine();

  // Windowed, like the row above: the map on the rest of the panel is
  // untouched and a full refresh would cost a second to redraw something a
  // few characters wide.
  if (!renderer.displayBufferWindow(0, kHeaderBarHeight, renderer.getScreenWidth(), kHikeElevationLineHeight)) {
    LOG_ERR(kLogTag, "hike elevation line window rejected");
  }
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

  // The clock, leftmost, when the phone has ever sent a non-zero utc. 24-hour
  // only: SETTINGS.clockFormat exists but its menu entry is hidden on hardware
  // with no RTC (StatusBarSettingsActivity.h:26), so honouring it here would
  // read a setting the rider cannot reach.
  //
  // Right-aligned inside its reserved slot so the colon does not walk
  // sideways between "9:05" and "10:05" -- the slot is sized for the widest
  // string ("00:00"), and a narrower one hangs off its right edge, against
  // the globe, rather than shifting the whole field.
  uint32_t localNow = 0;
  const int16_t clockTickNow = clockTick(localNow);
  if (clockTickNow >= 0) {
    const uint32_t secondsOfDay = localNow % 86400u;
    char clockText[6];
    if (clockIsCoarse()) {
      // "12:5*": the tens of minutes, then a mark standing in for the withheld
      // digit. Not "12:50", which would claim a minute it does not have.
      //
      // The mark was '_' first. **Judged on the panel 2026-08-22 and rejected**:
      // an underscore at SMALL_FONT_ID is invisible against the baseline, so
      // "8:0_" read as "8:0" -- which the maintainer described as looking like a
      // football score rather than a clock. '*' fills the digit's slot and is
      // roughly round, so the hh:mm shape survives. '?' was ruled out on paper
      // for reading as an error rather than as a withheld digit.
      snprintf(clockText, sizeof(clockText), "%u:%u*", static_cast<unsigned>(secondsOfDay / 3600u),
               static_cast<unsigned>((secondsOfDay % 3600u) / 60u / kClockCoarseMinutes));
    } else {
      snprintf(clockText, sizeof(clockText), "%u:%02u", static_cast<unsigned>(secondsOfDay / 3600u),
               static_cast<unsigned>((secondsOfDay % 3600u) / 60u));
    }
    const int slotRight = globeLeft - kHeaderClockToGlobeGap;
    const int textX = slotRight - renderer.getTextWidth(SMALL_FONT_ID, clockText);
    // kHeaderTextTopY, not iconTop: this is text standing next to the battery
    // percentage, and the two rows are 4px apart. headerStatusRect() starts at
    // whichever is higher, so this stays inside the refreshed window.
    renderer.drawText(SMALL_FONT_ID, textX, kHeaderTextTopY, clockText, true);
    // The same quantised value the repaint decision compares against.
    drawnClockMinute_ = clockTickNow;
  } else {
    drawnClockMinute_ = -1;
  }

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
    // A new connection starts owing nothing to the last one's signal.
    lastKnownBleBars_ = 0;
    return;
  }

  const int bars = resolveBleBars(ble.rssi());
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

MapActivity::MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* routePath,
                         bool resumedFromSleep)
    : Activity("Map", renderer, mappedInput), transfer_(kTileRoot), resumedFromSleep_(resumedFromSleep) {
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

  // NimBLEDevice::init() hangs at a low clock, and this screen can be entered
  // from an idle Home screen that is already throttled (verified 2026-08-04,
  // docs/power-management.md). HalPowerManager's BLE_SAFE_FREQ floor only
  // applies once the controller is enabled -- which is what the next line
  // does -- so the window before it needs closing here.
  powerManager.setPowerSaving(false);

  bleStartFailed_ = !freeink::BlePositionServer::getInstance().begin();
  if (bleStartFailed_) {
    LOG_ERR(kLogTag, "BlePositionServer.begin() failed");
  }
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
  headerRowDrawn_ = false;
  markerPatchValid_ = false;
  partialMoves_ = 0;
  screenMode_ = MapScreenMode::Follow;
  pendingEntryCleanRefresh_ = true;
  observeHoldZoomed_ = false;

  // Autosync starts from nothing every time this screen opens. The rate cap in
  // particular is per session on purpose: a rider who left the map and came
  // back is asking for the picture again, and making them wait out a cap armed
  // before they left would look like the feature is off.
  autoSyncWantCount_ = 0;
  autoSyncPending_ = 0;
  autoSyncArrived_ = false;
  autoSyncNextAskMs_ = 0;
  autoSyncDeadlineMs_ = 0;
  // A stale range from whatever was on screen before must not feed
  // recheckHatchedTiles() a mask that no longer matches this session's tile
  // source.
  lastTileRange_ = MapTileRangeSnapshot();
  lastClearedTileSeq_ = 0;
  arrivalRedrawDueMs_ = 0;
  lastTransferProgress_ = 0;
  transferIconShown_ = false;
  drawnLinkConnected_ = false;
  drawnBleBars_ = -1;
  lastKnownBleBars_ = 0;
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

  // Published for the wake-side routing, which has no other way to learn it:
  // routePath_ dies with this activity and the route picker is a separate screen.
  // Not saved here -- enterDeepSleep() writes the whole state file on the way out,
  // and this value is only ever read when lastSleepActivity says the sleep came
  // from the map.
  APP_STATE.lastSleepRoutePath = routePath_;
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
  // What `have` lists and `checked` settles. Wired once, not per reset: the
  // store outlives both this activity and the sync screen, which is the whole
  // reason the accumulation survives leaving the map (HeldTilesStore).
  consoleState_.setHeldTilesStore(&g_heldTiles);
  // Where `fake` lands. Only this screen offers it -- it is the one with the
  // projection and MISSING_TILES.
  consoleState_.setFakeSink(this);
  // The pins come back off the card before anything can ask for them. A failed
  // replay leaves the set empty *and* refuses every save, rather than appending
  // onto a history it never read (MapPins::pinSet).
  pins_.begin();
  consoleState_.setPinsSource(&pins_);
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
  if (mapStyleAnyDrawsPlaceLabels()) {
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

  // The POI marks' own source and file handle. Allocated whether or not a layer
  // is switched on: it is 1.3 KB, it is allocated once, and `Nearby -> Show on
  // map` must not have to allocate on a button press. With no layer on, the
  // render skips it entirely (nearbyCategoryMask_ == 0), so it costs no card
  // read either.
  //
  // Not allocated at all when the rider switched the whole layer off
  // (SETTINGS.mapPointsEnabled): then nothing can open a shard, the `Nearby`
  // row is dimmed, and the 1.3 KB goes back to the heap the map screen is
  // always short of.
  if (mapStyleAnyDrawsPointMarks() && pointsEnabled()) {
    pointFile_ = makeUniqueNoThrow<HalFileSource>();
    if (!pointFile_) {
      LOG_ERR(kLogTag, "OOM: HalFileSource for the point layer");
    } else {
      points_ = makeUniqueNoThrow<MapPointSource>(*pointFile_, proj_);
      if (!points_) {
        LOG_ERR(kLogTag, "OOM: MapPointSource (%u bytes)", static_cast<unsigned>(sizeof(MapPointSource)));
        pointFile_.reset();
      }
    }
  }

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
    //
    // Except on a wake into the map, where that premise is false and the frame is
    // expensive. False, because e-ink is still holding the sleep screen -- the map
    // with its moon -- for the whole boot, which says "where you were, waking up"
    // better than a logo does. Expensive, because the driver promotes the first
    // paint after a wake (_needsInitialFull, Ssd1677Driver.cpp), so this frame's
    // FAST request becomes a whole-panel HALF: measured 1,683 ms on 2026-08-19
    // against 500 ms for the same frame on an ordinary map entry, spent on a
    // picture that lives ~2.2 s before renderViewport() replaces it.
    if (!resumedFromSleep_) {
      renderLoadingTiles();
    }
    renderViewport(lastLatE7_, lastLonE7_, lastHeading_, lastDrawnSeq_);
  } else {
    renderWaiting();
  }
  LOG_DBG(kLogTag, "onEnter done");
}

void MapActivity::onExit() {
  Activity::onExit();

  // Before anything is torn down: on the way into a quick-resume sleep the frame
  // on the panel right now is the frame that sits there for the whole sleep, so
  // the live marker gets swapped for one that does not claim a heading. Gated on
  // the same bool the clean-frame request at the end of this function reads --
  // enterDeepSleep() writes it before goToSleep() runs (main.cpp).
  if (!APP_STATE.showBootScreen) {
    drawSleepMarker();
  }

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
    // Same skip TileSyncActivity::leave() makes: a peer that already let a
    // confirm time out will not hear FETCH_CANCEL either, so the 3 s wait for
    // it here is pure UI freeze on the way out with nothing delivered
    // (docs/ble-review-2026-08.md, "Console flush can freeze the activity
    // task").
    if (freeink::BlePositionServer::getInstance().lastConfirmTimedOut()) {
      LOG_ERR(kLogTag, "autosync: FETCH_CANCEL skipped: last confirm already timed out");
    } else if (!freeink::BlePositionServer::getInstance().sendCommandReply("FETCH_CANCEL")) {
      LOG_ERR(kLogTag, "autosync: FETCH_CANCEL not delivered");
    }
  }
  autoSyncPending_ = 0;
  consoleState_.setSkipObserver(nullptr);
  consoleState_.setStaleObserver(nullptr);
  consoleState_.setStaleTiles(nullptr);
  consoleState_.setFakeSink(nullptr);
  // The pins source is a member of this activity, which main.cpp deletes right
  // after this returns -- the console must not be left pointing into it.
  consoleState_.setPinsSource(nullptr);
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

  // The map leaves a dense frame on the glass, and every screen it can exit
  // into paints FAST (HomeActivity.cpp, TileSyncActivity.cpp,
  // RouteSelectActivity.cpp) -- a differential mode cannot clear this frame, so
  // its ghost would sit under the menu. This used to be `clearScreen()` plus a
  // whole-panel HALF here: a 1,684 ms refresh painting white that the next
  // screen overwrote 500 ms later, for two refreshes and a visible flash
  // between them. Handing the clean forward instead costs one refresh total and
  // the arriving screen is what the rider sees appear.
  //
  // Not requested when a quick-resume sleep is what we are exiting into:
  // SleepActivity keeps the frame on purpose there and only adds a moon to it.
  // enterDeepSleep() has already written that decision to
  // APP_STATE.showBootScreen before goToSleep() runs this (main.cpp), so this is
  // the same bool, read one activity later.
  if (APP_STATE.showBootScreen) {
    renderer.requestCleanNextFrame();
  }
}

void MapActivity::loop() {
  Activity::loop();

  // The wake-into-map guard is spent here rather than at the end of onEnter():
  // reaching a loop() tick proves onEnter() returned, which a hang inside the
  // first render would not. Value-checked so this is one SD write per wake and
  // nothing at all on an ordinary map entry (CLAUDE.md, write throttling).
  if (APP_STATE.mapActivityLoadCount != 0) {
    APP_STATE.mapActivityLoadCount = 0;
    APP_STATE.saveToFile();
    LOG_DBG(kLogTag, "wake-into-map guard cleared");
  }

  // The pin confirmation goes away on its own timer, or on the first button --
  // whichever comes first. Before the popup below gets the input: a rider who
  // presses CONFIRM to open the menu should get the map back under it, not a
  // stale "Camp saved" sitting behind a fresh dialog.
  // A row callback cannot open the next popup itself (see PinPopup), so this is
  // where the one it asked for actually opens -- before the popup below gets this
  // frame's input, so the new popup is the one that sees the next press.
  // A press means the rider is looking at the screen, so Observe's clock shows the
  // exact minute for a while (clockIsCoarse()). Read, not consumed -- the checks
  // below and the input handling further down all still see this frame's press.
  if (mappedInput.wasAnyPressed()) clockFineUntilMs_ = millis() + kClockFineAfterInputMs;

  if (pendingPinPopup_ != PinPopup::None) servicePendingPinPopup();
  if (pendingNearbyPopup_ != NearbyPopup::None) servicePendingNearbyPopup();

  if (pinNoticeUntilMs_ != 0 &&
      (static_cast<int32_t>(millis() - pinNoticeUntilMs_) >= 0 || mappedInput.wasAnyPressed())) {
    clearPinNotice();
  }

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
    // showingPersistedFix_ is in the condition because onEnter() seeds
    // lastDrawnSeq_ = 0 and hasReceivedAny_ = true for the card's last fix. A
    // phone whose rolling counter happens to sit at 0 would then have its first
    // real packet read as "already drawn" and the panel would keep showing the
    // persisted position until the counter moved -- and nothing moves it while
    // the rider is parked (SendPolicy.kt).
    if (!hasReceivedAny_ || showingPersistedFix_ || update.seq != lastDrawnSeq_) {
      hasReceivedAny_ = true;
      showingPersistedFix_ = false;
      lastDrawnSeq_ = update.seq;
      // heading is 0-15 straight off the wire now: the 19-byte packet
      // carries a MapHeading value, so the *2 fudge the old 8-step packet
      // needed is gone (BlePositionServer.h).
      //
      // speed and utc are carried and stored, and nothing reads them yet --
      // auto zoom and the on-screen fix time are later phases. Altitude now
      // feeds Hike mode's header line (drawHikeElevationLine()). Logged so
      // the still-unused fields can be seen arriving before anything depends
      // on them.
      char altStr[8];
      if (update.hasAltitude) {
        snprintf(altStr, sizeof(altStr), "%d", static_cast<int>(update.altitudeM));
        lastAltitudeM_ = update.altitudeM;
        hasAltitudeReading_ = true;
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
      // Same field the BLE fix above stores, so `mapcmd.py pos ... alt <m>`
      // exercises Hike mode's header line the same way a real fix does.
      // Unconditional on `moved`: an altitude-only console update must still
      // register, since altitude has no follow decision of its own to gate it.
      if (consoleState_.hasAltitude()) {
        lastAltitudeM_ = consoleState_.altitudeM();
        hasAltitudeReading_ = true;
      }
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
  // Hike mode's elevation/lat-lon line, kept current the same way: a no-op in
  // Ride/Cycle (mode_ gate inside).
  updateHikeElevationLine();

  // Advertising state and connection parameter requests, once per tick. A
  // restart that failed inside the NimBLE disconnect callback cannot be
  // retried there -- that callback runs on the host task, and the event that
  // clears the usual failure cause is queued on the same task
  // (BlePositionServer.h, "Advertising state"). This task is a different one,
  // so its retry can actually succeed. Costs one bool read when there is
  // nothing to do.
  //
  // transfer_.status().active is read fresh here (drainTransferredTiles()'s
  // own read above is not reused -- it ran before autosync could have started
  // a new transfer this same tick) and is this method's only view into
  // whether a file transfer is moving bytes: the BLE library carries bytes
  // only and does not know MapTransferReceiver exists (BlePositionServer.h,
  // TransferHooks), so the activity answers.
  //
  // Radio is only needed in Follow, or mid-transfer regardless of mode: a fix
  // received while observing does not redraw (applyFix() just records
  // observeReturnLatE7_ -- "Two coordinates, not one",
  // docs/map-observation-mode.md), and autosync is already Follow-only
  // (recheckHatchedTiles()). So Observe with nothing moving has no use for
  // advertising or a live connection -- stop it exactly like onExit() would,
  // and bring it back the moment either condition changes. Never touches
  // bleStartFailed_ here except to set it on a fresh begin(): a genuinely
  // failed init must not be retried every tick, only on the transition that
  // asks for the radio again.
  {
    const bool transferActive = transfer_.status().active;
    const bool needBle = screenMode_ == MapScreenMode::Follow || transferActive;
    auto& ble = freeink::BlePositionServer::getInstance();
    if (needBle) {
      if (!ble.isRunning() && !bleStartFailed_) {
        // The same window onEnter() closes, and here it is not a rare case but
        // the normal one: Observe called ble.end() just above, which removes
        // HalPowerManager's BLE_SAFE_FREQ floor (HalPowerManager.cpp,
        // lowPowerFloorMhz()), so the 3 s idle throttle takes the CPU to 10 MHz
        // while the radio is off. Leaving Observe then asks for the radio back
        // at that clock, and NimBLEDevice::init() hangs there -- the panel
        // freezes, the log stops, and no button on the device can reset it
        // (../../../docs/device-notes.md).
        //
        // Measured 2026-08-22 from a serial capture: throttle to 10 MHz at
        // t+301821 ms, `begin: calling NimBLEDevice::init` at t+301893 ms, and
        // nothing ever again. The hang itself was already known and documented
        // 2026-08-04 (docs/power-management.md); this call site was added later
        // and did not carry the guard onEnter() has.
        powerManager.setPowerSaving(false);
        bleStartFailed_ = !ble.begin();
      }
    } else if (ble.isRunning()) {
      ble.end();
    }
    ble.serviceAdvertising(transferActive);
  }

  // A transfer status line (`RDY`, `OK`, `ERR`) that found the connection's one
  // indication slot held by a command-channel reply. Same task-ownership reason
  // as serviceAdvertising above -- the host task cannot wait for the confirm
  // that frees the slot, because it is the task that delivers it
  // (BlePositionServer.h, sendTransferStatus). A separate call, not folded into
  // serviceAdvertising: that one owns advertising, this is the indication slot.
  // Costs one counter compare per tick when nothing is parked.
  freeink::BlePositionServer::getInstance().flushTransferStatus();

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
  //
  // Bytes may still be landing for the *next* tile of the same fetch when
  // this settle expires -- at the measured ~9.0 kB/s, any tile over ~45 KB
  // outlasts kArrivalRedrawSettleMs (5 s), so the timer armed by the previous
  // arrival routinely expires mid-transfer. Deferring here is not just about
  // wasting a waveform: forcing a redraw mid-transfer has been reproduced on
  // hardware to kill the transfer outright (see the constant block above),
  // so this re-arms the settle for as long as transfer_.status().active
  // stays true, with **no bound of its own** -- nothing in this branch ever
  // forces a redraw if `active` never goes false. expireAutoSync() releases
  // it independently once an outstanding ask proves the transfer has gone
  // silent; an unsolicited push with no ask behind it is still uncovered.
  // See docs/missing-tiles.md, "The settle can expire mid-transfer".
  if (arrivalRedrawDueMs_ != 0 && now >= arrivalRedrawDueMs_) {
    if (transfer_.status().active) {
      arrivalRedrawDueMs_ = now + kArrivalRedrawSettleMs;
    } else {
      arrivalRedrawDueMs_ = 0;
      LOG_INF(kLogTag, "tiles arrived, redrawing");
      showBusy();
      renderCurrent();
    }
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
      // Pan fires on **release**, not on press, because the same two side
      // buttons carry a second meaning here: held past kObserveZoomHoldMs,
      // Up/Down step the zoom ladder instead of panning. A press cannot be
      // read as a pan until it is over, or every zoom would pan first.
      //
      // Left/Right have no hold meaning and could still fire on press, but
      // they do not: half a direction pad answering on press and the other
      // half on release is a difference the hand feels and no comment can
      // explain away.
      //
      // One zoom step per hold, not a repeat. Keeping the button down does
      // nothing more -- the step arms a redraw that blocks loop() for the
      // better part of two seconds anyway (armRedraw()), so a repeat rate
      // would be a fiction, and the ladder is seven rungs wide
      // (MapViewport::kZoomStepCount, 1..45 m/px).
      if (!observeHoldZoomed_ && mappedInput.getHeldTime() >= kObserveZoomHoldMs) {
        if (mappedInput.isPressed(MappedInputManager::Button::Up)) {
          observeHoldZoomed_ = true;
          LOG_DBG(kLogTag, "observe: hold zoom in");
          stepZoom(-1);
        } else if (mappedInput.isPressed(MappedInputManager::Button::Down)) {
          observeHoldZoomed_ = true;
          LOG_DBG(kLogTag, "observe: hold zoom out");
          stepZoom(+1);
        }
      }
      // The release that ends a hold-zoom belongs to the zoom, not to a pan --
      // same idiom as suppressConfirmRelease_ below. Only Up/Down can have
      // zoomed, so only their release is swallowed; a Left/Right tap while
      // Up is still down is an ordinary pan and stays one.
      if (observeHoldZoomed_) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
            mappedInput.wasReleased(MappedInputManager::Button::Down)) {
          observeHoldZoomed_ = false;
          break;
        }
        // Nothing left to release: the latch has outlived its press (a
        // release event that never arrived, an activity re-entry mid-hold).
        // Clearing it here costs nothing and stops it eating the next pan.
        if (!mappedInput.isPressed(MappedInputManager::Button::Up) &&
            !mappedInput.isPressed(MappedInputManager::Button::Down) && !mappedInput.wasAnyReleased()) {
          observeHoldZoomed_ = false;
        }
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) panBy(PanDirection::Up);
      if (mappedInput.wasReleased(MappedInputManager::Button::Down)) panBy(PanDirection::Down);
      if (mappedInput.wasReleased(MappedInputManager::Button::Left)) panBy(PanDirection::Left);
      if (mappedInput.wasReleased(MappedInputManager::Button::Right)) panBy(PanDirection::Right);
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
  // A convenience must not be able to starve the work. Measured on the panel
  // 2026-08-12: the map screen sits at ~54 KB free at the widest rung, and BLE
  // tile transfers keep running while the menu is up. If taking the backdrop
  // would leave less than the reserve, do without it -- the close is then slow,
  // which is strictly better than an allocation failure somewhere else.
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (size + kMenuBackdropHeapReserve > freeHeap) {
    LOG_DBG(kLogTag, "menu backdrop skipped: %u bytes, free heap %u", static_cast<unsigned>(size),
            static_cast<unsigned>(freeHeap));
    return false;
  }
  auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
  if (!buffer) {
    // Not fatal: every close path still has renderCurrent() behind it.
    LOG_ERR(kLogTag, "menu backdrop unavailable: %u bytes, free heap %u", static_cast<unsigned>(size),
            static_cast<unsigned>(freeHeap));
    return false;
  }
  LOG_DBG(kLogTag, "menu backdrop %u bytes (%dx%d), free heap %u", static_cast<unsigned>(size), rect.width, rect.height,
          static_cast<unsigned>(freeHeap));
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
  const int h = renderer.getScreenHeight() - y;
  // A dialog tall enough to reach y == 0 makes this "one window" the whole
  // panel, which is exactly what the comment below says aborts the device.
  // Nothing bounded it until 2026-08-22.
  if (!windowRefreshAffordable(w, h) || !renderer.displayBufferWindow(x, y, w, h)) {
    LOG_ERR(kLogTag, "menu close window rejected: %d,%d %dx%d", x, y, w, h);
    return false;
  }
  // A popup with row actions drew the two side-hint boxes, and those sit outside
  // the dialog -- on the right edge, at the theme's own y. drawMapButtonHints()
  // above has already repainted this screen's own hints over them
  // (drawSideButtonHints() lays a white backing first); this refreshes just that
  // strip so the panel stops showing the popup's arrows.
  //
  // A second small window, never one big one. Refreshing the whole panel here
  // instead aborted the device: displayBufferWindow() allocates a buffer per
  // window inside the driver, and 480x800 does not fit a map screen's heap
  // (measured 2026-08-17 -- Ssd1677Driver::displayWindow -> operator new ->
  // bad_alloc -> terminate, with 38 KB free and a 34 KB largest block).
  if (popupDrewSideHints_) {
    popupDrewSideHints_ = false;
    const Rect hints = GUI.sideButtonHintsRect(renderer);
    if (hints.width > 0 && hints.height > 0 && hints.y < y) {
      if (!renderer.displayBufferWindow(hints.x, hints.y, hints.width, hints.height)) {
        LOG_ERR(kLogTag, "side hint window rejected: %d,%d %dx%d", hints.x, hints.y, hints.width, hints.height);
      }
    }
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
  // 13, not 11: the ladder is Observe/Follow, two zoom rows, Whole route,
  // Pins, Nearby, Mode, three toggles, Refresh and Debug info. A route loaded
  // in Observe reaches all of them, and a reserve that is one short reallocates
  // both vectors on the last push (CLAUDE.md, Resource Protocol 7).
  options.reserve(13);
  values.reserve(13);
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
  // Observe only. In Follow the two side buttons *are* the zoom ladder and a
  // menu row for them would be a second way to press a button the rider is
  // already holding; in Observe those buttons pan, and the ladder is only
  // reachable by holding them (kObserveZoomHoldMs) -- which nothing on the
  // panel says. These rows are where a rider finds out zoom still exists.
  //
  // Hidden at the ends of the ladder rather than shown doing nothing, same
  // rule as Whole route below and the same hard stops stepZoom() enforces.
  int zoomInIdx = -1;
  int zoomOutIdx = -1;
  if (screenMode_ == MapScreenMode::Observe) {
    if (zoomStep() > 0) {
      zoomInIdx = static_cast<int>(options.size());
      options.push_back(tr(STR_MAP_ZOOM_IN));
      values.emplace_back();
    }
    if (zoomStep() + 1 < MapViewport::kZoomStepCount) {
      zoomOutIdx = static_cast<int>(options.size());
      options.push_back(tr(STR_MAP_ZOOM_OUT));
      values.emplace_back();
    }
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
  // Pins. High in the list on purpose: it is the row a rider reaches for while
  // moving ("park here", "camp here"), unlike the settings toggles below it. The
  // value column carries how many are saved, so the rider does not have to open
  // the list to find out (0 is still worth a row -- it is where a pin is made).
  const int pinsIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_PINS));
  {
    char count[8];
    snprintf(count, sizeof(count), "%u", static_cast<unsigned>(pins_.pinCount()));
    values.emplace_back(count);
  }
  const int modeIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_MODE));
  values.push_back(I18N.get(kMapModeIds[static_cast<uint8_t>(mode_)]));
  // Quick toggles for settings the rider wants to flip mid-ride without
  // leaving the map -- zoom/rotation/heading mode. The Settings screen
  // entries for the same three fields (SettingsList.h) decide what the map
  // opens with, next time; this menu changes the same CrossPointSettings
  // fields live, so the two never disagree about the current value.
  const int rotationIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_ROTATION_MODE));
  values.push_back(I18N.get(SETTINGS.mapRotationMode == CrossPointSettings::MAP_ROTATION_NORTH_UP
                                ? StrId::STR_MAP_ROTATION_NORTH_UP
                                : StrId::STR_MAP_ROTATION_HEADING_UP));
  const int headingIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_HEADING_MODE));
  values.push_back(I18N.get(SETTINGS.mapHeadingMode == CrossPointSettings::MAP_HEADING_MANUAL
                                ? StrId::STR_MAP_HEADING_FROZEN
                                : StrId::STR_AUTO));
  const int zoomModeIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_ZOOM_MODE));
  values.push_back(
      I18N.get(SETTINGS.mapZoomMode == CrossPointSettings::MAP_ZOOM_AUTO ? StrId::STR_AUTO : StrId::STR_MANUAL));
  // `Useful places` sits low, just above Refresh: it is a screen a rider opens
  // when they need water or a hut, which is rarely, and the rows above it are
  // the ones touched every ride. Muscle memory belongs to the frequent rows.
  // The value column carries how many categories are drawn on the map right
  // now, because that is the only part of this feature still switched on after
  // the popup closes.
  const int nearbyIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_NEARBY));
  if (!pointsEnabled()) {
    // The row stays, dimmed, and says what it is: switched off in Settings. It
    // is not removed, because a row that vanished reads as a firmware that lost
    // a feature (../../docs/point-layer-lifecycle.md, decision 2). Dimming and
    // the skip come from setDisabledRows() below.
    values.push_back(I18N.get(StrId::STR_STATE_OFF));
  } else if (nearbyCategoryMask_ != 0) {
    int shown = 0;
    for (uint8_t c = 0; c < kSafetyCategoryCount; ++c) {
      if ((nearbyCategoryMask_ & (1u << c)) != 0) ++shown;
    }
    char layers[8];
    snprintf(layers, sizeof(layers), "%d", shown);
    values.emplace_back(layers);
  } else {
    values.emplace_back();
  }
  const int reloadIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_REFRESH));
  values.emplace_back();
  const int debugInfoIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_MAP_DEBUG_INFO));
  values.push_back(I18N.get(SETTINGS.mapDebugInfo ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF));
  optionPopup_.showWithValues(
      StrId::STR_MAP, options, values, 0,
      [this, observeIdx, zoomInIdx, zoomOutIdx, wholeRouteIdx, pinsIdx, nearbyIdx, modeIdx, rotationIdx, headingIdx,
       zoomModeIdx, reloadIdx, debugInfoIdx](int idx) {
        // Rows that redraw the map do not need the backdrop; rows
        // that change nothing on it (zoom mode) put it back
        // instead of re-rendering, and so does a plain dismiss
        // (loop()). Freed first for every redraw row, so the
        // buffer is not held across a tile read.
        // The pins list opens over the same map, so it keeps the backdrop --
        // openPinsMenu() gives it up itself if its own dialog outgrows the rect.
        if (idx != zoomModeIdx && idx != pinsIdx && idx != nearbyIdx) dropMenuBackdrop();
        if (idx == observeIdx) {
          toggleObserveMode();
        } else if (idx == pinsIdx) {
          // Recorded, not opened: this callback is running *inside* the popup's
          // own handleInput(), and show()ing from here reassigns the
          // std::function currently executing. loop() opens it next iteration.
          pendingPinPopup_ = PinPopup::List;
        } else if (idx == nearbyIdx) {
          // Same deferral, same reason. The radius search runs when the popup
          // opens, not here: it reads up to nine files off the card and this is
          // still inside the previous popup's input handler.
          pendingNearbyPopup_ = NearbyPopup::Menu;
        } else if (idx == zoomInIdx || idx == zoomOutIdx) {
          // The ladder step itself, then render now rather than on
          // stepZoom()'s settle timer: the settle exists to collapse a burst
          // of button presses, and a menu row cannot be pressed in a burst --
          // the popup is gone. Same reasoning as switchMode()'s own redraw.
          // The pixels the popup left behind need that frame anyway.
          stepZoom(idx == zoomInIdx ? -1 : +1);
          redrawDueMs_ = 0;
          showBusy();
          renderCurrent();
        } else if (idx == wholeRouteIdx) {
          // Back to the whole route, at any point in a ride. Costs one full refresh
          // and one pass over the route file, the same as the frame the picker drew.
          redrawDueMs_ = 0;
          showBusy();
          renderRouteOverview();
        } else if (idx == modeIdx) {
          // One Select steps ride->hike->cycle->ride and closes, same as every
          // other row -- picking a mode is a deliberate, one-shot choice, not the
          // start of a cycling gesture. A rider who wants to step again presses
          // CONFIRM again. mapRideModeName()'s array order.
          const uint8_t next = (static_cast<uint8_t>(mode_) + 1) % kMapRideModeCount;
          switchMode(static_cast<MapRideMode>(next));
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
        } else if (idx == reloadIdx) {
          redrawDueMs_ = 0;
          showBusy();  // Reload is the slowest thing on this screen; acknowledge it
          renderCurrent();
        } else if (idx == debugInfoIdx) {
          SETTINGS.mapDebugInfo = SETTINGS.mapDebugInfo ? 0 : 1;
          SETTINGS.saveToFile();
          redrawDueMs_ = 0;
          showBusy();
          renderCurrent();
        }
      });
  // The `Nearby` row when the whole point layer is switched off: dimmed and
  // unselectable, so it cannot open a screen that has nothing to search. Set
  // after show(), which clears the mask (OptionPopup::setDisabledRows()).
  if (!pointsEnabled()) {
    std::vector<uint8_t> disabled(options.size(), 0);
    disabled[static_cast<size_t>(nearbyIdx)] = 1;
    optionPopup_.setDisabledRows(std::move(disabled));
  }
  // After show() (the layout the rect comes from needs the rows) and before
  // the first draw (the framebuffer still holds the map).
  //
  // The size is recorded here too: every pins list opens at exactly this size, so
  // it lands as the next step of this menu and not as a differently shaped box --
  // and so this same backdrop still covers it.
  menuDialogWidth_ = optionPopup_.dialogWidth(renderer);
  menuVisibleRows_ = optionPopup_.visibleRows(renderer);
  captureMenuBackdrop();
  optionPopup_.processRender(renderer, mappedInput);
}

// ---------------------------------------------------------------------- pins
//
// The Pins UI is popups over this screen (../../../docs/pins-plan.md, phase 3).
// Nothing here draws a pin on the map -- that is phase 4, in the marker pass.

size_t MapActivity::pinSlotForRow(size_t row) const {
  size_t seen = 0;
  for (size_t slot = 0; slot < PinStore::kSlotCount; ++slot) {
    if (!pins_.store().at(slot).present) continue;
    if (seen == row) return slot;
    ++seen;
  }
  return PinStore::kSlotCount;
}

bool MapActivity::pointsEnabled() const {
  // One read of one setting, and every point path goes through it: the shard
  // source's allocation, the render's mark walk, and the map menu's `Nearby`
  // row (../../docs/point-layer-lifecycle.md, decision 2). Read live rather
  // than cached in onEnter(): the Settings screen can flip it while this
  // activity is alive, and a cached copy would keep drawing marks the rider
  // just switched off.
  return SETTINGS.mapPointsEnabled != 0;
}

int32_t MapActivity::riderLatE7() const {
  // Observe mode repoints lastLatE7_ at whatever the rider panned to, so it is
  // the wrong answer to "where am I". The route overview does not: it holds the
  // fix in lastLatE7_ (applyFix()).
  return screenMode_ == MapScreenMode::Observe ? observeReturnLatE7_ : lastLatE7_;
}

int32_t MapActivity::riderLonE7() const {
  return screenMode_ == MapScreenMode::Observe ? observeReturnLonE7_ : lastLonE7_;
}

uint8_t MapActivity::riderHeading() const {
  return screenMode_ == MapScreenMode::Observe ? observeReturnHeading_ : lastHeading_;
}

const char* MapActivity::pinSaveRefusal() const {
  // Never write 0,0. With no fix at all there is nothing to save *at*, and a pin
  // off the coast of Ghana is worse than no pin.
  if (!hasReceivedAny_) return tr(STR_PIN_NO_FIX);
  return nullptr;
}

bool MapActivity::pinFixAgeWarning(char* buf, size_t bufLen) const {
  if (buf == nullptr || bufLen == 0) return false;
  buf[0] = '\0';
  // The fix restored off the card in onEnter() is where the rider was when they
  // last had a phone, which can be days ago -- and it has no age in minutes to
  // quote, only a session boundary.
  if (showingPersistedFix_ || lastFixMs_ == 0) {
    snprintf(buf, bufLen, "%s", tr(STR_PIN_FIX_LAST_SESSION));
    return true;
  }
  const uint32_t ageMs = millis() - lastFixMs_;
  if (ageMs < kPinStaleFixMs) return false;
  snprintf(buf, bufLen, tr(STR_PIN_FIX_AGE), static_cast<int>(ageMs / 60000u));
  return true;
}

void MapActivity::pinDistanceText(const PinEntry& entry, char* buf, size_t bufLen) const {
  if (buf == nullptr || bufLen == 0) return;
  if (!hasReceivedAny_) {
    // Nothing to measure from. "-" and never "0 m": zero is a real distance and
    // would read as "you are standing on it".
    snprintf(buf, bufLen, "-");
    return;
  }
  const uint32_t metres = PinGeo::distanceM(riderLatE7(), riderLonE7(), entry.latE7, entry.lonE7);
  PinGeo::formatDistance(metres, buf, bufLen);
}

void MapActivity::servicePendingPinPopup() {
  const PinPopup which = pendingPinPopup_;
  const size_t arg = pendingPinArg_;
  pendingPinPopup_ = PinPopup::None;
  pendingPinArg_ = 0;
  // Exhaustive switch, no default, so a new value cannot land here silently
  // (control-flow-clarity).
  switch (which) {
    case PinPopup::None:
      return;
    case PinPopup::List:
      openPinsMenu();
      return;
    case PinPopup::AddList:
      openPinsAddList();
      return;
    case PinPopup::Offscreen:
      openPinsOffscreenList();
      return;
    case PinPopup::ConfirmSet:
      confirmPinReplaceSlot(arg);
      return;
    case PinPopup::ConfirmDelete:
      confirmPinDelete(arg);
      return;
    case PinPopup::Show:
      showPinOnMap(arg);
      return;
    case PinPopup::Save:
      if (arg < kPinSlotCount) savePin(kPinCatalog[arg].key, pinTypeLabel(arg));
      return;
  }
}

bool MapActivity::pinEdgeMarkerEnabled(const PinEntry& entry) const {
  // A key this build does not know has no bit of its own and follows the master --
  // wrong-but-visible beats a pin that silently has no marker (PinCatalog.h, the
  // unknown-key rule).
  if (entry.catalogIndex >= kPinSlotCount) return true;
  return (SETTINGS.mapPinsOffscreenMask & (1u << entry.catalogIndex)) != 0;
}

size_t MapActivity::pinEdgeMarkerCount() const {
  size_t on = 0;
  for (size_t slot = 0; slot < PinStore::kSlotCount; ++slot) {
    const PinEntry& entry = pins_.store().at(slot);
    if (entry.present && pinEdgeMarkerEnabled(entry)) ++on;
  }
  return on;
}

void MapActivity::openPinsOffscreenList() {
  // One row per saved pin plus `All` for the master switch. Only saved pins: a bit
  // for a slot with nothing in it is a setting for a pin that does not exist.
  std::vector<std::string> options;
  std::vector<std::string> values;
  const size_t count = pins_.pinCount();
  options.reserve(count + 1);
  values.reserve(count + 1);

  const bool master = SETTINGS.mapPinsOffscreen != 0;
  options.push_back(tr(STR_PIN_OFFSCREEN_ALL));
  values.emplace_back(I18N.get(master ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF));

  for (size_t row = 0; row < count; ++row) {
    const PinEntry& entry = pins_.store().at(pinSlotForRow(row));
    options.emplace_back(pinEntryLabel(entry));
    if (!master) {
      // Not "Off": the bit is whatever it is, the master is simply overriding it.
      // A dash says that without needing a greyed-out row, which this popup cannot
      // draw.
      values.emplace_back("-");
    } else {
      values.emplace_back(I18N.get(pinEdgeMarkerEnabled(entry) ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF));
    }
  }

  optionPopup_.showWithValues(StrId::STR_PIN_OFFSCREEN, options, values, pinsOffscreenRow_, [this](int idx) {
    if (idx == 0) {
      SETTINGS.mapPinsOffscreen = SETTINGS.mapPinsOffscreen ? 0 : 1;
    } else {
      const size_t slot = pinSlotForRow(static_cast<size_t>(idx - 1));
      const PinEntry& entry = pins_.store().at(slot);
      if (entry.catalogIndex < kPinSlotCount) {
        SETTINGS.mapPinsOffscreenMask ^= static_cast<uint16_t>(1u << entry.catalogIndex);
      }
    }
    SETTINGS.saveToFile();
    // The map underneath is now wrong -- a marker appeared or went away -- so the
    // backdrop is worth nothing and the close has to render a real frame. Dropping
    // it is how that happens: every dismiss path falls back to renderCurrent() when
    // there is nothing saved.
    dropMenuBackdrop();
    // Reopened rather than left closed, so a rider can flip several in one visit.
    // Through the pending mechanism, never a show() from inside this callback
    // (PinPopup).
    pinsOffscreenRow_ = static_cast<uint8_t>(idx);
    pendingPinPopup_ = PinPopup::Offscreen;
  });
  optionPopup_.setSizeHint(menuDialogWidth_, menuVisibleRows_);
  dropBackdropIfPopupOutgrew();
  optionPopup_.processRender(renderer, mappedInput);
}

// ## Nearby: the POI browser over the point layer -----------------------------
//
// ../../docs/nearby-menu.md has the three screens and the decisions behind
// them; ../../../docs/safety-concept.md, "Nearby", is the design this
// implements.

void MapActivity::servicePendingNearbyPopup() {
  const NearbyPopup which = pendingNearbyPopup_;
  const uint8_t arg = pendingNearbyArg_;
  pendingNearbyPopup_ = NearbyPopup::None;
  pendingNearbyArg_ = 0;
  // Exhaustive switch, no default, so a new value cannot land here silently.
  switch (which) {
    case NearbyPopup::None:
      return;
    case NearbyPopup::Menu:
      openNearbyMenu();
      return;
    case NearbyPopup::Category:
      openNearbyCategoryList(arg);
      return;
    case NearbyPopup::Detail:
      openNearbyPointDetail(arg);
      return;
  }
}

StrId MapActivity::nearbyCategoryLabel(uint8_t category) {
  // Indexed by MapSafetyCategory, which is also the menu's fixed row order --
  // that is why point_spec.py orders the enum for this screen and not for the
  // classifier (../../../docs/point-file-spec.md).
  switch (static_cast<MapSafetyCategory>(category)) {
    case MapSafetyCategory::Water:
      return StrId::STR_NEARBY_WATER;
    case MapSafetyCategory::Shelter:
      return StrId::STR_NEARBY_SHELTER;
    case MapSafetyCategory::Hut:
      return StrId::STR_NEARBY_HUT;
    case MapSafetyCategory::Lodging:
      return StrId::STR_NEARBY_LODGING;
    case MapSafetyCategory::Fuel:
      return StrId::STR_NEARBY_FUEL;
    case MapSafetyCategory::Hospital:
      return StrId::STR_NEARBY_HOSPITAL;
    case MapSafetyCategory::Pharmacy:
      return StrId::STR_NEARBY_PHARMACY;
    case MapSafetyCategory::Rescue:
      return StrId::STR_NEARBY_RESCUE;
    case MapSafetyCategory::EmergencyPhone:
      return StrId::STR_NEARBY_EMERGENCY_PHONE;
    case MapSafetyCategory::Transport:
      return StrId::STR_NEARBY_TRANSPORT;
    case MapSafetyCategory::Unknown:
      break;
  }
  return StrId::STR_MAP_PLACES;
}

StrId MapActivity::nearbyConditionLabel(uint8_t category, uint8_t flags) {
  // Water says what the condition means for water. A rider reads "Not verified"
  // under a spring and has to work out what was not verified; the whole point of
  // the flag is that nobody checked whether it is drinkable
  // (../../../docs/safety-concept.md, "Honesty rules").
  if (category == static_cast<uint8_t>(MapSafetyCategory::Water) && (flags & kPointUnverified) != 0) {
    return StrId::STR_NEARBY_WATER_UNVERIFIED;
  }
  // One line, not four. Order is worst-first: a restriction stops the rider
  // getting in at all, a season stops them getting in today, a fee is an
  // inconvenience, and "nobody checked" is the weakest claim of the four.
  if ((flags & kPointRestricted) != 0) return StrId::STR_NEARBY_COND_RESTRICTED;
  if ((flags & kPointSeasonal) != 0) return StrId::STR_NEARBY_COND_SEASONAL;
  if ((flags & kPointFee) != 0) return StrId::STR_NEARBY_COND_FEE;
  if ((flags & kPointUnverified) != 0) return StrId::STR_NEARBY_COND_UNVERIFIED;
  if ((flags & kPointUnstaffed) != 0) return StrId::STR_NEARBY_COND_UNSTAFFED;
  if ((flags & kPointOpenSided) != 0) return StrId::STR_NEARBY_COND_OPEN_SIDED;
  return StrId::STR_NEARBY_UNNAMED;  // caller checks flags first; never printed
}

void MapActivity::nearbyRowValue(const MapPointQuery::Hit& hit, char* buf, size_t bufLen) const {
  char distance[16];
  PinGeo::formatDistance(hit.metres, distance, sizeof(distance));
  // The flag column, the list's half of the map's corner triangle: one mark for
  // "there is a condition attached" and nothing finer, because at this size
  // nothing finer is readable and the exact condition is on the detail screen.
  const bool flagged = (hit.flags & kPointFlaggedOnMapMask) != 0;
  snprintf(buf, bufLen, "%s%s %s", flagged ? "? " : "", distance, MapPointQuery::sectorName(hit.sector));
}

bool MapActivity::runNearbyQuery() {
  // The query runs from the fix, never from the viewport: a hospital 14 km away
  // must be listed while the map shows 3x5 km. With no fix there is nothing to
  // search from, and a list measured from 0,0 would be worse than no list.
  if (!hasReceivedAny_) return false;

  if (!nearbyFile_) {
    nearbyFile_ = makeUniqueNoThrow<HalFileSource>();
    if (!nearbyFile_) {
      LOG_ERR(kLogTag, "OOM: HalFileSource for the Nearby query");
      return false;
    }
  }
  if (!nearbyQuery_) {
    nearbyQuery_ = makeUniqueNoThrow<MapPointQuery>(*nearbyFile_);
    if (!nearbyQuery_) {
      LOG_ERR(kLogTag, "OOM: MapPointQuery (%u bytes)", static_cast<unsigned>(sizeof(MapPointQuery)));
      return false;
    }
  }

  MapPointQuery::Config config;
  config.rootDir = kTileRoot;
  config.fixLatE7 = riderLatE7();
  config.fixLonE7 = riderLonE7();
  nearbyQuery_->begin(config);

  const bool ok = nearbyQuery_->nearestPerCategory(nearbyDistances_, kSafetyCategoryCount);
  LOG_INF(kLogTag, "nearby: %lu shard(s) read, %lu missing, %lu corrupt, %lu bytes",
          static_cast<unsigned long>(nearbyQuery_->shardsOpened()),
          static_cast<unsigned long>(nearbyQuery_->shardsMissing()),
          static_cast<unsigned long>(nearbyQuery_->shardsCorrupt()),
          static_cast<unsigned long>(nearbyQuery_->bytesRead()));
  return ok;
}

bool MapActivity::loadNearbyCategory(uint8_t category) {
  nearbyHitCount_ = 0;
  nearbyCategory_ = category;
  if (!nearbyQuery_) return false;
  nearbyHitCount_ = static_cast<uint8_t>(nearbyQuery_->listCategory(category, nearbyHits_, MapPointQuery::kMaxHits));
  return true;
}

void MapActivity::openNearbyMenu() {
  // Unreachable through the menu when the layer is off -- that row is dimmed
  // and unselectable. Guarded anyway: a screen that searched with the layer off
  // would allocate the query and read nine shards for a list nobody asked for.
  if (!pointsEnabled()) return;
  if (!runNearbyQuery()) {
    // A notice, not ten rows of "None": with no fix the distances are not
    // unknown, they are unanswerable. The map has to come back first, because
    // the menu's pixels are still on the panel (savePin's order).
    dropMenuBackdrop();
    redrawDueMs_ = 0;
    showBusy();
    renderCurrent();
    showPinNotice(tr(STR_NEARBY_NO_FIX));
    return;
  }

  std::vector<std::string> options;
  std::vector<std::string> values;
  options.reserve(kSafetyCategoryCount);
  values.reserve(kSafetyCategoryCount);

  char value[24];
  char none[24];
  snprintf(none, sizeof(none), tr(STR_NEARBY_NONE_IN_RANGE), static_cast<int>(MapPointShards::kSearchRadiusM / 1000.0));

  // Every category, in the enum's order, always. A category never disappears:
  // an absent row reads as zero distance or as a bug, and both are worse than
  // the truth (../../../docs/safety-concept.md, "Nearby").
  for (uint8_t category = 1; category < kSafetyCategoryCount; ++category) {
    options.emplace_back(I18N.get(nearbyCategoryLabel(category)));
    if (nearbyDistances_[category] == MapPointQuery::kNoDistance) {
      values.emplace_back(none);
    } else {
      char distance[16];
      PinGeo::formatDistance(nearbyDistances_[category], distance, sizeof(distance));
      // A checkmark while that category's marks are on the map, the same shape
      // the off-screen pin list uses for a bit that is set.
      const bool shown = (nearbyCategoryMask_ & (1u << category)) != 0;
      snprintf(value, sizeof(value), "%s%s", shown ? "* " : "", distance);
      values.emplace_back(value);
    }
  }

  const int hideAllIdx = nearbyCategoryMask_ != 0 ? static_cast<int>(options.size()) : -1;
  if (hideAllIdx >= 0) {
    // One row that clears every layer, so a rider who turned three on does not
    // have to walk three rows to turn them off. No layer manager beyond this.
    options.push_back(tr(STR_NEARBY_HIDE_ALL));
    values.emplace_back();
  }

  // `Places`, not the menu row's own `Useful places`: a popup title is centred
  // in a dialog narrower than the row it was opened from, and the rider just
  // pressed the row, so the long form has nothing left to disambiguate.
  optionPopup_.showWithValues(StrId::STR_MAP_PLACES, options, values, 0, [this, hideAllIdx](int idx) {
    if (idx == hideAllIdx) {
      nearbyCategoryMask_ = 0;
      // The map underneath lost its marks, so the backdrop is worthless and the
      // close has to draw a real frame.
      dropMenuBackdrop();
      redrawDueMs_ = 0;
      showBusy();
      renderCurrent();
      return;
    }
    // Rows run 1..kSafetyCategoryCount-1, so the row index is the category id
    // minus one -- there is no `unknown` row.
    pendingNearbyArg_ = static_cast<uint8_t>(idx + 1);
    nearbyRow_ = 0;
    pendingNearbyPopup_ = NearbyPopup::Category;
  });
  optionPopup_.setSizeHint(menuDialogWidth_, menuVisibleRows_);
  dropBackdropIfPopupOutgrew();
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::openNearbyCategoryList(uint8_t category) {
  if (!loadNearbyCategory(category)) return;

  std::vector<std::string> options;
  std::vector<std::string> values;
  options.reserve(nearbyHitCount_ + 1u);
  values.reserve(nearbyHitCount_ + 1u);

  // `Show on map` first: it is what this screen is for when the rider is
  // deciding rather than picking.
  const bool shown = (nearbyCategoryMask_ & (1u << category)) != 0;
  options.push_back(tr(STR_NEARBY_SHOW_ON_MAP));
  values.emplace_back(I18N.get(shown ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF));

  char value[24];
  for (uint8_t i = 0; i < nearbyHitCount_; ++i) {
    const MapPointQuery::Hit& hit = nearbyHits_[i];
    options.emplace_back(hit.name[0] != '\0' ? hit.name : tr(STR_NEARBY_UNNAMED));
    nearbyRowValue(hit, value, sizeof(value));
    values.emplace_back(value);
  }

  optionPopup_.showWithValues(I18N.get(nearbyCategoryLabel(category)), options, values, nearbyRow_,
                              [this, category](int idx) {
                                if (idx == 0) {
                                  toggleNearbyCategoryOnMap(category);
                                  return;
                                }
                                nearbyRow_ = static_cast<uint8_t>(idx);
                                pendingNearbyArg_ = static_cast<uint8_t>(idx - 1);
                                pendingNearbyPopup_ = NearbyPopup::Detail;
                              });
  optionPopup_.setSizeHint(menuDialogWidth_, menuVisibleRows_);
  dropBackdropIfPopupOutgrew();
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::openNearbyPointDetail(uint8_t hitIndex) {
  if (hitIndex >= nearbyHitCount_) return;
  const MapPointQuery::Hit& hit = nearbyHits_[hitIndex];

  // Two rows, and both do something. The first cut of this screen carried the
  // distance and the condition as rows of their own, which put a menu cursor on
  // text that could not be pressed -- "it reads as a sloppy hack" (maintainer,
  // on hardware 2026-08-21). So the distance rides in the value column of the
  // row it belongs to (the one that takes you there) and the condition is a
  // note under the title (OptionPopup::setNote).
  std::vector<std::string> options;
  std::vector<std::string> values;
  char distance[24];
  nearbyRowValue(hit, distance, sizeof(distance));

  const int viewIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_NEARBY_VIEW_ON_MAP));
  values.emplace_back(distance);
  const int destIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_NEARBY_SET_DESTINATION));
  values.emplace_back();

  optionPopup_.showWithValues(hit.name[0] != '\0' ? hit.name : tr(STR_NEARBY_UNNAMED), options, values, viewIdx,
                              [this, hitIndex, viewIdx, destIdx](int idx) {
                                if (idx == viewIdx) {
                                  viewNearbyPointOnMap(hitIndex);
                                  return;
                                }
                                if (idx == destIdx) setNearbyDestination(hitIndex);
                              });
  // Only when the point actually carries one. A note that says nothing would
  // make every POI look conditional.
  const bool hasCondition = (hit.flags & (kPointFlaggedOnMapMask | kPointUnstaffed | kPointOpenSided)) != 0;
  if (hasCondition) optionPopup_.setNote(I18N.get(nearbyConditionLabel(hit.category, hit.flags)));
  optionPopup_.setSizeHint(menuDialogWidth_, menuVisibleRows_);
  dropBackdropIfPopupOutgrew();
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::viewNearbyPointOnMap(uint8_t hitIndex) {
  if (hitIndex >= nearbyHitCount_) return;
  const MapPointQuery::Hit& hit = nearbyHits_[hitIndex];

  dropMenuBackdrop();

  // **Turn the category's layer on.** Without this the rider is taken to a
  // place with nothing on it: the marks are drawn only for categories the mask
  // carries, and `View on map` used to move the frame without setting the bit --
  // "kde je ten zdroj vody? ani prd" (maintainer, on hardware 2026-08-21).
  if (hit.category < 16) nearbyCategoryMask_ |= static_cast<uint16_t>(1u << hit.category);

  // And remember which one was asked for, so it can be told apart from its
  // neighbours: in a village a category is a field of squares, and the frame
  // being centred on one of them is not enough to say which.
  nearbyViewedLatE7_ = hit.latE7;
  nearbyViewedLonE7_ = hit.lonE7;
  nearbyViewedCategory_ = hit.category;
  nearbyViewedFlags_ = hit.flags;
  nearbyViewedValid_ = true;

  // Observation mode wholesale, exactly as a pin's `Show` does: it already
  // stops the next fix yanking the frame back, and the menu's `Follow mode` row
  // already knows how to return to the rider.
  if (screenMode_ == MapScreenMode::Follow) {
    screenMode_ = MapScreenMode::Observe;
    // The return anchor is the rider, captured before the frame moves --
    // lastLatE7_ is about to be repointed at the POI.
    observeReturnLatE7_ = lastLatE7_;
    observeReturnLonE7_ = lastLonE7_;
    observeReturnHeading_ = lastHeading_;
    observeReturnSeq_ = lastDrawnSeq_;
  }
  redrawDueMs_ = 0;
  showBusy();
  // anchorHeading_, the heading the frame on the panel was drawn with: looking
  // at a POI must not also rotate the map.
  renderViewport(hit.latE7, hit.lonE7, anchorHeading_, lastDrawnSeq_);
}

void MapActivity::toggleNearbyCategoryOnMap(uint8_t category) {
  nearbyCategoryMask_ ^= static_cast<uint16_t>(1u << category);
  LOG_INF(kLogTag, "nearby layer mask 0x%04x", static_cast<unsigned>(nearbyCategoryMask_));
  // The map underneath changed, so the saved pixels are wrong: drop the backdrop
  // and draw a real frame, exactly as the off-screen pin list does when it flips
  // a marker on.
  dropMenuBackdrop();
  redrawDueMs_ = 0;
  showBusy();
  renderCurrent();
}

void MapActivity::setNearbyDestination(uint8_t hitIndex) {
  if (hitIndex >= nearbyHitCount_) return;
  const MapPointQuery::Hit& hit = nearbyHits_[hitIndex];

  // Whether this replaces a destination or creates one, read before the write --
  // afterwards there is always one there.
  const bool replaced = hasDestination();

  // An ordinary v1 pin record under the `dest` key. No new catalogue row, no
  // provenance field: a pin record has nowhere to put "this was a spring", and
  // adding one means a v2 line that an older build skips whole, losing the pin
  // (../../../docs/safety-concept.md, "Set destination").
  const bool wrote = pins_.pinSet("dest", hit.latE7, hit.lonE7, MapPins::utcNowOrZero());

  // The header row now means something different, which is one of the two
  // exceptions to the 30 s repaint floor: setting and clearing a destination
  // change what the row *says* rather than its value.
  if (wrote) {
    nextDestRepaintMs_ = 0;
    drawnDestPresent_ = false;
  }

  // The frame first, the notice on top of it -- the popup's pixels are still on
  // the panel, so a notice drawn before the map would sit on the popup. Same
  // order savePin() uses. The frame is not optional either: the square becomes
  // the destination pin's balloon, which is the mark vocabulary saying what
  // happened without a word (../../../docs/map-render-spec.md).
  dropMenuBackdrop();
  redrawDueMs_ = 0;
  showBusy();
  renderCurrent();
  // tr() is a macro that pastes its argument after `StrId::`, so the choice has
  // to be made on the id and not inside the macro.
  const StrId notice =
      !wrote ? StrId::STR_PIN_WRITE_FAILED : (replaced ? StrId::STR_NEARBY_DEST_REPLACED : StrId::STR_NEARBY_DEST_SET);
  showPinNotice(I18N.get(notice));
}

// --- the destination readout in the header ----------------------------------

bool MapActivity::hasDestination() const {
  const PinEntry* entry = pins_.store().find("dest");
  return entry != nullptr && entry->present;
}

uint8_t MapActivity::destSector() const {
  const PinEntry* entry = pins_.store().find("dest");
  if (entry == nullptr || !entry->present) return 0;
  return MapPointQuery::sector8(riderLatE7(), riderLonE7(), entry->latE7, entry->lonE7);
}

uint32_t MapActivity::destQuantisedDistance() const {
  const PinEntry* entry = pins_.store().find("dest");
  if (entry == nullptr || !entry->present) return 0;
  const uint32_t metres = PinGeo::distanceM(riderLatE7(), riderLonE7(), entry->latE7, entry->lonE7);
  // The table from ../../docs/nearby-menu.md: 100 m steps under 1 km, 0.1 km to
  // 10 km, 1 km above. Returned in the printed unit's own steps, so a value that
  // has not changed cannot cause a repaint -- which is the whole reason this is
  // quantised harder than the Pins list's 10 m.
  if (metres < 10000) return metres / 100;  // 100 m steps, printed as m then as 0.1 km
  // Rounded, not truncated, and it has to match destHeaderText() exactly: a
  // quantised value that moves differently from the printed one either repaints
  // for nothing or holds a row that already reads wrong.
  return (metres + 500) / 1000;
}

bool MapActivity::destHeaderText(char* buf, size_t bufLen) const {
  if (buf == nullptr || bufLen == 0) return false;
  buf[0] = '\0';
  if (!hasDestination() || !hasReceivedAny_) return false;

  const PinEntry* entry = pins_.store().find("dest");
  const uint32_t metres = PinGeo::distanceM(riderLatE7(), riderLonE7(), entry->latE7, entry->lonE7);
  const char* sector = MapPointQuery::sectorName(destSector());

  // Never degrees, and never a metre. The device says the target is roughly
  // that way, roughly that far: a number that ticks every second on e-ink is
  // both a lie about the precision and a waveform pass per tick.
  if (metres < 1000) {
    snprintf(buf, bufLen, "%s %lu00 m", sector, static_cast<unsigned long>(metres / 100));
  } else if (metres < 10000) {
    snprintf(buf, bufLen, "%s %lu.%lu km", sector, static_cast<unsigned long>(metres / 1000),
             static_cast<unsigned long>((metres % 1000) / 100));
  } else {
    snprintf(buf, bufLen, "%s %lu km", sector, static_cast<unsigned long>((metres + 500) / 1000));
  }
  return true;
}

void MapActivity::openPinsMenu() {
  std::vector<std::string> options;
  std::vector<std::string> values;
  const size_t count = pins_.pinCount();
  options.reserve(count + 1);
  values.reserve(count + 1);

  // Add / Replace first, and it is the only row when nothing is saved yet: an
  // empty slot is not listed here (it lives in that list), so a rider with no
  // pins sees the one row that does something rather than ten that say "empty".
  options.push_back(tr(STR_PIN_ADD));
  values.emplace_back();

  // How many pins would put a marker on the edge, out of how many are saved. The
  // row is here rather than in the map menu because it belongs to the pins, and the
  // map menu is already eleven rows long.
  const int offscreenIdx = static_cast<int>(options.size());
  options.push_back(tr(STR_PIN_OFFSCREEN));
  {
    char ratio[16];
    snprintf(ratio, sizeof(ratio), "%u/%u", static_cast<unsigned>(SETTINGS.mapPinsOffscreen ? pinEdgeMarkerCount() : 0),
             static_cast<unsigned>(count));
    values.emplace_back(ratio);
  }

  char distance[16];
  for (size_t row = 0; row < count; ++row) {
    const PinEntry& entry = pins_.store().at(pinSlotForRow(row));
    options.emplace_back(pinEntryLabel(entry));
    pinDistanceText(entry, distance, sizeof(distance));
    values.emplace_back(distance);
  }

  optionPopup_.showWithValues(StrId::STR_MAP_PINS, options, values, 0, [this, offscreenIdx](int idx) {
    if (idx == 0) {
      pendingPinPopup_ = PinPopup::AddList;
      return;
    }
    if (idx == offscreenIdx) {
      pinsOffscreenRow_ = 0;
      pendingPinPopup_ = PinPopup::Offscreen;
      return;
    }
    // Show is not a popup, but it renders a frame -- and rendering from inside
    // handleInput() would draw under the popup that is still on the panel. Same
    // deferral, same reason.
    pendingPinArg_ = static_cast<uint8_t>(pinSlotForRow(pinRowToListIndex(idx)));
    pendingPinPopup_ = PinPopup::Show;
  });
  // Row actions, so Delete and Replace are one press each instead of a submenu
  // per pin. This takes the front pair away from scrolling, which is why the side
  // hints go up with it (OptionPopup::setRowActions).
  OptionPopup::RowActions actions;
  actions.leftLabel = tr(STR_DELETE);
  actions.rightLabel = tr(STR_REPLACE);
  actions.confirmLabel = tr(STR_SHOW);
  actions.onLeft = [this](int idx) {
    // The rows above the pins (Add / Replace, Off-screen markers) have no pin under
    // them to delete, and a press that silently does nothing is better than one that
    // deletes the first pin.
    if (idx < kPinListFirstPinRow) return;
    pendingPinArg_ = static_cast<uint8_t>(pinSlotForRow(pinRowToListIndex(idx)));
    pendingPinPopup_ = PinPopup::ConfirmDelete;
  };
  actions.onRight = [this](int idx) {
    if (idx < kPinListFirstPinRow) return;
    pendingPinArg_ = static_cast<uint8_t>(pinSlotForRow(pinRowToListIndex(idx)));
    pendingPinPopup_ = PinPopup::ConfirmSet;
  };
  optionPopup_.setRowActions(std::move(actions));
  // Rotated by the theme: a right-pointing glyph lands pointing up, a left one
  // lands pointing down -- the same rotation drawPanSideHints() had to account
  // for, verified there on hardware.
  optionPopup_.setSideHints("→", "←", UI_10_FONT_ID);
  popupDrewSideHints_ = true;
  // Same size as the menu it came out of: a differently sized box in the middle
  // of the previous one reads as a different kind of dialog rather than the next
  // step of the same one, and matching it keeps the menu backdrop valid.
  optionPopup_.setSizeHint(menuDialogWidth_, menuVisibleRows_);
  dropBackdropIfPopupOutgrew();
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::openPinsAddList() {
  // Every catalogue slot, so an empty one can be filled and an occupied one
  // replaced from the same place. Eleven rows scroll inside the popup's
  // six-row window, which costs one refresh per selection step and no RAM
  // (BaseTheme::optionPopupGeometry()).
  std::vector<std::string> options;
  std::vector<std::string> values;
  options.reserve(kPinSlotCount);
  values.reserve(kPinSlotCount);

  char distance[16];
  for (size_t i = 0; i < kPinSlotCount; ++i) {
    options.emplace_back(pinTypeLabel(i));
    const PinEntry& entry = pins_.store().at(i);
    if (entry.present) {
      pinDistanceText(entry, distance, sizeof(distance));
      values.emplace_back(distance);
    } else {
      values.emplace_back();
    }
  }

  optionPopup_.showWithValues(StrId::STR_PIN_ADD, options, values, 0, [this](int idx) {
    const size_t catalogIndex = static_cast<size_t>(idx);
    if (catalogIndex >= kPinSlotCount) return;
    char age[48];
    const bool occupied = pins_.store().at(catalogIndex).present;
    // An empty slot saves straight away -- there is nothing to lose, so a
    // confirmation would only be in the way. Unless the fix is old: then the
    // save is about to record where the rider *was*, and that is worth a
    // question even on an empty slot.
    if (!occupied && !pinFixAgeWarning(age, sizeof(age))) {
      pendingPinArg_ = static_cast<uint8_t>(catalogIndex);
      pendingPinPopup_ = PinPopup::Save;
      return;
    }
    pendingPinArg_ = static_cast<uint8_t>(catalogIndex);
    pendingPinPopup_ = PinPopup::ConfirmSet;
  });
  optionPopup_.setSizeHint(menuDialogWidth_, menuVisibleRows_);
  dropBackdropIfPopupOutgrew();
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::confirmPinReplaceSlot(size_t slot) {
  if (slot >= PinStore::kSlotCount) return;
  const PinEntry& entry = pins_.store().at(slot);
  // A foreign key has no catalogue row, so the label is the raw key and the key
  // itself has to come off the entry -- which is also what keeps a pin written by
  // a later firmware replaceable rather than only deletable.
  //
  // Whether a slot is foreign is a property of `slot` itself (PinStore reserves
  // slots kPinSlotCount.. for foreign keys and never puts one in a catalogue
  // slot), not of `entry.catalogIndex` -- an empty catalogue slot defaults
  // `catalogIndex` to `kPinIndexUnknown` (PinStore.h), which equals
  // kPinSlotCount, so checking the entry made every never-saved catalogue slot
  // look foreign and read `entry.key` (never written, empty) instead of
  // `kPinCatalog[slot].key`. Reported 2026-08-24: saving to an empty slot
  // ("Base") failed with "no slot for ''" -- PinStore::makeSetRecord() refusing
  // the empty key that reached it this way.
  const bool foreign = slot >= kPinSlotCount;
  const char* key = foreign ? entry.key : kPinCatalog[slot].key;
  const char* label = foreign ? entry.key : pinTypeLabel(slot);

  char title[96];
  snprintf(title, sizeof(title), tr(STR_PIN_REPLACE_CONFIRM), label);

  // Cancel first, so the destructive option is never the one already under the
  // cursor. Same two-option shape as the reader's bookmark delete
  // (EpubReaderBookmarksActivity.cpp).
  const char* options[] = {tr(STR_CANCEL), tr(STR_REPLACE)};
  char keyCopy[kPinKeyBytes];
  snprintf(keyCopy, sizeof(keyCopy), "%s", key);
  std::string labelCopy(label);
  optionPopup_.show(title, options, 2, 0, [this, keyCopy, labelCopy](int idx) {
    if (idx != 1) {
      // Nothing changed, so the frame under the popup is still right.
      if (!restoreMenuBackdrop()) {
        redrawDueMs_ = 0;
        showBusy();
        renderCurrent();
      }
      return;
    }
    savePin(keyCopy, labelCopy.c_str());
  });
  // Same size as the list this confirmation replaces -- after show(), which
  // resets it to 0 (OptionPopup::resetRowChrome()). Missing here (every other
  // popup opener sets it) is why the confirm box used to compute its own
  // narrower size from just "Cancel"/"Replace" instead of matching the
  // Add/Replace or Pins list behind it. Reported on the S8 2026-08-24.
  optionPopup_.setSizeHint(menuDialogWidth_, menuVisibleRows_);
  dropBackdropIfPopupOutgrew();
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::confirmPinDelete(size_t slot) {
  if (slot >= PinStore::kSlotCount) return;
  const PinEntry& entry = pins_.store().at(slot);
  if (!entry.present) return;

  char title[96];
  snprintf(title, sizeof(title), tr(STR_PIN_DELETE_CONFIRM), pinEntryLabel(entry));
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  const int slotCopy = static_cast<int>(slot);
  optionPopup_.show(title, options, 2, 0, [this, slotCopy](int idx) {
    if (idx != 1) {
      if (!restoreMenuBackdrop()) {
        redrawDueMs_ = 0;
        showBusy();
        renderCurrent();
      }
      return;
    }
    deletePin(static_cast<size_t>(slotCopy));
  });
  // Same missing size hint as confirmPinReplaceSlot() above, after show() for
  // the same reason -- same fix.
  optionPopup_.setSizeHint(menuDialogWidth_, menuVisibleRows_);
  dropBackdropIfPopupOutgrew();
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::savePin(const char* key, const char* label) {
  dropMenuBackdrop();
  const char* refusal = pinSaveRefusal();
  if (refusal != nullptr) {
    // The popup's pixels are still on the panel, so the map has to come back
    // before the notice lands on top of it.
    redrawDueMs_ = 0;
    showBusy();
    renderCurrent();
    showPinNotice(refusal);
    return;
  }

  char notice[64];
  if (!pins_.pinSet(key, riderLatE7(), riderLonE7(), MapPins::utcNowOrZero())) {
    snprintf(notice, sizeof(notice), "%s", tr(STR_PIN_WRITE_FAILED));
  } else {
    snprintf(notice, sizeof(notice), tr(STR_PIN_SAVED), label);
  }
  // A full frame, not a backdrop restore: the pin belongs in the picture, and
  // from phase 4 on it is drawn in the same pass as the marker.
  redrawDueMs_ = 0;
  showBusy();
  renderCurrent();
  showPinNotice(notice);
}

void MapActivity::deletePin(size_t slot) {
  dropMenuBackdrop();
  if (slot >= PinStore::kSlotCount) return;
  // The label is read before the delete: afterwards the slot is empty and a
  // foreign key's label lives in the entry that just went away.
  char label[kPinKeyBytes + 16];
  snprintf(label, sizeof(label), "%s", pinEntryLabel(pins_.store().at(slot)));
  const bool ok = pins_.pinDelete(pins_.store().at(slot).key);

  char notice[64];
  snprintf(notice, sizeof(notice), ok ? tr(STR_PIN_DELETED) : tr(STR_PIN_WRITE_FAILED), label);
  redrawDueMs_ = 0;
  showBusy();
  renderCurrent();
  showPinNotice(notice);
}

namespace {

// Liang-Barsky, cut down to what an edge marker needs: where the segment from
// `ox,oy` to `px,py` leaves the rectangle. False when the segment never enters it
// at all, which happens with the rider off screen (Observe mode panned away) and
// the pin on the far side.
bool clipExitPoint(double ox, double oy, double px, double py, double left, double top, double right, double bottom,
                   double& outX, double& outY) {
  double t0 = 0.0;
  double t1 = 1.0;
  const double dx = px - ox;
  const double dy = py - oy;
  const double p[4] = {-dx, dx, -dy, dy};
  const double q[4] = {ox - left, right - ox, oy - top, bottom - oy};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0.0) {
      if (q[i] < 0.0) return false;  // parallel and outside this edge
      continue;
    }
    const double t = q[i] / p[i];
    if (p[i] < 0.0) {
      if (t > t1) return false;
      if (t > t0) t0 = t;
    } else {
      if (t < t0) return false;
      if (t < t1) t1 = t;
    }
  }
  outX = ox + dx * t1;
  outY = oy + dy * t1;
  return true;
}

}  // namespace

Rect MapActivity::pinEdgeArea() const {
  // Where an edge marker may land. Everything this screen draws over the map has a
  // fixed place, and a marker that lands under one of them is invisible -- measured
  // on the panel 2026-08-17, where one came out beneath the zoom hints with only
  // its "11" readable and another sat behind the button bar.
  //
  // The geometry comes from whoever owns each piece: the theme for both hint bands
  // (BaseTheme::buttonHintsRect(), sideButtonHintsRect()), this file's own compass
  // constants for the compass. Copying their numbers here is what drift is made of.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  int top = kPinEdgeMargin;
  int bottom = screenH - kPinEdgeMargin;
  int right = screenW - kPinEdgeMargin;
  const int left = kPinEdgeMargin;

  const Rect bottomHints = GUI.buttonHintsRect(renderer);
  if (bottomHints.height > 0) bottom = bottomHints.y - kPinEdgeMargin / 2;

  const Rect sideHints = GUI.sideButtonHintsRect(renderer);
  // X4 stacks them on the right edge; X3 puts one on each side, which is a band
  // across the full width and cannot be avoided by narrowing -- not special-cased
  // yet, and there is no X3 here to try it on.
  if (sideHints.width > 0 && sideHints.width < screenW && sideHints.x > screenW / 2) {
    right = sideHints.x - kPinEdgeMargin / 2;
  }

  // The compass sits top-right and the header row above it; clear both by taking
  // the compass glyph's own bottom edge.
  const int compassBottom = kCompassCenterTop + kCompassGlyphRadius;
  if (compassBottom + kPinEdgeMargin > top) top = compassBottom + kPinEdgeMargin / 2;

  if (right <= left || bottom <= top) return Rect{left, top, 0, 0};
  return Rect{left, top, right - left, bottom - top};
}

bool MapActivity::windowRefreshAffordable(int w, int h) const {
  if (w <= 0 || h <= 0) return false;
  // The driver's own arithmetic: one bit per pixel, rows padded to whole bytes
  // (Ssd1677Driver::displayWindow). Rounded up rather than down -- the point is
  // to refuse early, not to be exact.
  const size_t bytes = (static_cast<size_t>(w) / 8 + 1) * static_cast<size_t>(h);
  const size_t largest = ESP.getMaxAllocHeap();
  const bool ok = bytes + kWindowHeapMargin <= largest;
  if (!ok) {
    LOG_DBG(kLogTag, "window %dx%d wants %u bytes, largest block %u -- full refresh instead", w, h,
            static_cast<unsigned>(bytes), static_cast<unsigned>(largest));
  }
  return ok;
}

void MapActivity::drawViewedNearbyPoint() {
  // A line per refusal, deliberately. This ring did not appear on hardware
  // 2026-08-22, three readings of this function could not say which guard took
  // it, and the laptop preview cannot show it at all -- the preview runs
  // MapRenderer through IMapCanvas and this draws straight onto GfxRenderer. So
  // the device has to say it.
  if (!nearbyViewedValid_) {
    LOG_DBG(kLogTag, "ring skipped: no viewed point set");
    return;
  }
  // Only while looking around. Back in Follow the rider is following themselves
  // again and a ring around something they looked at once is stale decoration.
  if (screenMode_ != MapScreenMode::Observe) {
    LOG_DBG(kLogTag, "ring skipped: not in observe mode");
    return;
  }
  if (mapStyleFor(mode_, zoomStep()).pointSquarePx <= 0) {
    LOG_DBG(kLogTag, "ring skipped: the style draws no square");
    return;
  }

  double mercX = 0.0;
  double mercY = 0.0;
  MapProjection::lonLatToMerc(static_cast<double>(nearbyViewedLatE7_) / 1e7,
                              static_cast<double>(nearbyViewedLonE7_) / 1e7, mercX, mercY);
  int32_t sx = 0;
  int32_t sy = 0;
  proj_.projectMercWide(mercX, mercY, sx, sy);

  // A circle, not a bigger square, and with real white between it and the mark.
  //
  // The first cut was a rounded rect 3 px outside a 15 px square: on the panel
  // that reads as a square with a fatter border, not as a highlight. Measured
  // the hard way 2026-08-22 -- the log said `ring drawn at 230,600` while both
  // the maintainer and this session looked at the panel and saw no ring. A
  // different *shape* is what carries at this size, so the highlight is a circle
  // around the square with a 5 px gap, and the gap is knocked out in white so
  // the map underneath cannot fill it in.
  const int side = mapStyleFor(mode_, zoomStep()).pointSquarePx;
  const int gap = 5;                  // white between the square and the circle
  const int radius = side / 2 + gap;  // circle radius around the mark's centre
  const int box = radius * 2;
  if (sx - radius - 2 < 0 || sy - radius - 2 < mapContentTop() || sx + radius + 2 >= renderer.getScreenWidth() ||
      sy + radius + 2 >= renderer.getScreenHeight()) {
    LOG_DBG(kLogTag, "ring skipped: point at %d,%d off the panel (%dx%d, content top %d)", (int)sx, (int)sy,
            renderer.getScreenWidth(), renderer.getScreenHeight(), mapContentTop());
    return;
  }

  const int x = static_cast<int>(sx) - radius;
  const int y = static_cast<int>(sy) - radius;
  // White ring outside the black one, the same halo trick the marker and the
  // compass use: this lands on live map lines, and a bare black circle over a
  // road casing is not a circle. Radius = half the box, which is what makes
  // drawRoundedRect draw a circle (MapRenderer does the same for a place dot).
  renderer.drawRoundedRect(x - 2, y - 2, box + 4, box + 4, 2, (box + 4) / 2, false);
  renderer.drawRoundedRect(x, y, box, box, 2, box / 2, true);
  LOG_DBG(kLogTag, "ring drawn at %d,%d, box %d", (int)sx, (int)sy, box);
}

void MapActivity::drawPins() {
  if (pins_.pinCount() == 0) return;

  // Straight onto GfxRenderer, not through IMapCanvas: a pin is not map data, it
  // is the rider's own mark, and MapRenderer knows nothing about it. That is also
  // why the webapp's firmware preview panel cannot show pins (parent
  // docs/device-preview.md) -- say so there rather than let someone chase it.
  //
  // Above the route rather than under it, which the plan had the other way round:
  // the route is drawn inside MapRenderer::render() as a second source, and pins
  // deliberately are not in that renderer at all. The glyph is small and its halo
  // is tight, so what it covers of a line is a few pixels at the pin's own place.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  int drawn = 0;

  // Off-screen markers, when the rider turned them on. Collected first and drawn
  // after the loop, so overlapping ones can merge into a single glyph with a
  // count instead of stacking into an unreadable pile.
  const bool wantEdges = SETTINGS.mapPinsOffscreen != 0 && hasReceivedAny_;
  PinEdgeMark edges[kPinEdgeMax];
  size_t edgeCount = 0;
  size_t edgesDropped = 0;
  // Where the bearing is measured from: the rider, projected through the frame on
  // screen. Not the screen centre -- the marker sits low on the panel by design
  // (the marker ladder), so a centre-based ray points at the wrong edge.
  double riderX = 0.0;
  double riderY = 0.0;
  if (wantEdges) {
    double mercX = 0.0;
    double mercY = 0.0;
    MapProjection::lonLatToMerc(static_cast<double>(riderLatE7()) / 1e7, static_cast<double>(riderLonE7()) / 1e7, mercX,
                                mercY);
    int32_t rx = 0;
    int32_t ry = 0;
    proj_.projectMercWide(mercX, mercY, rx, ry);
    riderX = static_cast<double>(rx);
    riderY = static_cast<double>(ry);
    // Direction is measured from the rider only while the rider is on this frame.
    // Panned away in Observe mode they are not, and a ray between two points that
    // are both off the panel frequently crosses none of it -- so every distant pin
    // silently lost its marker (panel, 2026-08-17). Fall back to the middle of what
    // is on screen: "that way" from what the rider is looking at is the only
    // question a marker can answer then. The distance stays rider-to-pin, which is
    // the number they actually want.
    const Rect area = pinEdgeArea();
    const bool riderOnFrame =
        riderX >= area.x && riderX <= area.x + area.width && riderY >= area.y && riderY <= area.y + area.height;
    if (!riderOnFrame) {
      riderX = area.x + area.width / 2.0;
      riderY = area.y + area.height / 2.0;
      LOG_DBG(kLogTag, "pin markers: rider off frame, bearings from the screen centre");
    }
  }

  for (size_t slot = 0; slot < PinStore::kSlotCount; ++slot) {
    const PinEntry& entry = pins_.store().at(slot);
    if (!entry.present) continue;

    double mercX = 0.0;
    double mercY = 0.0;
    MapProjection::lonLatToMerc(static_cast<double>(entry.latE7) / 1e7, static_cast<double>(entry.lonE7) / 1e7, mercX,
                                mercY);
    // Wide projection: a pin can be a hundred kilometres away, which overflows the
    // int16_t path and would draw a glyph in the middle of the map
    // (MapProjection::projectMercWide).
    int32_t sx = 0;
    int32_t sy = 0;
    proj_.projectMercWide(mercX, mercY, sx, sy);

    // The balloon hangs *above* its tip, so the on-screen test is asymmetric: a
    // tip a little below the panel still has a readable body on it.
    const PinShapeFrame& upright = kPinShapeFrames[0];
    if (sx < -upright.w || sy < -upright.h || sx > screenW + upright.w || sy > screenH + upright.h) {
      if (!wantEdges) continue;
      if (!pinEdgeMarkerEnabled(entry)) continue;
      double ex = 0.0;
      double ey = 0.0;
      // Clipped to the area nothing else on this screen owns (pinEdgeArea()), not
      // to the panel: a marker under the hint bar or the compass is a marker the
      // rider never sees.
      const Rect area = pinEdgeArea();
      if (area.width <= 0 || area.height <= 0) continue;
      if (!clipExitPoint(riderX, riderY, static_cast<double>(sx), static_cast<double>(sy), area.x, area.y,
                         area.x + area.width, area.y + area.height, ex, ey)) {
        continue;
      }
      if (edgeCount >= kPinEdgeMax) {
        ++edgesDropped;
        continue;
      }
      PinEdgeMark& mark = edges[edgeCount++];
      mark.x = static_cast<int16_t>(ex);
      mark.y = static_cast<int16_t>(ey);
      // Unit direction times 64, not the raw difference: a pin 100 km away is
      // millions of pixels off and would overflow the int16 the mark keeps.
      const double vx = static_cast<double>(sx) - riderX;
      const double vy = static_cast<double>(sy) - riderY;
      const double len = sqrt(vx * vx + vy * vy);
      mark.dirX = len > 0.0 ? static_cast<int16_t>(lround(vx / len * 64.0)) : 0;
      mark.dirY = len > 0.0 ? static_cast<int16_t>(lround(vy / len * 64.0)) : 0;
      mark.metres = PinGeo::distanceM(riderLatE7(), riderLonE7(), entry.latE7, entry.lonE7);
      mark.catalogIndex = static_cast<uint8_t>(entry.catalogIndex);
      mark.count = 1;
      continue;
    }

    drawPinBalloon(static_cast<int>(sx), static_cast<int>(sy), entry.catalogIndex);
    ++drawn;
  }

  // Crowding: two pins in nearly the same direction leave the screen at nearly
  // the same point, and two arrows on top of each other read as one broken one.
  // Merge them into the nearest one's arrow with a count.
  for (size_t i = 0; i < edgeCount; ++i) {
    if (edges[i].count == 0) continue;
    for (size_t j = i + 1; j < edgeCount; ++j) {
      if (edges[j].count == 0) continue;
      const int dx = edges[i].x - edges[j].x;
      const int dy = edges[i].y - edges[j].y;
      if (dx * dx + dy * dy > kPinEdgeMergePx * kPinEdgeMergePx) continue;
      // The nearer pin's distance survives: "the closest of these is 2.4 km" is
      // the useful half of a merged marker.
      if (edges[j].metres < edges[i].metres) {
        edges[i].metres = edges[j].metres;
        edges[i].dirX = edges[j].dirX;
        edges[i].dirY = edges[j].dirY;
        // The nearest pin's identity travels with its distance: a merged marker
        // says "the closest of these is the parking, 2.4 km that way".
        edges[i].catalogIndex = edges[j].catalogIndex;
      }
      edges[i].count = static_cast<uint8_t>(edges[i].count + edges[j].count);
      edges[j].count = 0;
    }
  }

  for (size_t i = 0; i < edgeCount; ++i) {
    if (edges[i].count == 0) continue;
    drawPinEdgeMark(edges[i]);
    ++drawn;
  }
  // Never a silent cap: a marker that was not drawn is a pin the rider cannot
  // see, which is exactly the failure the feature is about.
  if (edgesDropped > 0)
    LOG_ERR(kLogTag, "%u off-screen pin marker(s) dropped: more than %d", static_cast<unsigned>(edgesDropped),
            kPinEdgeMax);
  if (drawn > 0) LOG_DBG(kLogTag, "%d pin mark(s) drawn", drawn);
}

void MapActivity::drawPinBalloon(int tipX, int tipY, size_t catalogIndex, uint8_t step) {
  // A map pin, not a bare glyph: the shape is a baked asset
  // (src/components/icons/pin-shape.svg through scripts/gen_pin_icons.py) and the
  // tail's point is *at* the coordinate. Both things a 16 px glyph could not do --
  // it is findable on a panel full of building outlines, and it says which pixel it
  // means instead of hovering over it (asked for on hardware 2026-08-17, where bare
  // glyphs were unfindable and vanished under the position marker).
  //
  // `step` turns the shape so its point aims somewhere other than straight down;
  // the glyph is drawn upright regardless, because a numeral that rotates with the
  // pin stops being readable. Step 0 is point-down, which is every pin inside the
  // viewport.
  //
  // Three passes, and the first is not optional: the ink array is an outline, so
  // without the silhouette painted white underneath, road lines show through the
  // head. Same reasoning as the marker's halo, just shaped like the pin.
  const PinShapeFrame& frame = kPinShapeFrames[step % kPinShapeSteps];
  const int x = tipX - frame.tipX;
  const int y = tipY - frame.tipY;
  renderer.drawMono1bpp(frame.mask, x, y, frame.w, frame.h, false);
  renderer.drawMono1bpp(frame.ink, x, y, frame.w, frame.h, true);
  renderer.drawMono1bpp(pinGlyphBits(catalogIndex), x + frame.headX - kPinGlyphPx / 2,
                        y + frame.headY - kPinGlyphPx / 2, kPinGlyphPx, kPinGlyphPx, true);
}

// Which baked rotation points closest to `dx,dy` (screen space, y down).
//
// Step k is the shape turned k * 360/kPinShapeSteps degrees clockwise from
// point-down, so its point aims at (-sin, cos) of that angle -- which inverts to
// atan2(-dx, dy).
uint8_t MapActivity::pinShapeStepFor(double dx, double dy) {
  if (dx == 0.0 && dy == 0.0) return 0;
  const double stepAngle = 2.0 * M_PI / kPinShapeSteps;
  double turns = atan2(-dx, dy) / stepAngle;
  int step = static_cast<int>(lround(turns)) % kPinShapeSteps;
  if (step < 0) step += kPinShapeSteps;
  return static_cast<uint8_t>(step);
}

void MapActivity::drawPinEdgeMark(const PinEdgeMark& mark) {
  // The pin itself, turned so its point aims at where the pin actually is. An arrow
  // plus a distance said "11 km that way" and nothing about *what* was that way,
  // and a separate arrow next to an upright pin said it twice (both judged on the
  // panel 2026-08-17). One object now: the pin is the arrow.
  const double dx = static_cast<double>(mark.dirX) / 64.0;
  const double dy = static_cast<double>(mark.dirY) / 64.0;
  const uint8_t step = pinShapeStepFor(dx, dy);
  const PinShapeFrame& frame = kPinShapeFrames[step % kPinShapeSteps];
  const Rect area = pinEdgeArea();
  const int leftLimit = area.x;
  const int rightLimit = area.x + area.width;
  const int topLimit = area.y;
  const int bottomLimit = area.y + area.height;

  // Anchor on the head, not the point: the head is the shape's visual weight (it
  // carries the glyph), and drawPinBalloon() rotates the body around the tip, not
  // the head -- so a tip-anchored draw makes the head swing to a different screen
  // position every frame, which reads as the glyph "jumping" as the marker turns
  // (reported 2026-08-23). Anchoring the head instead keeps the glyph pinned to one
  // screen spot for all 16 rotations; the point is what swings, landing wherever
  // the turned body puts it. That does give up the point sitting exactly on the
  // boundary -- it now pokes past it by a frame-dependent amount -- which is an
  // accepted trade for the glyph no longer moving.
  int headX = mark.x;
  int headY = mark.y;
  if (headX - frame.headX < leftLimit) headX = leftLimit + frame.headX;
  if (headX - frame.headX + frame.w > rightLimit) headX = rightLimit - frame.w + frame.headX;
  if (headY - frame.headY < topLimit) headY = topLimit + frame.headY;
  if (headY - frame.headY + frame.h > bottomLimit) headY = bottomLimit - frame.h + frame.headY;
  // drawPinBalloon() still takes a tip target -- back-solve it from the head
  // target and this frame's own tip-to-head offset.
  const int tipX = headX + frame.tipX - frame.headX;
  const int tipY = headY + frame.tipY - frame.headY;
  drawPinBalloon(tipX, tipY, mark.catalogIndex, step);

  char distance[16];
  PinGeo::formatDistance(mark.metres, distance, sizeof(distance));
  char text[24];
  if (mark.count > 1) {
    // How many pins are behind this one, and how far the nearest is. Plain
    // literals, no tr(): a multiplication sign is not language dependent, same
    // call as the map's "+"/"--" side hints.
    snprintf(text, sizeof(text), "%s x%u", distance, static_cast<unsigned>(mark.count));
  } else {
    snprintf(text, sizeof(text), "%s", distance);
  }

  // SMALL_FONT_ID (8pt), not UI_10: a distance readout is detail-view chrome, not
  // a primary label, and the same weight as scale-bar/header text overstated it
  // (reported 2026-08-23 alongside the pivot -- too big, too bold for what it is).
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, text);
  const int textHeight = renderer.getLineHeight(SMALL_FONT_ID);
  // Under the head, not under the point: the point is against the edge of the area
  // and aiming outward, so there is no room that side.
  int textX = headX - textWidth / 2;
  int textY = headY - frame.headY + frame.h + 2;
  if (textY + textHeight > bottomLimit) textY = headY - frame.headY - textHeight - 2;
  // And not under the rider's own marker, which is drawn after this and would eat
  // it: a pin near the bottom edge puts its label exactly where the marker sits
  // (panel, 2026-08-17 -- measured, the label was at 149,685 90x24 against a marker
  // at 198,658 64x64).
  //
  // Sideways, not above or below: near the bottom edge there is no room either way,
  // and there is always most of a screen to one side. Vertical is the last resort.
  {
    int mx = 0, my = 0, mw = 0, mh = 0;
    // Where the marker is *about to* land, not where the last frame left it:
    // drawPins() only ever runs inside a full render, and a full render puts the
    // marker on the ladder anchor (renderViewport()). markerDrawnX_ still describes
    // the previous frame at this point.
    markerRect(MapViewport::kAnchorScreenX, MapViewport::markerYForStep(markerStep()), mx, my, mw, mh);
    const bool overlaps = textX < mx + mw && textX + textWidth > mx && textY < my + mh && textY + textHeight > my;
    if (overlaps) {
      if (mx - leftLimit >= textWidth + 4) {
        textX = mx - textWidth - 4;
      } else if (rightLimit - (mx + mw) >= textWidth + 4) {
        textX = mx + mw + 4;
      } else if (my - textHeight - 4 >= topLimit) {
        textY = my - textHeight - 4;
      } else {
        textY = my + mh + 4;
      }
    }
  }

  // White halo, not an opaque box: same technique place labels use
  // (MapLabels.cpp:315-325, kHaloRing) -- the map keeps showing between the
  // letters instead of disappearing under a filled rectangle, matching how a
  // place name reads at a detailed zoom (reported 2026-08-23).
  static constexpr int kHaloOffsets[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int radius = 1; radius <= 2; ++radius) {
    for (const auto& offset : kHaloOffsets) {
      renderer.drawText(SMALL_FONT_ID, textX + offset[0] * radius, textY + offset[1] * radius, text, false);
    }
  }
  renderer.drawText(SMALL_FONT_ID, textX, textY, text, true);
}

void MapActivity::showPinOnMap(size_t slot) {
  dropMenuBackdrop();
  if (slot >= PinStore::kSlotCount) return;
  const PinEntry entry = pins_.store().at(slot);
  if (!entry.present) return;

  // Observation mode wholesale (docs/map-observation-mode.md), rather than a
  // fourth kind of frame: it already stops the next fix yanking the viewport
  // back, and the menu's "Follow mode" row already knows how to return to the
  // rider. GPS keeps being recorded, the route is untouched.
  if (screenMode_ == MapScreenMode::Follow) {
    screenMode_ = MapScreenMode::Observe;
    // The return anchor is the rider, captured before the frame moves -- the same
    // capture toggleObserveMode() does, and for the same reason: lastLatE7_ is
    // about to be repointed at the pin.
    observeReturnLatE7_ = lastLatE7_;
    observeReturnLonE7_ = lastLonE7_;
    observeReturnHeading_ = lastHeading_;
    observeReturnSeq_ = lastDrawnSeq_;
  }
  // Already in Observe: the anchor is whatever fix was in effect when the rider
  // started looking around, which is still the right place to go back to.

  LOG_INF(kLogTag, "show pin %s at %ld,%ld", entry.key, static_cast<long>(entry.latE7), static_cast<long>(entry.lonE7));
  redrawDueMs_ = 0;
  showBusy();
  // anchorHeading_, the heading the frame on the panel was drawn with -- the same
  // choice panBy() makes, so looking at a pin does not also rotate the map.
  renderViewport(entry.latE7, entry.lonE7, anchorHeading_, lastDrawnSeq_);
}

void MapActivity::pinNoticeRect(int& x, int& y, int& w, int& h) const {
  const int pad = 6;
  h = renderer.getLineHeight(UI_12_FONT_ID) + pad * 2;
  w = renderer.getScreenWidth() - kTextX * 2;
  x = kTextX;
  // Above the busy badge's clearance line, which is itself above
  // GUI.drawButtonHints' band -- so the notice never lands on a hint box.
  y = renderer.getScreenHeight() - kBusyMarginBottom - h - pad;
}

void MapActivity::showPinNotice(const char* text) {
  if (text == nullptr || text[0] == '\0') return;
  clearPinNotice();

  int x = 0, y = 0, w = 0, h = 0;
  pinNoticeRect(x, y, w, h);

  // Save what it covers, so the notice can go away again without re-reading a
  // tile. Small (a few KB at 480 px wide), and freed as soon as it is put back.
  const size_t size = renderer.getRegionByteSize(x, y, w, h);
  if (size > 0) {
    auto buffer = makeUniqueNoThrow<uint8_t[]>(size);
    if (buffer && renderer.copyRegionToBuffer(x, y, w, h, buffer.get(), size)) {
      pinNoticePatch_ = std::move(buffer);
      pinNoticePatchSize_ = size;
      pinNoticePatchRect_ = Rect{x, y, w, h};
    } else if (!buffer) {
      LOG_ERR(kLogTag, "pin notice patch unavailable: %u bytes", static_cast<unsigned>(size));
    }
  }

  // Opaque, like the compass halo and the busy badge: this lands on live map
  // lines, not on margin.
  renderer.fillRect(x, y, w, h, false);
  renderer.drawRect(x, y, w, h, kBusyBorder, true);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, text);
  const int textX = x + (w - textWidth) / 2;
  const int textY = y + (h - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, textX > x ? textX : x + 4, textY, text, true);

  if (!renderer.displayBufferWindow(x, y, w, h)) {
    LOG_ERR(kLogTag, "pin notice window rejected: %d,%d %dx%d", x, y, w, h);
  }
  pinNoticeUntilMs_ = millis() + kPinNoticeMs;
}

void MapActivity::clearPinNotice() {
  pinNoticeUntilMs_ = 0;
  if (!pinNoticePatch_) return;
  const Rect rect = pinNoticePatchRect_;
  const bool written =
      renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, pinNoticePatch_.get(), pinNoticePatchSize_);
  pinNoticePatch_.reset();
  pinNoticePatchSize_ = 0;
  if (!written) {
    LOG_ERR(kLogTag, "pin notice restore rejected: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
    return;
  }
  if (!renderer.displayBufferWindow(rect.x, rect.y, rect.width, rect.height)) {
    LOG_ERR(kLogTag, "pin notice clear window rejected: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
  }
}

void MapActivity::dropBackdropIfPopupOutgrew() {
  if (!menuBackdrop_) return;
  const Rect popup = optionPopup_.frameRect(renderer);
  const Rect saved = menuBackdropRect_;
  const bool covered = popup.x >= saved.x && popup.y >= saved.y && popup.x + popup.width <= saved.x + saved.width &&
                       popup.y + popup.height <= saved.y + saved.height;
  if (covered) return;
  // Restoring a rect smaller than what is on the panel would leave the previous
  // popup's frame around the new one, so the backdrop is worth nothing now. The
  // close then costs a full redraw, which is the behaviour before backdrops
  // existed.
  LOG_DBG(kLogTag, "menu backdrop dropped: popup %dx%d outgrew the saved %dx%d", popup.width, popup.height, saved.width,
          saved.height);
  dropMenuBackdrop();
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
  // Back to following the rider: the ring around whatever POI they went to look
  // at is stale decoration now (drawViewedNearbyPoint()).
  nearbyViewedValid_ = false;
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
  // 30 % of the screen per press, not half of it: at half a screen the frame jumps
  // far enough that the rider has to re-find where they were looking, and every
  // press costs a full tile read and a refresh either way (asked for on hardware
  // 2026-08-17). Enough overlap to follow a road across two presses.
  const int16_t stepX = static_cast<int16_t>(renderer.getScreenWidth() * MapViewport::kPanStepPercent / 100);
  const int16_t stepY = static_cast<int16_t>(renderer.getScreenHeight() * MapViewport::kPanStepPercent / 100);
  int16_t targetX = MapViewport::kAnchorScreenX;
  int16_t targetY = markerY;
  switch (direction) {
    case PanDirection::Left:
      targetX -= stepX;
      break;
    case PanDirection::Right:
      targetX += stepX;
      break;
    case PanDirection::Up:
      targetY -= stepY;
      break;
    case PanDirection::Down:
      targetY += stepY;
      break;
  }

  // Inverse-project through proj_ -- the frame actually on screen, whether it
  // was drawn by the last real fix or the previous pan step -- so each press
  // moves one step from wherever the rider last panned to.
  double mercX = 0.0, mercY = 0.0;
  proj_.screenToMerc(targetX, targetY, mercX, mercY);
  double lat = 0.0, lon = 0.0;
  MapProjection::mercToLonLat(mercX, mercY, lat, lon);

  LOG_DBG(kLogTag, "pan: %d%% step, new anchor %.5f,%.5f", MapViewport::kPanStepPercent, lat, lon);
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
  // A rest position inside the keep-in margin can never settle: decide()
  // (MapFollow.cpp:31) checks the new fix's absolute screen position, and
  // after a ReAnchor the fix sits exactly at markerYForStep(next) -- so a
  // step that already violates the margin forces ReAnchor on every following
  // fix, moving or standing still. That is the "map keeps refreshing" bug
  // reported 2026-08-15: kMarkerLadder's last rung (760, MapViewport.h:181)
  // sits past the margin at every zoom rung (720-741 px, MapMarkerMetrics.h's
  // per-rung ring + MapFollow::kKeepInSlackPx). Checked against the full-size
  // marker's margin -- the largest one, rungs 0-4 -- so the step is refused
  // regardless of which rung is on screen now or chosen later.
  const int16_t worstCaseMarginPx = static_cast<int16_t>(kMarkerMetricsFull.ring + MapFollow::kKeepInSlackPx);
  if (MapViewport::markerYForStep(next) >= renderer.getScreenHeight() - worstCaseMarginPx) return;
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
            static_cast<unsigned long>(modeMasks_.forMode(mode_, zoomStep())));
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
  // **The rider's position, not the frame's anchor.** riderLatE7() and not
  // lastLatE7_, and the difference is a real bug this replaced (found on
  // hardware 2026-08-21, docs/map-observation-mode.md, "The pan target is not
  // the rider"): renderViewport() repoints lastLatE7_ at whatever it draws, so
  // after a pan, a pin's `Show` or Nearby's `View on map` that variable is the
  // place the rider was *looking at*. Persisting it wrote the pan target as the
  // last known fix, and the next entry into the map drew the Follow marker --
  // ring plus heading arrow -- on it, claiming the rider was standing there.
  const int32_t fixLatE7 = riderLatE7();
  const int32_t fixLonE7 = riderLonE7();
  const uint8_t fixHeading = riderHeading();
  // Only a fix this *session* actually produced, never the one onEnter()
  // just bootstrapped off the card -- otherwise every re-entry would write
  // the same fix straight back at itself. showingPersistedFix_ is exactly
  // that distinction (cleared the moment a real fix lands, see loop()).
  const bool fixChanged = hasReceivedAny_ && !showingPersistedFix_ &&
                          (!SETTINGS.mapHasLastFix || SETTINGS.mapLastLatE7 != fixLatE7 ||
                           SETTINGS.mapLastLonE7 != fixLonE7 || SETTINGS.mapLastHeading != fixHeading);
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
    SETTINGS.mapLastLatE7 = fixLatE7;
    SETTINGS.mapLastLonE7 = fixLonE7;
    SETTINGS.mapLastHeading = fixHeading;
  }
  if (!SETTINGS.saveToFile()) {
    LOG_ERR(kLogTag, "failed to persist map ladder state");
    return;
  }
  LOG_DBG(kLogTag, "saved ladder state: mode %s, zoom %u, marker %u", mapRideModeName(mode_),
          static_cast<unsigned>(zoomStep()), static_cast<unsigned>(markerStep()));
}

bool MapActivity::preventAutoSleep() { return freeink::BlePositionServer::getInstance().isRunning(); }

void MapActivity::kickFullClock() { powerManager.setPowerSaving(false); }

bool MapActivity::preventThrottle() {
  // Queued work the rider is waiting on, or bytes moving. Everything else on
  // this screen is a handful of integer compares per tick and runs fine at the
  // BLE-safe clock.
  return redrawDueMs_ != 0 || arrivalRedrawDueMs_ != 0 || transfer_.status().active;
}

void MapActivity::renderWaiting() {
  kickFullClock();
  // Same reason as renderViewport(): whatever asked for this frame asked for the
  // ordinary map, not the overview.
  overviewShown_ = false;
  renderer.clearScreen();
  renderer.drawText(UI_10_FONT_ID, 8, 8, bleStartFailed_ ? tr(STR_MAP_BLE_START_FAILED) : tr(STR_MAP_WAITING_BLE),
                    true);
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_MAP_OPTIONS), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  drawZoomSideHints();
  renderer.displayBuffer(pendingEntryCleanRefresh_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  pendingEntryCleanRefresh_ = false;
  busyShown_ = false;  // this frame painted over the badge
  // No map and no marker on this frame: there is nothing for a fix to move
  // inside, so the next one draws a real viewport (applyFix()).
  viewportDrawn_ = false;
  // No header row either -- this frame is just the waiting text, drawn with
  // renderer.drawText() above, not drawHeaderStatus().
  headerRowDrawn_ = false;
  markerPatchValid_ = false;
}

void MapActivity::renderLoadingTiles() {
  kickFullClock();
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
  kickFullClock();
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

void MapActivity::drawSleepMarker() {
  // Three conditions, and NOT viewportDrawn_ -- which is the wrong flag and cost
  // a hardware pass to learn (2026-08-19, "sleep marker skipped (viewport=0
  // patch=1)" with a perfectly good map and marker on the glass). It means "a
  // live viewport a fix can move a marker inside": renderViewport() sets it to
  // `!showingPersistedFix_`, so a frame drawn from the persisted fix -- what every
  // map session shows with no phone connected -- leaves it false.
  //
  // markerPatchValid_ is the flag that answers the real question. It is set right
  // after markerDrawnX_/Y_ are recorded and the marker is painted
  // (renderViewport(), moveMarker()), so it means exactly "a marker is on the
  // panel at markerDrawnX_/Y_ and this patch erases it". renderWaiting() and
  // renderLoadingTiles(), the two marker-less frames, clear it.
  //
  // Observe is its own case: renderViewport() sets the patch but deliberately
  // draws NO marker there, because the anchor is a pan target the rider chose and
  // a marker glyph on it would claim to be a fix. Drawing a sleep marker on it
  // would make exactly that claim.
  if (!markerPatchValid_ || !markerPatch_ || markerBoxDrawn_ <= 0 || screenMode_ == MapScreenMode::Observe) {
    LOG_DBG(kLogTag, "sleep marker skipped (patch=%d box=%d observe=%d)", (int)markerPatchValid_, (int)markerBoxDrawn_,
            (int)(screenMode_ == MapScreenMode::Observe));
    return;
  }

  int x, y, w, h;
  markerRect(markerDrawnX_, markerDrawnY_, x, y, w, h);
  // Same erase moveMarker() does, and for the same reason.
  renderer.writeFramebufferRegion(x, y, w, h, markerPatch_.get());

  const int cx = markerDrawnX_;
  const int cy = markerDrawnY_;
  const int haloRadius = kSleepMarkerRing / 2 + kSleepMarkerHalo;
  renderer.fillRoundedRect(cx - haloRadius, cy - haloRadius, haloRadius * 2, haloRadius * 2, haloRadius, Color::White);
  const int radius = kSleepMarkerRing / 2;
  renderer.drawRoundedRect(cx - radius, cy - radius, kSleepMarkerRing, kSleepMarkerRing, kSleepMarkerRingWidth, radius,
                           true);
  renderer.fillRoundedRect(cx - kSleepMarkerDot / 2, cy - kSleepMarkerDot / 2, kSleepMarkerDot, kSleepMarkerDot,
                           kSleepMarkerDot / 2, Color::Black);

  // The box refreshed is the *live* marker's, which is larger than the sleep one
  // and concentric with it, so one window covers both the erase and the new
  // shape. Costs 500 ms, the same as any windowed refresh whatever its area
  // (MapMarkerMetrics.h) -- and the sleep screen's moon is a second one, so the
  // way into sleep is two windowed refreshes rather than the whole-panel HALF it
  // used to be.
  if (!renderer.displayBufferWindow(x, y, w, h)) {
    LOG_ERR(kLogTag, "sleep marker window refused at %d,%d %dx%d", x, y, w, h);
  }
  // The patch describes the background under a marker that is no longer there.
  // Nothing in this activity runs again, but a stale-valid patch is not a thing
  // to leave set.
  markerPatchValid_ = false;
}

void MapActivity::drawObserveFixMarker() {
  // observeReturnLatE7_/Lon_ is "where the rider actually is" while Observe
  // is active (see the field comment, MapActivity.h) -- lastLatE7_/Lon_ is
  // the pan target here, which is what renderViewport()'s Observe guard on
  // drawPositionMarker() is protecting against being mistaken for a fix.
  // Projecting the real fix separately and drawing the sleep glyph on it
  // makes no such claim: it says "this is where you were", same as the
  // sleep screen.
  double mercX, mercY;
  MapProjection::lonLatToMerc(static_cast<double>(observeReturnLatE7_) / 1e7,
                              static_cast<double>(observeReturnLonE7_) / 1e7, mercX, mercY);
  int32_t sx, sy;
  proj_.projectMercWide(mercX, mercY, sx, sy);

  // No patch, no erase: unlike drawSleepMarker() this is not replacing a
  // marker already on the panel, it is the first and only thing drawn at
  // this pixel this frame, and the marker goes on last (see the comment
  // above renderViewport()'s own drawPositionMarker() call) so there is
  // nothing under it yet to save.
  const int haloRadius = kSleepMarkerRing / 2 + kSleepMarkerHalo;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  if (sx < -haloRadius || sy < -haloRadius || sx > screenW + haloRadius || sy > screenH + haloRadius) {
    // Panned far enough that the real fix is off this frame. Same as today:
    // no glyph, no off-screen arrow -- that is a separate feature.
    return;
  }

  const int cx = static_cast<int>(sx);
  const int cy = static_cast<int>(sy);
  renderer.fillRoundedRect(cx - haloRadius, cy - haloRadius, haloRadius * 2, haloRadius * 2, haloRadius, Color::White);
  const int radius = kSleepMarkerRing / 2;
  renderer.drawRoundedRect(cx - radius, cy - radius, kSleepMarkerRing, kSleepMarkerRing, kSleepMarkerRingWidth, radius,
                           true);
  renderer.fillRoundedRect(cx - kSleepMarkerDot / 2, cy - kSleepMarkerDot / 2, kSleepMarkerDot, kSleepMarkerDot,
                           kSleepMarkerDot / 2, Color::Black);
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
  // The union of two far-apart boxes can be the whole panel -- the marker jumps
  // corner to corner after a re-anchor, which `Nearby -> View on map` does on
  // purpose. Unbounded, that is a 48,000-byte allocation inside the driver and
  // an abort() on this build (see windowRefreshAffordable()).
  const bool shown =
      windowRefreshAffordable(unionW, unionH) && renderer.displayBufferWindow(unionX, unionY, unionW, unionH);
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
  // Before every early return below: a fix held back because Observe mode or the
  // route overview owns the frame is still a fresh fix, and a pin saved while one
  // of those is up is saved at it (riderLatE7()).
  lastFixMs_ = millis();
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

  // The reason comes out of decide() rather than being worked out here: with two
  // checks able to fire on one fix, only the ladder itself knows which one did
  // (MapFollow::Reason). The line text is formatted there too, so this log and
  // the host console in test/map_window print the same bytes.
  MapFollow::Reason reason = MapFollow::Reason::None;
  const MapFollow::Action action = MapFollow::decide(request, reason);
  char decisionLog[160];
  MapFollow::formatDecisionLog(request, action, reason, seq, decisionLog, sizeof(decisionLog));

  switch (action) {
    case MapFollow::Action::Skip:
      // The panel is not touched. The fix is still the newest one, so a later
      // ladder step re-anchors around it and not around the stale one.
      lastLatE7_ = latE7;
      lastLonE7_ = lonE7;
      lastHeading_ = headingStep;
      LOG_DBG(kLogTag, "%s", decisionLog);
      return;
    case MapFollow::Action::MoveMarker:
      lastLatE7_ = latE7;
      lastLonE7_ = lonE7;
      lastHeading_ = headingStep;
      // moveMarker() logs the same line itself, from the state after the move.
      moveMarker(fixX, fixY, headingStep);
      return;
    case MapFollow::Action::ReAnchor:
      LOG_DBG(kLogTag, "%s", decisionLog);
      renderViewport(latE7, lonE7, headingStep, seq);
      return;
  }
}

void MapActivity::renderRouteOverview() {
  kickFullClock();
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
  GfxRendererCanvas canvas(renderer, mapContentTop(), kScaleMarginBottom, kSideHintReservedPx);

  MapViewState view;
  view.markerX = anchorX;
  view.markerY = anchorY;
  view.heading = static_cast<MapHeading>(fit.heading & 0x0F);
  // Same rule as the follow frame, from the rung the fit chose.
  view.zoomStep = static_cast<uint8_t>(fit.zoomStep);

  const uint32_t missing = drawMapLayers(range, canvas, view, nullptr, {}, &nearestPlaces_);
  // Pins on the overview too: "where is the car relative to this whole route" is
  // exactly the question the overview is for.
  drawPins();
  // North still rotates with the frame -- the overview is drawn at the fit's
  // heading, not north-up, so the compass is the only thing that says which way
  // the picture is turned.
  drawCompass(fit.heading);
  drawHeaderStatus();
  drawHikeElevationLine();  // no-op outside Hike mode
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
  // drawHeaderStatus() above did draw a header row onto this frame, though --
  // the overview has the same link-status question as the follow map, and
  // gating on viewportDrawn_ here would freeze it for as long as the overview
  // is up, the same bug this flag exists to avoid elsewhere.
  headerRowDrawn_ = true;
  markerPatchValid_ = false;
  overviewShown_ = true;

  LOG_INF(kLogTag, "route overview: heading %u, zoom step %u, %lu tiles, %lu missing, %lu ms, %s",
          static_cast<unsigned>(fit.heading), static_cast<unsigned>(fit.zoomStep),
          static_cast<unsigned long>(source_->tilesOpened()), static_cast<unsigned long>(source_->tilesUnavailable()),
          static_cast<unsigned long>(millis() - startMs), fit.fits ? "whole route" : "too long for the ladder");
  (void)missing;

  renderer.displayBuffer(pendingEntryCleanRefresh_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  pendingEntryCleanRefresh_ = false;
  busyShown_ = false;
}

uint32_t MapActivity::drawMapLayers(const MapViewport::TileRange& range, IMapCanvas& canvas, const MapViewState& view,
                                    MapRenderTiming* timing, MapLayerBits knownBadLayers,
                                    MapNearestPlaces* nearestOut) {
  // The style for this frame's travel mode and zoom rung. Every rule in
  // data/mapstyle.json was already resolved at build time, so this is one array
  // lookup (MapStyleTable.h). It has to come before the tile config: the reject
  // margin and the class filter are both read off it.
  const MapStyle& style = mapStyleFor(mode_, view.zoomStep);

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
  // The rung is in the filter too since 2026-08-25: a class the style hides at
  // this rung is not read past its header, instead of being read, projected and
  // then drawn at width 0.
  config.classMask = modeMasks_.forMode(mode_, view.zoomStep);
  // The screen test: geometry whose bbox cannot reach the panel is dropped in
  // the source, before its points are projected (MapTileSource::Config). The
  // margin comes off the compiled style, so a wider road in mapstyle.json
  // widens it with no code change.
  config.screenWidth = static_cast<int16_t>(renderer.getScreenWidth());
  config.screenHeight = static_cast<int16_t>(renderer.getScreenHeight());
  config.rejectMarginPx = mapStyleMaxStrokePx(style);
  // Card time, so a slow frame can be split into "the card was slow" and "the
  // arithmetic was slow" -- the two have entirely different fixes.
  config.nowUs = &cardClockUs;
  // Layers a previous attempt at this same frame found corrupt. Empty on the
  // first attempt (MapTileSource::Config::knownBadLayers).
  config.knownBadLayers = knownBadLayers;
  source_->begin(config);

  // `style` is the compiled data/mapstyle.json resolved for this mode and rung
  // (MapStyleDefaults.h, generated by scripts/gen_mapstyle.py). Nothing
  // overrides it at runtime; the device reads no style file off the card.
  //
  // The route rides along as a second source, re-read from the card on every
  // reset and never held in RAM (IMapRouteSource.h). nullptr when the rider
  // skipped the picker, and then the route pass costs nothing at all.
  //
  // Place names ride along on the same walk. `labels_` is allocated once, in
  // onEnter(), and is null when the style draws no labels -- in which case
  // MapRenderer skips the whole pass rather than allocating anything here
  // (MapLabels.h).
  // The POI marks, and only the categories the rider turned on: `Nearby -> Show
  // on map` is a temporary view, so it filters this walk and never the style
  // (MapPointSource::Config::categoryMask). With no layer on, the source is not
  // even handed over and no shard is opened.
  IMapPointSource* pointSource = nullptr;
  if (points_ && nearbyCategoryMask_ != 0 && pointsEnabled()) {
    MapPointSource::Config pointConfig;
    pointConfig.rootDir = kTileRoot;
    // The point layer is its own grid, so it gets its own range off the same
    // projection -- z10 shards, not the base LOD's tiles.
    const MapViewport::TileRange shards =
        MapViewport::tileRangeFor(proj_, MapPointShards::kShardZoom, static_cast<int>(renderer.getScreenWidth()),
                                  static_cast<int>(renderer.getScreenHeight()));
    pointConfig.range.col0 = shards.col0;
    pointConfig.range.row0 = shards.row0;
    pointConfig.range.col1 = shards.col1;
    pointConfig.range.row1 = shards.row1;
    pointConfig.categoryMask = nearbyCategoryMask_;
    pointConfig.screenWidth = static_cast<int16_t>(renderer.getScreenWidth());
    pointConfig.screenHeight = static_cast<int16_t>(renderer.getScreenHeight());
    // A mark is drawn centred on its point, so the margin is the mark's own
    // reach and not the widest stroke in the style.
    pointConfig.rejectMarginPx = MapPointMarks::reachPx(style);
    points_->begin(pointConfig);
    pointSource = points_.get();
  }

  MapRenderer::render(canvas, *source_, view, style, route_.get(), timing, nearestOut, labels_.get(), pointSource);

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
  // The one seam every heavy frame goes through. renderCurrent() is not
  // enough: applyFix()'s follow decision, the observation-mode return and the
  // console's goto all call this directly, and the first bench run of the
  // 80 MHz floor caught a 10.6 s viewport reset that had bypassed the
  // wrappers' guard entirely.
  kickFullClock();
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
  GfxRendererCanvas canvas(renderer, mapContentTop(), kScaleMarginBottom, kSideHintReservedPx);

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
  view.zoomStep = static_cast<uint8_t>(zoomStep());

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
    // Four words since the bit set widened to 256 bits for 15 layer slots
    // (MapLayerBits.h). Printed high word first, so the string reads as one
    // number.
    LOG_ERR(kLogTag, "%lu corrupt layer(s) drawn (mask 0x%016llx%016llx%016llx%016llx) -- redrawing without them",
            static_cast<unsigned long>(source_->corruptLayers()),
            static_cast<unsigned long long>(bad.w[3]), static_cast<unsigned long long>(bad.w[2]),
            static_cast<unsigned long long>(bad.w[1]), static_cast<unsigned long long>(bad.w[0]));
    renderer.clearScreen();
    missing = drawMapLayers(range, canvas, view, &timing, bad, &nearestPlaces_);
  }

  // The rider's own marks, over the map and under everything below: the marker,
  // the compass, the readout and the hints all still land on top of them.
  drawPins();
  // And the POI the rider asked to look at, if any -- over the marks the
  // renderer drew, because the whole point of it is to be findable among them.
  drawViewedNearbyPoint();

  // Outside IMapCanvas: screen furniture, not map data, so it lands on top
  // regardless of what the hatch above covered. Rotated to this frame's
  // heading, which is the only heading it is ever correct for.
  // The frame's heading, not the fix's: the compass says which way the picture
  // is turned, and with a route holding the frame that is the route's direction.
  drawCompass(frameHeading);
  drawHeaderStatus();
  drawHikeElevationLine();  // no-op outside Hike mode
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
          static_cast<unsigned long>(timing.labelsMs), static_cast<unsigned long>(source_->pointsProjected()),
          static_cast<unsigned long>(source_->waysOffScreen()), static_cast<unsigned long>(source_->ioUs() / 1000u),
          static_cast<unsigned long>(source_->crc32Skipped()), static_cast<unsigned long>(source_->cellsSkipped()),
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
  // Kept for recheckHatchedTiles() to re-scan against the clock between
  // resets -- see the field comment (MapActivity.h).
  lastTileRange_ = rangeSnapshot;

  // The same walk, one field further: each tile's content identity, which the
  // header parse already put in RAM. This is the only moment on the device where
  // it is free, so the freshness check reads it off the render rather than
  // opening every tile again (docs/tile-freshness.md).
  // Recorded, not replaced. The store accumulates across resets and drains as
  // the phone answers, so a rider who pans across a city can have all of it
  // checked rather than only the last screenful (HeldTilesStore).
  for (uint32_t index = 0; index < range.count(); ++index) {
    // A tile that did not open has no content to compare, and saying it is
    // held at content 0 would have the phone report it stale forever. It is
    // already on the missing path, which is where it belongs.
    if ((missing & (1u << index)) != 0) continue;
    g_heldTiles.record(range.z, range.colAt(index), range.rowAt(index), source_->contentIdAt(index));
  }
  consoleState_.setRenderStats(source_->tilesOpened(), source_->tilesUnavailable(), source_->waysEmitted(),
                               source_->bytesRead(), source_->waysFiltered());
  consoleState_.setZoomInfo(zoomStep(), range.z, MapViewport::kZoomLadder[zoomStep()].mpp);
  sendViewportDiagonalIfChanged();

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
  } else {
    // The anchor still gets no marker (see above), but the rider's real last
    // fix does, in the sleep style -- same shape, same "this is where you
    // were, not where you're looking" meaning. Was previously not drawn at
    // all here, which made Observe look like the fix had been lost.
    drawObserveFixMarker();
  }

  // The persisted-fix frame carries a banner only a full redraw can clear, so it
  // is deliberately not followable (applyFix()).
  viewportDrawn_ = !showingPersistedFix_;
  // drawHeaderStatus() above ran unconditionally, persisted fix or not -- the
  // header row is on the panel either way, so its own repaint gate does not
  // follow showingPersistedFix_ the way viewportDrawn_ does.
  headerRowDrawn_ = true;

  // Timed above, deliberately: the gate is how long the framebuffer takes to
  // be ready, not how long the panel takes to show it.
  renderer.displayBuffer(pendingEntryCleanRefresh_ ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  pendingEntryCleanRefresh_ = false;
  busyShown_ = false;  // this frame painted over the badge
}
