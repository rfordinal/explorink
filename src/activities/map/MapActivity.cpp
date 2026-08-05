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
#include "MapHatch.h"
#include "MapRenderer.h"
#include "MapStyleDefaults.h"
#include "MapViewport.h"
#include "MissingTilesStore.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "MAP";

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

// Lets the `missing` command read MissingTilesStore without MapCommandConsole
// including it: that header pulls in ArduinoJson and the SD-backed
// PersistableStore, and the console half is deliberately free of both (its
// native tests link neither). This adapter is the only place the two meet.
//
// Stateless, so one file-scope instance rather than a member: no heap, and
// nothing to keep in sync with the store it forwards to.
class MissingTilesConsoleSource final : public IMissingTilesSource {
 public:
  void orderForFetch() override { MISSING_TILES.sortByFetchPriority(); }
  size_t missingTileCount() const override { return MISSING_TILES.hits().size(); }
  MapMissingTile missingTileAt(size_t index) const override {
    const MissingTileHit& hit = MISSING_TILES.hits()[index];
    return MapMissingTile{hit.z, hit.col, hit.row, hit.count};
  }
};
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

// North indicator geometry, fixed top-right corner. Ported 1:1 (scale 1
// design-unit = 1 pixel) from the user's exact vector spec (2026-08-05): a
// 100x100 normalized canvas with "N" label, two open arcs (real angles, not
// GfxRenderer::drawArc's axis-aligned quadrants -- those can only start/end
// on 90 degree boundaries and a naive mirrored pair of half-circles meets
// into a closed ring, which is not this glyph), a plain solid center
// triangle (no concave cutout -- this spec dropped that from an earlier
// draft) and a small separate accent triangle. Drawn straight up because
// the map is always north-up today (see kNoRouteDisplayHeading below); it
// stops being static furniture the day track-up rotation lands.
constexpr int kCompassMarginRight = 40;  // design x=50 (arc/label/triangle center), in from the right edge
constexpr int kCompassTopOffset = 6;     // design y=2 lands here on screen
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
// Halo bounding box in the same design space: left arc's leftmost point (its
// sweep crosses 180 degrees) to the accent triangle's tip, label top to the
// main triangle's base.
constexpr int kCompassDesignMinX = 26, kCompassDesignMaxX = 78;
constexpr int kCompassDesignMinY = 2, kCompassDesignMaxY = 66;
constexpr int kCompassHaloMargin = 5;  // white backing, past the glyph's own bounding box

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

// design x=50 is kCompassMarginRight in from the screen's right edge;
// design y=2 lands at kCompassTopOffset. Every other design coordinate is
// an offset from those two anchors, so the whole glyph moves as one unit.
int compassScreenX(int centerX, int designX) { return centerX + (designX - kCompassLabelCenterX); }
int compassScreenY(int topScreenY, int designY) { return topScreenY + (designY - kCompassDesignMinY); }

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

void MapActivity::drawCompass() {
  const int centerX = renderer.getScreenWidth() - kCompassMarginRight;
  const int topScreenY = kCompassTopOffset;
  auto sx = [&](int designX) { return compassScreenX(centerX, designX); };
  auto sy = [&](int designY) { return compassScreenY(topScreenY, designY); };

  // White halo first, sized to the glyph's design-space bounding box, for
  // the same reason the marker gets one: this sits over live map lines, not
  // blank margin.
  renderer.fillRoundedRect(sx(kCompassDesignMinX) - kCompassHaloMargin, sy(kCompassDesignMinY) - kCompassHaloMargin,
                           (kCompassDesignMaxX - kCompassDesignMinX) + 2 * kCompassHaloMargin,
                           (kCompassDesignMaxY - kCompassDesignMinY) + 2 * kCompassHaloMargin, kCompassHaloMargin,
                           Color::White);

  // 1. "N", centered on the label's own point -- UI_12, not a NotoSans/Serif
  // size: those are compiled out under OMIT_FONTS (platformio.ini's
  // slim-build flag) and silently draw nothing (confirmed on hardware --
  // GfxRenderer logs "Font not found" and skips). UI_10/UI_12/SMALL are the
  // only sizes guaranteed present in every build.
  const char* label = "N";
  const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int labelHeight = renderer.getTextHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, sx(kCompassLabelCenterX) - labelWidth / 2,
                    sy(kCompassLabelCenterY) - labelHeight / 2, label, true);

  // 2. Left arc, 3. right arc -- both open, not a closed ring: each spans
  // 100 degrees, leaving a gap at the top (under "N") and at the bottom
  // (below the triangle) instead of meeting its mirror.
  drawCompassArc(renderer, sx(kCompassArcCx), sy(kCompassArcCy), kCompassArcRadius, kCompassLeftArcStartDeg,
                 kCompassLeftArcEndDeg, kCompassArcLineWidth);
  drawCompassArc(renderer, sx(kCompassArcCx), sy(kCompassArcCy), kCompassArcRadius, kCompassRightArcStartDeg,
                 kCompassRightArcEndDeg, kCompassArcLineWidth);

  // 4. Main center pointer -- a plain solid triangle, drawn after the arcs
  // so it stays the dominant shape on top.
  const int mainXs[3] = {sx(kCompassTriTopX), sx(kCompassTriLeftX), sx(kCompassTriRightX)};
  const int mainYs[3] = {sy(kCompassTriTopY), sy(kCompassTriLeftY), sy(kCompassTriRightY)};
  renderer.fillPolygon(mainXs, mainYs, 3, true);

  // 5. Small accent triangle on the right arc.
  const int accentXs[3] = {sx(kCompassAccentX1), sx(kCompassAccentX2), sx(kCompassAccentX3)};
  const int accentYs[3] = {sy(kCompassAccentY1), sy(kCompassAccentY2), sy(kCompassAccentY3)};
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

