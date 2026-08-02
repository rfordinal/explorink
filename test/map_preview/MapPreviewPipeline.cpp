#include "MapPreviewPipeline.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "HeapProbe.h"
#include "MapProjection.h"
#include "MapRenderer.h"
#include "MapTileGrid.h"
#include "MapTileSource.h"
#include "StdioFileSource.h"

namespace {

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 800;

// style.device.marker_x_px / marker_y_px default -- docs/map-render-spec.md,
// "Marker freedom is implemented in mapbuilder". The viewport re-anchors on
// the marker at this screen position on every reset, so this harness treats
// the requested lat/lon as both the anchor and the marker's own fix.
constexpr int16_t kAnchorScreenX = 230;
constexpr int16_t kAnchorScreenY = 620;

// Label overhang margin for the geometry bbox -- docs/map-data-spec.md,
// "Which tiles to load".
constexpr double kMarginPx = 64.0;

struct LodStep {
  double mpp;
  uint8_t z;
};

// docs/map-data-spec.md, "Zoom is a hardware button, so zoom is a ladder".
constexpr LodStep kZoomLadder[5] = {
    {3.0, 13},   // step 0, detail
    {5.0, 13},   // step 1, detail
    {7.5, 12},   // step 2, regional
    {11.0, 11},  // step 3, overview
    {15.0, 11},  // step 4, overview
};

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

// docs/map-data-spec.md, "Which tiles to load": rotate the viewport rect by
// heading, take its axis-aligned Mercator bbox, inflate by the label
// margin, then map to the tile grid at this LOD's zoom.
void tileRangeForViewport(const MapProjection& proj, uint8_t z, uint32_t& col0, uint32_t& row0, uint32_t& col1,
                          uint32_t& row1) {
  double minX = 0, minY = 0, maxX = 0, maxY = 0;
  const int corners[4][2] = {{0, 0}, {SCREEN_WIDTH, 0}, {0, SCREEN_HEIGHT}, {SCREEN_WIDTH, SCREEN_HEIGHT}};
  for (int i = 0; i < 4; ++i) {
    double mx, my;
    proj.screenToMerc(static_cast<int16_t>(corners[i][0]), static_cast<int16_t>(corners[i][1]), mx, my);
    if (i == 0) {
      minX = maxX = mx;
      minY = maxY = my;
    } else {
      minX = std::min(minX, mx);
      maxX = std::max(maxX, mx);
      minY = std::min(minY, my);
      maxY = std::max(maxY, my);
    }
  }
  const double marginMerc = kMarginPx * proj.mppMerc();
  minX -= marginMerc;
  maxX += marginMerc;
  minY -= marginMerc;
  maxY += marginMerc;

  uint32_t cA, rA, cB, rB;
  MapTileGrid::mercToTileColRow(minX, maxY, z, cA, rA);  // NW corner
  MapTileGrid::mercToTileColRow(maxX, minY, z, cB, rB);  // SE corner
  col0 = std::min(cA, cB);
  col1 = std::max(cA, cB);
  row0 = std::min(rA, rB);
  row1 = std::max(rA, rB);
}

}  // namespace

MapPreviewResult renderMapPreview(const MapPreviewRequest& request, IMapCanvas& canvas) {
  MapPreviewResult result;
  const LodStep& lod = kZoomLadder[request.zoom];
  result.lodZoom = lod.z;

  const double mppMerc = lod.mpp / std::cos(request.lat * 3.14159265358979323846 / 180.0);

  MapProjection proj;
  proj.reset(request.lat, request.lon, kAnchorScreenX, kAnchorScreenY, request.heading, mppMerc);

  if (request.singleTile) {
    result.col0 = result.col1 = request.tileCol;
    result.row0 = result.row1 = request.tileRow;
  } else {
    tileRangeForViewport(proj, lod.z, result.col0, result.row0, result.col1, result.row1);
    const uint32_t tileCount = (result.col1 - result.col0 + 1) * (result.row1 - result.row0 + 1);
    if (tileCount > 9) {
      std::fprintf(stderr, "warning: %u tiles needed (col %u..%u, row %u..%u) -- more than the 3x3 worst case\n",
                   tileCount, result.col0, result.col1, result.row0, result.row1);
    }
  }

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
  view.markerX = kAnchorScreenX;
  view.markerY = kAnchorScreenY;
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
  source->begin(config);

  result.sourceBytes = sizeof(MapTileSource);

  // Everything resident is already allocated. What the render itself costs
  // on top of that is what the reset..read window measures, and the answer
  // should be nothing.
  HeapProbe::reset();
  MapRenderer::render(canvas, *source, view);
  result.peakHeapDuringRender = HeapProbe::peakBytes();
  result.allocsDuringRender = HeapProbe::allocCount();

  result.waysDrawn = source->waysEmitted();
  result.placesDrawn = source->placesEmitted();
  result.tilesLoaded = static_cast<int>(source->tilesOpened());
  result.tilesMissing = static_cast<int>(source->tilesUnavailable());

  return result;
}
