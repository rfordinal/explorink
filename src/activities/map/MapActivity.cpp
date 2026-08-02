#include "MapActivity.h"

#include <BlePositionServer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "GfxRendererCanvas.h"
#include "MockMapData.h"
#include "fontIds.h"

namespace {

// Placeholder linear projection from raw lat/lon to screen space: the
// first update received becomes the screen center, later updates offset
// from it by a fixed pixels-per-degree scale. NOT a real map projection
// (mapbuilder's Web Mercator format, see architecture-plan.md, is the real
// one) -- just enough to prove BLE data actually drives the marker, until
// on-device base-map loading replaces this (docs/roadmap.md item 7).
constexpr float kPlaceholderPixelsPerDegree = 50000.0f;
constexpr int kMarkerMargin = 20;

void projectPlaceholder(int32_t lat, int32_t lon, int32_t originLat, int32_t originLon, int screenWidth,
                        int screenHeight, int16_t& outX, int16_t& outY) {
  const float dLat = static_cast<float>(lat - originLat) / 1e7f;
  const float dLon = static_cast<float>(lon - originLon) / 1e7f;
  const int centerX = screenWidth / 2;
  const int centerY = screenHeight / 2;
  const int x = centerX + static_cast<int>(dLon * kPlaceholderPixelsPerDegree);
  const int y = centerY - static_cast<int>(dLat * kPlaceholderPixelsPerDegree);  // lat north = up = -y
  outX = static_cast<int16_t>(std::clamp(x, kMarkerMargin, screenWidth - kMarkerMargin));
  outY = static_cast<int16_t>(std::clamp(y, kMarkerMargin, screenHeight - kMarkerMargin));
}

}  // namespace

MapActivity::MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Map", renderer, mappedInput) {}

void MapActivity::onEnter() {
  Activity::onEnter();

  freeink::BlePositionServer::getInstance().begin();
  hasReceivedAny_ = false;
  hasOrigin_ = false;
  lastDrawnSeq_ = 0;

  renderDebugReadout(false, 0, 0, 0, 0);
}

void MapActivity::onExit() {
  Activity::onExit();
  freeink::BlePositionServer::getInstance().end();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void MapActivity::loop() {
  Activity::loop();

  freeink::PositionUpdate update;
  if (freeink::BlePositionServer::getInstance().getLatest(update)) {
    if (!hasReceivedAny_ || update.seq != lastDrawnSeq_) {
      hasReceivedAny_ = true;
      lastDrawnSeq_ = update.seq;
      if (!hasOrigin_) {
        hasOrigin_ = true;
        originLat_ = update.lat;
        originLon_ = update.lon;
      }
      renderDebugReadout(true, update.lat, update.lon, update.heading, update.seq);
    }
  }

  // P3 serial command console -- see MapSerialConsole.h. Non-blocking.
  if (console_.poll()) {
    const MapConsoleState& cs = console_.state();
    if (!hasOrigin_) {
      hasOrigin_ = true;
      originLat_ = cs.latE7();
      originLon_ = cs.lonE7();
    }
    hasReceivedAny_ = true;
    // heading()/2 because the debug readout still speaks the BLE packet's
    // 8-step heading; the odd 16-step values round down until the real
    // projection lands.
    renderDebugReadout(true, cs.latE7(), cs.lonE7(), static_cast<uint8_t>(cs.heading() / 2),
                       static_cast<uint8_t>(cs.seq()));
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::MAP);
  }
}

bool MapActivity::preventAutoSleep() { return freeink::BlePositionServer::getInstance().isRunning(); }

void MapActivity::renderDebugReadout(bool haveUpdate, int32_t lat, int32_t lon, uint8_t heading, uint8_t seq) {
  renderer.clearScreen();
  GfxRendererCanvas canvas(renderer);
  MapViewState state = buildMockMapViewState(renderer.getScreenWidth(), renderer.getScreenHeight());

  if (haveUpdate) {
    projectPlaceholder(lat, lon, originLat_, originLon_, renderer.getScreenWidth(), renderer.getScreenHeight(),
                       state.markerX, state.markerY);
    // BlePositionServer.h's wire format still sends 0-7 (the old 8-step
    // MapHeading) -- *2 maps it onto the same compass directions in the
    // now-16-step enum (see MapHeading.h) instead of landing on the new
    // intermediate steps.
    state.heading = static_cast<MapHeading>((heading % 8) * 2);
  }

  MockMapSource source(renderer.getScreenWidth(), renderer.getScreenHeight());
  MapRenderer::render(canvas, source, state);

  // Debug line -- proves the raw values driving the marker above, kept
  // alongside it (not just before it) since it's still useful to confirm
  // the placeholder projection is doing something sane.
  if (haveUpdate) {
    char line[64];
    snprintf(line, sizeof(line), "BLE lat=%ld lon=%ld hdg=%u seq=%u", static_cast<long>(lat), static_cast<long>(lon),
             heading, seq);
    renderer.drawText(UI_10_FONT_ID, 8, 8, line, true);
  } else {
    renderer.drawText(UI_10_FONT_ID, 8, 8, tr(STR_MAP_WAITING_BLE), true);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