MapActivity::MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Map", renderer, mappedInput), transfer_(kTileRoot) {}

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
  const uint32_t heapAfterAlloc = ESP.getFreeHeap();
  LOG_DBG(kLogTag, "heap: %lu before source alloc, %lu after, delta %ld (sizeof MapTileSource = %u)",
          static_cast<unsigned long>(heapBeforeAlloc), static_cast<unsigned long>(heapAfterAlloc),
          static_cast<long>(heapBeforeAlloc) - static_cast<long>(heapAfterAlloc),
          static_cast<unsigned>(sizeof(MapTileSource)));

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

  // Before the fetch branch below: a tile can land at any time, fetch screen
  // up or not -- a phone is free to push a corridor update while the rider is
  // just looking at the map.
  drainTransferredTiles();

  if (fetchPhase_ != FetchPhase::Off) {
    // The fetch screen owns the panel and the buttons while it is up. A
    // position packet or a console command redrawing the map underneath would
    // wipe the progress bar, and there is nowhere else to put it -- the
    // progress screen cannot be its own activity (MapActivity.h).
    //
    // The consoles are still drained: `skip` is how the phone reports a tile
    // it cannot supply, and it has to be able to arrive and be answered
    // exactly while this screen is the one on the panel. Whatever redraw a
    // command asks for is deferred to endFetch()'s renderCurrent().
    serial_.poll();
    ble_.poll();
    updateFetchProgress();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) endFetch();
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
      renderViewport(update.lat, update.lon, update.heading, update.seq);
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
      hasReceivedAny_ = true;
      showingPersistedFix_ = false;
      lastLatE7_ = consoleState_.latE7();
      lastLonE7_ = consoleState_.lonE7();
      lastHeading_ = consoleState_.heading();
      renderViewport(lastLatE7_, lastLonE7_, lastHeading_, static_cast<uint8_t>(consoleState_.seq()));
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
  options.push_back(tr(STR_MAP_FETCH_TILES));
  optionPopup_.show(StrId::STR_MAP, options, initialIndex, [this](int idx) {
    if (idx == 0) {
      redrawDueMs_ = 0;
      showBusy();  // Refresh is the slowest thing on this screen; acknowledge it
      renderCurrent();
    } else if (idx == 2) {
      startFetch();
    } else {
      // Cycle, don't open a second popup -- repeated Select on this row
      // steps ride->hike->cycle->ride. mapRideModeName()'s array order.
      const uint8_t next = (static_cast<uint8_t>(mode_) + 1) % kMapRideModeCount;
      switchMode(static_cast<MapRideMode>(next));
      openMapMenu(1);  // reopen with the label refreshed, Mode still highlighted
    }
  });
  optionPopup_.processRender(renderer, mappedInput);
}

