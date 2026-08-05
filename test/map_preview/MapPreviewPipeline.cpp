#include "MapPreviewPipeline.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "HeapProbe.h"
#include "MapHatch.h"
#include "MapProjection.h"
#include "MapRenderer.h"
#include "MapStyleDefaults.h"
#include "MapTileGrid.h"
#include "MapTileSource.h"
#include "MapViewport.h"
#include "StdioFileSource.h"

namespace {

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 800;

long fileSizeBytes(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return -1;
  return static_cast<long>(st.st_size);
}

std::string tilePath(const std::string& tilesDir, uint8_t z, uint32_t col, uint32_t row) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/base/%u/%u/%u.tib", z, col, row);
  return tilesDir + buf;
}

}  // namespace

MapPreviewResult renderMapPreview(const MapPreviewRequest& request, IMapCanvas& canvas) {
  MapPreviewResult result;
  const MapViewport::ZoomStep& lod = MapViewport::kZoomLadder[request.zoom];
  result.lodZoom = lod.z;

  const MapStyle& style = request.style ? *request.style : kDefaultMapStyle;
  const int16_t markerY = request.markerY != 0 ? request.markerY : style.markerYPx;
  result.markerY = markerY;

  MapProjection proj;
  proj.reset(request.lat, request.lon, style.markerXPx, markerY, request.heading,
             MapViewport::mppMercFor(request.zoom, request.lat));

  MapViewport::TileRange range;
  if (request.singleTile) {
    range.z = lod.z;
    range.col0 = range.col1 = request.tileCol;
    range.row0 = range.row1 = request.tileRow;
  } else {
    range = MapViewport::tileRangeFor(proj, lod.z, SCREEN_WIDTH, SCREEN_HEIGHT);
    if (range.count() > MapViewport::kMaxTiles) {
      std::fprintf(stderr, "warning: %u tiles needed (col %u..%u, row %u..%u) -- more than the 3x3 worst case\n",
                   range.count(), range.col0, range.col1, range.row0, range.row1);
    }
  }
  result.col0 = range.col0;
  result.col1 = range.col1;
  result.row0 = range.row0;
  result.row1 = range.row1;

  // On-disk sizes, so the O(1) claim can be read against how much the tiles
  // themselves vary. Pure reporting -- the renderer never sees this.
  for (uint32_t col = result.col0; col <= result.col1; ++col) {
    for (uint32_t row = result.row0; row <= result.row1; ++row) {
      const long size = fileSizeBytes(tilePath(request.tilesDir, lod.z, col, row));
      if (size < 0) continue;
      if (result.smallestTileBytes < 0 || size < result.smallestTileBytes) result.smallestTileBytes = size;
      if (size > result.largestTileBytes) result.largestTileBytes = size;
    }
  }

  MapViewState view;
  view.markerX = style.markerXPx;
  view.markerY = markerY;
  view.heading = static_cast<MapHeading>(request.heading);

  StdioFileSource file;
  // Heap, not a local: MapTileSource is ~5 KB and CLAUDE.md caps stack
  // locals at 256 bytes. On the device this is makeUniqueNoThrow in the
  // activity's onEnter(); here std::make_unique is the same shape.
  auto source = std::make_unique<MapTileSource>(file, proj);

  MapTileSource::Config config;
  config.rootDir = request.tilesDir.c_str();
  config.z = lod.z;
  config.col0 = result.col0;
  config.row0 = result.row0;
  config.col1 = result.col1;
  config.row1 = result.row1;
  config.classMask = request.classMask;
  source->begin(config);

  result.sourceBytes = sizeof(MapTileSource);

  // Everything resident is already allocated. What the render itself costs
  // on top of that is what the reset..read window measures, and the answer
  // should be nothing.
  HeapProbe::reset();
  MapRenderer::render(canvas, *source, view, style);
  // render() does not draw the marker (MapActivity draws its own mode-specific
  // one). This preview has no travel mode, so it draws the style's puck
  // explicitly.
  MapRenderer::drawMarker(canvas, view.markerX, view.markerY, view.heading, style);
  result.peakHeapDuringRender = HeapProbe::peakBytes();
  result.allocsDuringRender = HeapProbe::allocCount();

  // The source counts every record it hands out, and the renderer asks for the
  // road layer MapRenderer::kRoadPasses times. What a reader of these numbers
  // wants is ways in the picture, i.e. one walk's worth.
  result.waysDrawn = source->waysEmitted() / MapRenderer::kRoadPasses;
  result.waysFiltered = source->waysFiltered() / MapRenderer::kRoadPasses;
  result.placesDrawn = source->placesEmitted();
  result.tilesLoaded = static_cast<int>(source->tilesOpened());
  result.tilesMissing = static_cast<int>(source->tilesUnavailable());
  result.missingMask = source->unavailableMask();

  // Off by default, and deliberately: the committed golden PPM is the only
  // safety net the streaming refactor has, and it is never regenerated to
  // make a test pass. The device always hatches (MapActivity); this flag is
  // here so the same drawing can be eyeballed on the laptop first.
  if (request.drawHatch && result.missingMask != 0) {
    for (uint32_t index = 0; index < range.count() && index < 32; ++index) {
      if ((result.missingMask & (1u << index)) == 0) continue;
      MapHatch::drawTile(canvas, proj, range.z, range.colAt(index), range.rowAt(index));
    }
    MapRenderer::drawMarker(canvas, view.markerX, view.markerY, view.heading, style);
  }

  return result;
}
