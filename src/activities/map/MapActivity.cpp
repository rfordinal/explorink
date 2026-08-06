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
#include "MapFollow.h"
#include "MapHatch.h"
#include "MapRenderer.h"
#include "MapRouteFit.h"
#include "MapStyleDefaults.h"
#include "MapViewport.h"
#include "MissingTilesConsoleSource.h"
#include "MissingTilesStore.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "MAP";

// The clock MapRenderer's optional per-layer timing calls. A plain function
// pointer, because MapRenderer is compiled for the host too and cannot see
// millis() (MapRenderer.h, MapRenderTiming).
uint32_t renderClockMs() { return millis(); }

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

// Stateless view onto MISSING_TILES for the `missing` command, shared with the
// tile sync screen (MissingTilesConsoleSource.h).
MissingTilesConsoleSource g_missingTilesConsoleSource;

// Debug readout geometry.
constexpr int kTextX = 8;
constexpr int kTextLine1Y = 8;
constexpr int kTextLine2Y = 26;
constexpr int kTextLine3Y = 44;
// Only drawn when a BLE map transfer has actually happened -- an idle screen
// keeps the lines it had before that channel existed. Its own line rather than
// sharing line 3, which the last-known-fix notice already owns.
constexpr int kTextLine4Y = 62;

// Busy badge geometry: an hourglass just above the button hints, right-aligned.
// Out of the way of the debug readout (top-left) and the compass (top-right),
// and small enough that its windowed refresh is cheap to look at.
constexpr int kBusySize = 34;
constexpr int kBusyMarginRight = 10;
constexpr int kBusyMarginBottom = 50;  // clears GUI.drawButtonHints' band
constexpr int kBusyBorder = 2;
constexpr int kBusyGlassInset = 8;  // hourglass inset inside the badge box

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
constexpr int kCompassCenterMarginRight = 56;  // arc centre, in from the right edge
constexpr int kCompassCenterTop = 56;          // arc centre, down from the top edge
// Design-space distance from the arc centre to the furthest thing drawn: the
// label sits 33 above it and is about 16 tall, so 46 clears it with slack. The
// accent triangle's tip (28) and the arcs (24) are well inside.
constexpr int kCompassGlyphRadius = 46;
constexpr int kCompassLabelCenterX = 50, kCompassLabelCenterY = 14;
constexpr int kCompassArcCx = 50, kCompassArcCy = 47;
constexpr int kCompassArcRadius = 24;
constexpr float kCompassLeftArcStartDeg = 130.0f, kCompassLeftArcEndDeg = 230.0f;
constexpr float kCompassRightArcStartDeg = 310.0f, kCompassRightArcEndDeg = 360.0f + 50.0f;
constexpr int kCompassArcSegments = 12;  // straight segments approximating each curve
constexpr int kCompassArcLineWidth = 2;
constexpr int kCompassTriTopX = 50, kCompassTriTopY = 28;
constexpr int kCompassTriLeftX = 42, kCompassTriLeftY = 66;
constexpr int kCompassTriRightX = 58, kCompassTriRightY = 66;
constexpr int kCompassAccentX1 = 71, kCompassAccentY1 = 44;
constexpr int kCompassAccentX2 = 71, kCompassAccentY2 = 52;
constexpr int kCompassAccentX3 = 78, kCompassAccentY3 = 48;
constexpr int kCompassHaloMargin = 5;  // white backing, past the glyph's own sweep

// Position marker: one family, three modes, "the higher the speed, the more
// directional" -- hike is a plain dot (position over direction), cycle is a
// small arrow (both matter), ride is a large arrow that fills the ring
// (direction over position). All three share the same outline ring.
constexpr int kMarkerRingDiameter = 54;
constexpr int kMarkerRingWidth = 3;
constexpr int kMarkerHikeDotDiameter = 18;
constexpr int kMarkerCycleTipLen = 16;    // center to tip, pixels
constexpr int kMarkerCycleBaseHalfW = 9;  // center to each base corner, pixels
constexpr int kMarkerRideTipLen = 25;
constexpr int kMarkerRideBaseHalfW = 18;
constexpr int kMarkerHaloMargin = 5;  // white backing, past the ring's own radius

