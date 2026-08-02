#include "MapActivity.h"

#include <BlePositionServer.h>
#include <I18n.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "GfxRendererCanvas.h"
#include "MapHatch.h"
#include "MapRenderer.h"
#include "MapViewport.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "MAP";

// docs/map-data-spec.md, "Layers as separate files".
constexpr const char* kTileRoot = "/trailink";

// Fixed until P5 puts the ladder on the hardware buttons. Step 0 is the
// detail LOD (z13, 3 m/px) -- the closest rung, and what hike mode starts
// on. docs/map-data-spec.md, "Zoom is a hardware button, so zoom is a
// ladder".
constexpr int kZoomStep = 0;

// Debug readout geometry.
constexpr int kTextX = 8;
constexpr int kTextLine1Y = 8;
constexpr int kTextLine2Y = 26;

}  // namespace

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
    : Activity("Map", renderer, mappedInput) {}

void MapActivity::onEnter() {
  Activity::onEnter();

  freeink::BlePositionServer::getInstance().begin();
  hasReceivedAny_ = false;
  lastDrawnSeq_ = 0;

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

  renderWaiting();
}

void MapActivity::onExit() {
  Activity::onExit();
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

  freeink::PositionUpdate update;
  if (freeink::BlePositionServer::getInstance().getLatest(update)) {
    if (!hasReceivedAny_ || update.seq != lastDrawnSeq_) {
      hasReceivedAny_ = true;
      lastDrawnSeq_ = update.seq;
      renderViewport(update.lat, update.lon, update.heading, update.seq);
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::MAP);
  }
}

bool MapActivity::preventAutoSleep() { return freeink::BlePositionServer::getInstance().isRunning(); }

void MapActivity::renderWaiting() {
  renderer.clearScreen();
  renderer.drawText(UI_10_FONT_ID, 8, 8, tr(STR_MAP_WAITING_BLE), true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void MapActivity::renderViewport(int32_t latE7, int32_t lonE7, uint8_t heading, uint8_t seq) {
  if (!source_) {
    renderWaiting();
    return;
  }

  const uint32_t startMs = millis();
  const uint32_t heapBefore = ESP.getFreeHeap();

  const double lat = static_cast<double>(latE7) / 1e7;
  const double lon = static_cast<double>(lonE7) / 1e7;

  // BlePositionServer's 12-byte wire format still carries the old 8-step
  // heading -- *2 lands it on the same compass directions in the 16-step
  // enum instead of on the new intermediate steps. The packet widens in P5.
  const uint8_t headingStep = static_cast<uint8_t>((heading % 8) * 2);

  const uint8_t tileZ = MapViewport::kZoomLadder[kZoomStep].z;
  proj_.reset(lat, lon, MapViewport::kAnchorScreenX, MapViewport::kAnchorScreenY, headingStep,
              MapViewport::mppMercFor(kZoomStep, lat));

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
  source_->begin(config);

  renderer.clearScreen();
  GfxRendererCanvas canvas(renderer);

  MapViewState view;
  view.markerX = MapViewport::kAnchorScreenX;
  view.markerY = MapViewport::kAnchorScreenY;
  view.heading = static_cast<MapHeading>(headingStep);

  MapRenderer::render(canvas, *source_, view);

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
    }
    MapRenderer::drawMarker(canvas, view.markerX, view.markerY, view.heading);
  }

  const uint32_t elapsedMs = millis() - startMs;
  const uint32_t heapAfter = ESP.getFreeHeap();

  // Debug readout, kept from the BLE checkpoint: the raw values driving the
  // marker, plus what the viewport reset actually cost.
  char line[80];
  snprintf(line, sizeof(line), "%.5f %.5f h%u #%u", lat, lon, heading, seq);
  drawDebugLine(kTextLine1Y, line);
  snprintf(line, sizeof(line), "z%u %lut %luw %lums", range.z, static_cast<unsigned long>(source_->tilesOpened()),
           static_cast<unsigned long>(source_->waysEmitted()), static_cast<unsigned long>(elapsedMs));
  drawDebugLine(kTextLine2Y, line);

  LOG_DBG(kLogTag, "reset z%u col %u..%u row %u..%u: %lu tiles ok, %lu missing (mask 0x%lx), %lu ways, %lu places",
          range.z, range.col0, range.col1, range.row0, range.row1,
          static_cast<unsigned long>(source_->tilesOpened()), static_cast<unsigned long>(source_->tilesUnavailable()),
          static_cast<unsigned long>(missing), static_cast<unsigned long>(source_->waysEmitted()),
          static_cast<unsigned long>(source_->placesEmitted()));
  LOG_DBG(kLogTag, "heap: %lu before tile load, %lu after, delta %ld; framebuffer ready in %lu ms",
          static_cast<unsigned long>(heapBefore), static_cast<unsigned long>(heapAfter),
          static_cast<long>(heapBefore) - static_cast<long>(heapAfter), static_cast<unsigned long>(elapsedMs));

  // Timed above, deliberately: the gate is how long the framebuffer takes to
  // be ready, not how long the panel takes to show it.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