void MapActivity::startFetch() {
  // Priority order first, so the list the phone is about to read is already
  // sorted -- the `missing` command's page 0 would do it too, but the count
  // below and the phone's listing must be talking about the same list.
  MISSING_TILES.sortByFetchPriority();
  fetchTotal_ = static_cast<uint32_t>(MISSING_TILES.hits().size());

  consoleState_.clearSkips();
  fetchBaseCompleted_ = transfer_.status().completed;
  fetchDrawnDone_ = 0;
  fetchDrawnFailed_ = 0;

  if (fetchTotal_ == 0) {
    // Worth its own screen rather than a silent no-op: the rider pressed a
    // button and is owed an answer, and "nothing is missing" is good news.
    fetchPhase_ = FetchPhase::Finished;
    fetchVerdict_ = StrId::STR_MAP_FETCH_NOTHING;
    renderFetchScreen();
    return;
  }

  // Unsolicited indication on the command channel -- the one place the device
  // starts a conversation instead of answering one. Fails if BLE is down or
  // nobody has subscribed, which is exactly the case the rider needs told:
  // without a phone listening, no tile is ever going to arrive.
  char line[32];
  snprintf(line, sizeof(line), "NEED_TILES %lu", static_cast<unsigned long>(fetchTotal_));
  if (!freeink::BlePositionServer::getInstance().sendCommandReply(line)) {
    LOG_ERR(kLogTag, "fetch: NEED_TILES not delivered, nobody subscribed");
    fetchPhase_ = FetchPhase::Finished;
    fetchVerdict_ = StrId::STR_MAP_FETCH_NO_PHONE;
    renderFetchScreen();
    return;
  }

  LOG_INF(kLogTag, "fetch: asked for %lu tiles", static_cast<unsigned long>(fetchTotal_));
  fetchPhase_ = FetchPhase::Running;
  fetchVerdict_ = StrId::STR_MAP_FETCH_DONE;
  renderFetchScreen();
}

void MapActivity::endFetch() {
  if (fetchPhase_ == FetchPhase::Running) {
    // The phone is mid-push and has to be told, because the device cannot stop
    // it from this end: the transfer channel's abort opcode (0x03) is a frame
    // the *central* writes, so the only cancel the peripheral has is a word on
    // the command channel.
    if (!freeink::BlePositionServer::getInstance().sendCommandReply("FETCH_CANCEL")) {
      LOG_ERR(kLogTag, "fetch: FETCH_CANCEL not delivered");
    }
    LOG_INF(kLogTag, "fetch: cancelled by rider");
  }

  fetchPhase_ = FetchPhase::Off;
  // A `zoom`/`mode` command that arrived while the panel was up moved the
  // console's numbers and nothing else; this is where they reach the map.
  syncLaddersFromConsole();
  redrawDueMs_ = 0;
  showBusy();  // full-screen redraw ahead, same as leaving the menu
  renderCurrent();
}

void MapActivity::fetchPanelRect(int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // Three text lines and the bar, centred. Generous rather than tight: this
  // rectangle is what the windowed refresh repaints, and clipping a descender
  // would leave a smear that only a full refresh clears.
  x = metrics.contentSidePadding;
  w = pageWidth - metrics.contentSidePadding * 2;
  h = lineHeight * 3 + metrics.progressBarHeight + metrics.verticalSpacing * 3;
  y = (pageHeight - h) / 2;
}

void MapActivity::drawFetchPanel() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  int px, py, pw, ph;
  fetchPanelRect(px, py, pw, ph);
  // Opaque: this repaints over its own last contents, and stale pixels under
  // new text read as corruption.
  renderer.fillRect(px, py, pw, ph, false);

  const MapTransferReceiver::Status transfer = transfer_.status();
  const uint32_t done = transfer.completed - fetchBaseCompleted_;
  const uint32_t failed = consoleState_.skips().count;

  int lineY = py + metrics.verticalSpacing;
  renderer.drawCenteredText(UI_10_FONT_ID, lineY,
                            fetchPhase_ == FetchPhase::Running ? tr(STR_MAP_FETCHING) : I18N.get(fetchVerdict_));
  lineY += lineHeight + metrics.verticalSpacing;

  char line[64];
  snprintf(line, sizeof(line), "%lu / %lu   fail %lu", static_cast<unsigned long>(done),
           static_cast<unsigned long>(fetchTotal_), static_cast<unsigned long>(failed));
  renderer.drawCenteredText(UI_10_FONT_ID, lineY, line);
  lineY += lineHeight + metrics.verticalSpacing;

  GUI.drawProgressBar(renderer, Rect{px, lineY, pw, metrics.progressBarHeight}, done + failed, fetchTotal_);
  lineY += metrics.progressBarHeight + metrics.verticalSpacing;

  // Which tile is on the wire, or the last one that landed. Bytes as well as
  // the coordinate: a stalled transfer looks identical to a slow one without
  // them, and this line is the only place to tell.
  if (transfer.active) {
    snprintf(line, sizeof(line), "%lu / %lu bytes", static_cast<unsigned long>(transfer.received),
             static_cast<unsigned long>(transfer.total));
    renderer.drawCenteredText(UI_10_FONT_ID, lineY, line);
  } else if (transfer.lastTileValid) {
    snprintf(line, sizeof(line), "z%u %lu/%lu", static_cast<unsigned>(transfer.lastTile.z),
             static_cast<unsigned long>(transfer.lastTile.col), static_cast<unsigned long>(transfer.lastTile.row));
    renderer.drawCenteredText(UI_10_FONT_ID, lineY, line);
  }
}