// The marker's halo box: the unit of every partial operation. Everything the
// marker can draw is inside it (the halo is the outermost thing
// drawPositionMarker() paints), so saving this box before the marker goes down
// and writing it back afterwards erases the marker exactly.
constexpr int kMarkerBoxSize = kMarkerRingDiameter + 2 * kMarkerHaloMargin;  // 64
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
  const int radius = kMarkerRingDiameter / 2;
  // White halo first: the ring is only a 2px stroke, so without this the
  // map lines it sits over would show straight through its interior, and a
  // busy junction under the ring reads as clutter, not a marker.
  const int haloRadius = radius + kMarkerHaloMargin;
  renderer.fillRoundedRect(cx - haloRadius, cy - haloRadius, haloRadius * 2, haloRadius * 2, haloRadius, Color::White);
  renderer.drawRoundedRect(cx - radius, cy - radius, kMarkerRingDiameter, kMarkerRingDiameter, kMarkerRingWidth, radius,
                           true);

  if (mode == MapRideMode::Hike) {
    // Position over direction: a plain dot, no heading arrow at all.
    renderer.fillRoundedRect(cx - kMarkerHikeDotDiameter / 2, cy - kMarkerHikeDotDiameter / 2, kMarkerHikeDotDiameter,
                             kMarkerHikeDotDiameter, kMarkerHikeDotDiameter / 2, Color::Black);
    return;
  }

  // Cycle and ride both point at the real incoming heading, never the
  // forced-north display heading renderViewport() uses for map rotation
  // (kNoRouteDisplayHeading) -- direction of travel matters at riding speed
  // even though the map underneath stays north-up.
  const int tipLen = mode == MapRideMode::Ride ? kMarkerRideTipLen : kMarkerCycleTipLen;
  const int baseHalfW = mode == MapRideMode::Ride ? kMarkerRideBaseHalfW : kMarkerCycleBaseHalfW;
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

  // 1. "N", centered on the label's own point -- UI_12, not a NotoSans/Serif
  // size: those are compiled out under OMIT_FONTS (platformio.ini's
  // slim-build flag) and silently draw nothing (confirmed on hardware --
  // GfxRenderer logs "Font not found" and skips). UI_10/UI_12/SMALL are the
  // only sizes guaranteed present in every build.
  //
  // The letter's *position* rotates with the glyph; the letter itself stays
  // upright. Rotated text is not available at arbitrary angles (GfxRenderer
  // only has drawTextRotated90CW), and an upright letter on a turning bezel is
  // how a real compass rose reads anyway.
  const char* label = "N";
  const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int labelHeight = renderer.getTextHeight(UI_12_FONT_ID);
  int labelX = 0, labelY = 0;
  compassPoint(centreX, centreY, kCompassLabelCenterX, kCompassLabelCenterY, cosTheta, sinTheta, labelX, labelY);
  renderer.drawText(UI_12_FONT_ID, labelX - labelWidth / 2, labelY - labelHeight / 2, label, true);

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

