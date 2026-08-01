#include "MapActivity.h"

#include <BlePositionServer.h>
#include <I18n.h>

#include <cstdio>

#include "GfxRendererCanvas.h"
#include "MockMapData.h"
#include "fontIds.h"

MapActivity::MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Map", renderer, mappedInput) {}

void MapActivity::onEnter() {
  Activity::onEnter();

  freeink::BlePositionServer::getInstance().begin();
  hasReceivedAny_ = false;
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
  const MapViewState state = buildMockMapViewState(renderer.getScreenWidth(), renderer.getScreenHeight());
  MapRenderer::render(canvas, state);

  // Temporary Phase 3 debug readout -- proves BLE data is flowing,
  // independent of the map/marker rendering above (not wired into the
  // marker position yet, see "Wire received BLE position into actual map
  // marker" in docs/roadmap.md).
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
