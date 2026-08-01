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
    state.heading = static_cast<MapHeading>(heading % 8);
  }

  MapRenderer::render(canvas, state);

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