void MapActivity::drawDebugLine(int y, char* text) {
  // GfxRenderer::drawText does not clip, and GfxRenderer::drawPixel answers
  // every off-panel pixel with a LOG_ERR -- one overlong readout line is
  // several hundred error lines over USB CDC. Trim to what fits instead.
  const int maxWidth = renderer.getScreenWidth() - kTextX * 2;
  for (size_t len = strlen(text); len > 0 && renderer.getTextWidth(UI_10_FONT_ID, text) > maxWidth; --len) {
    text[len - 1] = '\0';
  }
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
  // Constant for the build, so once here rather than per reset. `info` reports
  // it; the tile sync screen quotes the same number in NEED_TILES.
  consoleState_.setTileFormatVersion(MapTileReader::kFormatVersion);
  consoleState_.setLinkMtuProvider(
      +[]() -> uint16_t { return freeink::BlePositionServer::getInstance().negotiatedMtu(); });
  consoleState_.setLinkIntervalProvider(
      +[]() -> uint16_t { return freeink::BlePositionServer::getInstance().connIntervalMs(); });
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
  const bool popupWasActive = optionPopup_.isActive();
  if (optionPopup_.handleInput(mappedInput, [this] { optionPopup_.processRender(renderer, mappedInput); })) {
    if (popupWasActive && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      suppressBackRelease_ = true;
      // handleInput() already set active=false and fired the redraw callback
      // above, but that callback is optionPopup_.processRender(), which is a
      // no-op once inactive -- nothing repaints the map underneath, and the
      // panel just keeps showing the popup's last pixels. Redraw for real.
      redrawDueMs_ = 0;
      showBusy();  // the popup's pixels are still up; say the redraw started
      renderCurrent();
    }
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
      // speed and utc are carried and stored, and nothing reads them yet --
      // auto zoom and the on-screen fix time are later phases. Logged so the
      // fields can be seen arriving before anything depends on them.
      LOG_DBG(kLogTag, "ble fix: seq %u, heading %u, speed %u km/h, utc %lu, accuracy %u m",
              static_cast<unsigned>(update.seq), static_cast<unsigned>(update.heading),
              static_cast<unsigned>(update.speedKmh), static_cast<unsigned long>(update.utc),
              static_cast<unsigned>(update.accuracyM));
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
  // Up is toward the closest rung: step 0 is 3 m/px, step 4 is 15. Zooming
  // in is going up the ladder.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) stepZoom(-1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) stepZoom(+1);

  // Right increases look-ahead, which moves the marker *down* the screen --
  // docs/architecture-plan.md. Read the pair as a look-ahead slider, not as
  // a marker position, or the direction reads backwards.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) stepMarker(-1);
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) stepMarker(+1);

  // Opens the map menu: Refresh (the whole reason the 10-minute hike cadence
  // is acceptable is that a rider standing at a junction can still force a
  // fresh picture) and Mode (switch ride/hike/cycle without leaving the map).
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openMapMenu();
  }
}

void MapActivity::openMapMenu(int initialIndex) {
  std::vector<std::string> options;
  options.reserve(3);
  options.push_back(tr(STR_REFRESH));
  options.push_back(std::string(tr(STR_MAP_MODE)) + ": " + I18N.get(kMapModeIds[static_cast<uint8_t>(mode_)]));
  // Only with a route loaded. A row that cannot do anything is worse than no
  // row: it reads as a feature that is broken rather than one that needs a route
  // picked at the door.
  const bool hasRoute = route_ != nullptr;
  if (hasRoute) options.push_back(tr(STR_MAP_WHOLE_ROUTE));
  optionPopup_.show(StrId::STR_MAP, options, initialIndex, [this, hasRoute](int idx) {
    if (idx == 0) {
      redrawDueMs_ = 0;
      showBusy();  // Refresh is the slowest thing on this screen; acknowledge it
      renderCurrent();
    } else if (idx == 1) {
      // Cycle, don't open a second popup -- repeated Select on this row
      // steps ride->hike->cycle->ride. mapRideModeName()'s array order.
      const uint8_t next = (static_cast<uint8_t>(mode_) + 1) % kMapRideModeCount;
      switchMode(static_cast<MapRideMode>(next));
      openMapMenu(1);  // reopen with the label refreshed, Mode still highlighted
    } else if (hasRoute) {
      // Back to the whole route, at any point in a ride. Costs one full refresh
      // and one pass over the route file, the same as the frame the picker drew.
      redrawDueMs_ = 0;
      showBusy();
      renderRouteOverview();
    }
  });
  optionPopup_.processRender(renderer, mappedInput);
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
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  busyShown_ = false;  // this frame painted over the badge
  // No map and no marker on this frame: there is nothing for a fix to move
  // inside, so the next one draws a real viewport (applyFix()).
  viewportDrawn_ = false;
  markerPatchValid_ = false;
}