void MapActivity::renderFetchScreen() {
  renderer.clearScreen();
  drawFetchPanel();
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  busyShown_ = false;  // this frame painted over the badge

  const MapTransferReceiver::Status transfer = transfer_.status();
  fetchDrawnDone_ = transfer.completed - fetchBaseCompleted_;
  fetchDrawnFailed_ = consoleState_.skips().count;
}

void MapActivity::updateFetchProgress() {
  const MapTransferReceiver::Status transfer = transfer_.status();
  const uint32_t done = transfer.completed - fetchBaseCompleted_;
  const uint32_t failed = consoleState_.skips().count;

  // Per tile, not per chunk. A windowed refresh costs a real waveform pass,
  // and a byte counter ticking a hundred times a file would spend the whole
  // transfer refreshing instead of receiving.
  if (done == fetchDrawnDone_ && failed == fetchDrawnFailed_) return;
  fetchDrawnDone_ = done;
  fetchDrawnFailed_ = failed;

  if (done + failed >= fetchTotal_) {
    fetchPhase_ = FetchPhase::Finished;
    fetchVerdict_ = StrId::STR_MAP_FETCH_DONE;
    LOG_INF(kLogTag, "fetch: done, %lu landed, %lu skipped", static_cast<unsigned long>(done),
            static_cast<unsigned long>(failed));
    // Full frame: the button hint changes from Cancel to Back, and that is
    // outside the panel rectangle.
    renderFetchScreen();
    return;
  }

  drawFetchPanel();
  int px, py, pw, ph;
  fetchPanelRect(px, py, pw, ph);
  if (!renderer.displayBufferWindow(px, py, pw, ph)) {
    LOG_ERR(kLogTag, "fetch panel window rejected: %d,%d %dx%d", px, py, pw, ph);
  }
}

void MapActivity::drainTransferredTiles() {
  const MapTransferReceiver::Status transfer = transfer_.status();
  if (!transfer.lastTileValid || transfer.tileSeq == lastClearedTileSeq_) return;
  lastClearedTileSeq_ = transfer.tileSeq;

  if (!MISSING_TILES.forget(transfer.lastTile.z, transfer.lastTile.col, transfer.lastTile.row)) {
    // A tile pushed that the device never hatched -- a corridor update ahead
    // of the ride, say. Nothing to clear, and not an error.
    return;
  }
  LOG_INF(kLogTag, "missing list: z%u %lu/%lu arrived, dropped from the list",
          static_cast<unsigned>(transfer.lastTile.z), static_cast<unsigned long>(transfer.lastTile.col),
          static_cast<unsigned long>(transfer.lastTile.row));
  // Same 10-minute cap the hatch path arms (renderViewport()): a list that
  // still asks for tiles already on the card would have the phone send them
  // again after a restart, so this must reach the card -- but not once per
  // arriving file.
  if (missingTilesSaveDueMs_ == 0) missingTilesSaveDueMs_ = millis() + kMissingTilesSaveIntervalMs;
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
  renderer.clearScreen();
  renderer.drawText(UI_10_FONT_ID, 8, 8, tr(STR_MAP_WAITING_BLE), true);
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  busyShown_ = false;  // this frame painted over the badge
}

void MapActivity::renderCurrent() {
  if (!hasReceivedAny_) {
    renderWaiting();
    return;
  }
  renderViewport(lastLatE7_, lastLonE7_, lastHeading_, lastDrawnSeq_);
}