void MapActivity::renderLoadingTiles() {
  renderer.clearScreen();
  renderer.drawText(UI_10_FONT_ID, 8, 8, tr(STR_MAP_LOADING_TILES), true);
  // The rung, because "reading tiles" alone does not say how long this will
  // take and the rung is what decides it (docs/optimization/01-render-pipeline.md
  // has the per-rung times).
  char line[48];
  snprintf(line, sizeof(line), "z%u  %.0f m/px", MapViewport::kZoomLadder[zoomStep()].z,
           MapViewport::kZoomLadder[zoomStep()].mpp);
  renderer.drawText(UI_10_FONT_ID, 8, kTextLine2Y, line, true);
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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

void MapActivity::markerRect(int cx, int cy, int& x, int& y, int& w, int& h) const {
  w = kMarkerBoxSize;
  h = kMarkerBoxSize;
  x = cx - kMarkerBoxSize / 2;
  y = cy - kMarkerBoxSize / 2;
}

bool MapActivity::saveMarkerPatch(int cx, int cy) {
  if (!markerPatch_) return false;
  int x, y, w, h;
  markerRect(cx, cy, x, y, w, h);
  return renderer.readFramebufferRegion(x, y, w, h, markerPatch_.get(), markerPatchCapacity_) != 0;
}

void MapActivity::moveMarker(int16_t sx, int16_t sy, uint8_t headingStep) {
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
  const int unionX = newX < oldX ? newX : oldX;
  const int unionY = newY < oldY ? newY : oldY;
  const int unionW = (newX > oldX ? newX - oldX : oldX - newX) + kMarkerBoxSize;
  const int unionH = (newY > oldY ? newY - oldY : oldY - newY) + kMarkerBoxSize;
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
          (unsigned)partialMoves_, (unsigned)MapFollow::kMaxPartialMoves);
}

void MapActivity::applyFix(int32_t latE7, int32_t lonE7, uint8_t headingStep, uint8_t seq) {
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
  GfxRendererCanvas canvas(renderer);

  MapViewState view;
  view.markerX = anchorX;
  view.markerY = anchorY;
  view.heading = static_cast<MapHeading>(fit.heading & 0x0F);

  const uint32_t missing = drawMapLayers(range, canvas, view);
  // North still rotates with the frame -- the overview is drawn at the fit's
  // heading, not north-up, so the compass is the only thing that says which way
  // the picture is turned.
  drawCompass(fit.heading);

  char line[80];
  snprintf(line, sizeof(line), "%s", route_->name());
  drawDebugLine(kTextLine1Y, line);
  // Says out loud when the ladder could not hold the whole route, because a
  // frame showing the middle of a route looks exactly like one showing all of a
  // shorter route.
  if (fit.fits) {
    snprintf(line, sizeof(line), "%lu pts  z%u %.0fm/px  h%u", static_cast<unsigned long>(route_->pointCount()),
             static_cast<unsigned>(fit.zoomStep), MapViewport::kZoomLadder[fit.zoomStep].mpp,
             static_cast<unsigned>(fit.heading));
  } else {
    snprintf(line, sizeof(line), "%lu pts  z%u  %s", static_cast<unsigned long>(route_->pointCount()),
             static_cast<unsigned>(fit.zoomStep), tr(STR_MAP_ROUTE_PARTIAL));
  }
  drawDebugLine(kTextLine2Y, line);

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

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
                                    MapRenderTiming* timing) {
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
  source_->begin(config);

  // kDefaultMapStyle is the compiled data/mapstyle.json (MapStyleDefaults.h,
  // generated by scripts/gen_mapstyle.py). Nothing overrides it at runtime;
  // the device reads no style file off the card.
  //
  // The route rides along as a second source, re-read from the card on every
  // reset and never held in RAM (IMapRouteSource.h). nullptr when the rider
  // skipped the picker, and then the route pass costs nothing at all.
  MapRenderer::render(canvas, *source_, view, kDefaultMapStyle, route_.get(), timing);

  // Hatch after the geometry, because which tiles are missing is only known
  // once the source has tried to open them, and asking up front would cost a
  // third read of every tile in the range. A missing tile's own area carries
  // no geometry -- tiles do not overlap -- so the only thing hatch can cover
  // is the marker, which goes back on top afterwards.
  const uint32_t missing = source_->unavailableMask();
  if (missing != 0) {
    for (uint32_t index = 0; index < range.count() && index < 32; ++index) {
      if ((missing & (1u << index)) == 0) continue;
      MapHatch::drawTile(canvas, proj_, range.z, range.colAt(index), range.rowAt(index));
      MISSING_TILES.record(range.z, range.colAt(index), range.rowAt(index));
    }
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
  proj_.reset(lat, lon, MapViewport::kAnchorScreenX, markerY, headingStep, MapViewport::mppMercFor(zoomStep(), lat));

  const MapViewport::TileRange range =
      MapViewport::tileRangeFor(proj_, tileZ, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (range.count() > MapViewport::kMaxTiles) {
    // A count above 3x3 is a bug in the range arithmetic, not a state.
    LOG_ERR(kLogTag, "tile range %u..%u x %u..%u = %u tiles, over the 3x3 worst case", range.col0, range.col1,
            range.row0, range.row1, range.count());
  }

  renderer.clearScreen();
  GfxRendererCanvas canvas(renderer);

  MapViewState view;
  view.markerX = MapViewport::kAnchorScreenX;
  view.markerY = markerY;
  // The same heading proj_ was rotated by. MapRenderer draws direction glyphs
  // in raw screen direction (MapRenderer.cpp's kHeadingDir, not
  // rotation-aware), so anything but agreement here has the two disagreeing
  // about which way is up.
  view.heading = static_cast<MapHeading>(headingStep & 0x0F);

  // Per-layer timing, so a slow reset can be attributed to a layer rather than
  // to the frame (docs/optimization/01-render-pipeline.md, step 1). Costs one
  // millis() call per layer and changes no pixel.
  MapRenderTiming timing;
  timing.nowMs = &renderClockMs;
  const uint32_t missing = drawMapLayers(range, canvas, view, &timing);

  // Outside IMapCanvas: screen furniture, not map data, so it lands on top
  // regardless of what the hatch above covered. Rotated to this frame's
  // heading, which is the only heading it is ever correct for.
  drawCompass(headingStep);

  // Does not count the marker or its patch save, both of which happen after the
  // readout is composed -- this is the tile-and-geometry cost, which is the one
  // worth watching.
  const uint32_t elapsedMs = millis() - startMs;
  const uint32_t heapAfter = ESP.getFreeHeap();

  // Debug readout, kept from the BLE checkpoint: the raw values driving the
  // marker, plus what the viewport reset actually cost.
  char line[80];
  snprintf(line, sizeof(line), "%.5f %.5f h%u #%u", lat, lon, headingStep, seq);
  drawDebugLine(kTextLine1Y, line);
  snprintf(line, sizeof(line), "%s z%u m%u %lut %luw %lums", mapRideModeName(mode_), zoomStep(), markerStep(),
           static_cast<unsigned long>(source_->tilesOpened()), static_cast<unsigned long>(source_->waysEmitted()),
           static_cast<unsigned long>(elapsedMs));
  drawDebugLine(kTextLine2Y, line);
  if (showingPersistedFix_) {
    // Still the fix loaded off the card in onEnter() -- nothing from BLE or
    // the console has landed yet this session. Cleared the moment one does
    // (see loop()'s BLE/console branches).
    snprintf(line, sizeof(line), "%s", tr(STR_MAP_LAST_KNOWN_WAITING_BLE));
    drawDebugLine(kTextLine3Y, line);
  } else if (routePath_[0] != '\0' && !route_) {
    // The rider picked a route and there is none on screen. The picker only
    // checks each file's header, so a route whose point array fails its own crc
    // gets this far -- and an empty map with no explanation reads as a bug in
    // the route feature rather than as a broken file. Shares line 3 with the
    // notice above, which is about a different session state and cannot be up
    // at the same time.
    snprintf(line, sizeof(line), "%s", tr(STR_MAP_ROUTE_REFUSED));
    drawDebugLine(kTextLine3Y, line);
  }

  // Enough to see that a file push happened and whether it landed. No new
  // screen and no refresh of its own: this rides whatever redraw the map was
  // going to do anyway, because an e-ink refresh costs the better part of two
  // seconds and a byte counter is not worth one.
  transfer_.formatStatus(line, sizeof(line));
  if (line[0] != '\0') drawDebugLine(kTextLine4Y, line);

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
          "render %lu ms: landuse %lu, buildings %lu, water %lu, roads %lu, route %lu, places %lu; "
          "%lu points projected",
          static_cast<unsigned long>(timing.landuseMs + timing.buildingsMs + timing.waterMs + timing.roadsMs +
                                     timing.routeMs + timing.placesMs),
          static_cast<unsigned long>(timing.landuseMs), static_cast<unsigned long>(timing.buildingsMs),
          static_cast<unsigned long>(timing.waterMs), static_cast<unsigned long>(timing.roadsMs),
          static_cast<unsigned long>(timing.routeMs), static_cast<unsigned long>(timing.placesMs),
          static_cast<unsigned long>(source_->pointsProjected()));
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
  consoleState_.setRenderStats(source_->tilesOpened(), source_->tilesUnavailable(), source_->waysEmitted(),
                               source_->bytesRead(), source_->waysFiltered());
  consoleState_.setZoomInfo(zoomStep(), range.z, MapViewport::kZoomLadder[zoomStep()].mpp);

  // Composited last, over the map's own bottom-edge pixels rather than into
  // reserved space -- same idea as the debug readout at the top of the
  // screen (drawDebugLine() above): the map fills the whole viewport, there
  // is no margin set aside for chrome, so UI text overlays whatever tiles
  // were there.
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // The marker goes on **last**, and its patch is taken immediately before it.
  // Everything the marker can sit over -- map, hatch, compass, readout, button
  // hints -- is already in the framebuffer, so the patch holds the real
  // background and restoring it later erases the marker with nothing left
  // behind. Draw it any earlier and a marker low on the screen would come back
  // with the hints painted through it.
  anchorHeading_ = headingStep;
  markerDrawnX_ = view.markerX;
  markerDrawnY_ = markerY;
  partialMoves_ = 0;
  markerPatchValid_ = saveMarkerPatch(markerDrawnX_, markerDrawnY_);
  if (!markerPatchValid_) {
    // Follow is off until the next reset gets one; every fix redraws in full.
    // Correct picture, expensive picture.
    LOG_ERR(kLogTag, "marker patch unavailable -- fixes will redraw in full");
  }
  // Relative heading 0: this frame is drawn track-up for this very fix, so the
  // arrow points straight up by construction.
  drawPositionMarker(markerDrawnX_, markerDrawnY_, 0, mode_);

  // The persisted-fix frame carries a banner only a full redraw can clear, so it
  // is deliberately not followable (applyFix()).
  viewportDrawn_ = !showingPersistedFix_;

  // Timed above, deliberately: the gate is how long the framebuffer takes to
  // be ready, not how long the panel takes to show it.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  busyShown_ = false;  // this frame painted over the badge
}