void MapActivity::renderViewport(int32_t latE7, int32_t lonE7, uint8_t headingStep, uint8_t seq) {
  LOG_DBG(kLogTag, "renderViewport start: lat=%d lon=%d heading=%u seq=%u", (int)latE7, (int)lonE7,
          (unsigned)headingStep, (unsigned)seq);
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
  // North-up, not track-up: there is no route feature on the device yet
  // (roadmap items 13/15), so there is nothing to rotate *toward* -- turning
  // the map to face the BLE heading with no route drawn just spins it for no
  // navigational benefit. headingStep itself is untouched (debug line, LOG_DBG,
  // lastHeading_ still show the real incoming value); only rotation ignores it.
  // Revisit once a route is actually drawn (docs/roadmap.md, "Map rotation
  // model").
  constexpr uint8_t kNoRouteDisplayHeading = static_cast<uint8_t>(MapHeading::N);
  proj_.reset(lat, lon, MapViewport::kAnchorScreenX, markerY, kNoRouteDisplayHeading,
              MapViewport::mppMercFor(zoomStep(), lat));

  const MapViewport::TileRange range =
      MapViewport::tileRangeFor(proj_, tileZ, renderer.getScreenWidth(), renderer.getScreenHeight());
  if (range.count() > MapViewport::kMaxTiles) {
    // A count above 3x3 is a bug in the range arithmetic, not a state.
    LOG_ERR(kLogTag, "tile range %u..%u x %u..%u = %u tiles, over the 3x3 worst case", range.col0, range.col1,
            range.row0, range.row1, range.count());
  }

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

  renderer.clearScreen();
  GfxRendererCanvas canvas(renderer);

  MapViewState view;
  view.markerX = MapViewport::kAnchorScreenX;
  view.markerY = markerY;
  // Same north-up call as proj_.reset() above -- the marker's own triangle
  // is drawn in raw screen direction (MapRenderer.cpp's kHeadingDir, not
  // rotation-aware), so it must agree with proj_'s rotation or the two
  // disagree about which way is "up" the moment heading isn't N.
  view.heading = static_cast<MapHeading>(kNoRouteDisplayHeading);

  // kDefaultMapStyle is the compiled data/mapstyle.json (MapStyleDefaults.h,
  // generated by scripts/gen_mapstyle.py). Nothing overrides it at runtime;
  // the device reads no style file off the card.
  MapRenderer::render(canvas, *source_, view, kDefaultMapStyle);

  // Hatch after the geometry, because which tiles are missing is only known
  // once the source has tried to open them, and asking up front would cost a
  // third read of every tile in the range. A missing tile's own area carries
  // no geometry -- tiles do not overlap -- so the only thing hatch can cover
  // is the marker, which goes back on top below.
  const uint32_t missing = source_->unavailableMask();
  if (missing != 0) {
    for (uint32_t index = 0; index < range.count() && index < 32; ++index) {
      if ((missing & (1u << index)) == 0) continue;
      MapHatch::drawTile(canvas, proj_, range.z, range.colAt(index), range.rowAt(index));
      MISSING_TILES.record(range.z, range.colAt(index), range.rowAt(index));
    }
    // No marker restore here: drawPositionMarker() below is the marker on this
    // screen and it runs after the hatch anyway. Drawing the style's puck
    // first would only leave it peeking out from under a smaller mode marker.

    // Arm only once: a re-hatch of tiles already on the list leaves isDirty()
    // false (MissingTilesStore's own account of a count-only change), and
    // re-arming on every one of those would mean a coverage gap the rider
    // sits in for ten minutes never actually saves. The first genuinely new
    // tile starts the clock; it is not pushed out further after that.
    if (MISSING_TILES.isDirty() && missingTilesSaveDueMs_ == 0) {
      missingTilesSaveDueMs_ = millis() + kMissingTilesSaveIntervalMs;
    }
  }

  // Drawn last and outside IMapCanvas: this is the marker on the device, and
  // it needs the real heading rather than the forced-north one view.heading
  // carries.
  drawPositionMarker(view.markerX, view.markerY, headingStep, mode_);

  // Drawn last and outside IMapCanvas: fixed screen furniture, not map data,
  // so it always lands on top regardless of what the hatch above covered.
  drawCompass();

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

  // Timed above, deliberately: the gate is how long the framebuffer takes to
  // be ready, not how long the panel takes to show it.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  busyShown_ = false;  // this frame painted over the badge
}
