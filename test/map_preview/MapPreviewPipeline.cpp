#include "MapPreviewPipeline.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "HeapProbe.h"
#include "MapHatch.h"
#include "MapProjection.h"
#include "MapLabels.h"
#include "MapRenderer.h"
#include "MapRouteFit.h"
#include "MapRouteSource.h"
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
  const MapStyle& style = request.style ? *request.style : kDefaultMapStyle;

  MapProjection proj;

  // The route is loaded before the projection is built, because --fit-route
  // lets it decide the projection. Its own file source, never the tile source's:
  // both stream during a render and one seek cursor cannot serve two readers
  // (MapRouteSource.h).
  StdioFileSource routeFile;
  std::unique_ptr<MapRouteSource> route;
  if (!request.routePath.empty()) {
    route = std::make_unique<MapRouteSource>(routeFile, proj);
    if (route->load(request.routePath.c_str())) {
      result.routeLoaded = true;
      result.routeName = route->name();
      result.routePoints = route->pointCount();
    } else {
      std::fprintf(stderr, "warning: %s was refused -- bad magic, wrong version, or a failed crc\n",
                   request.routePath.c_str());
      route.reset();
    }
  }

  // What the frame is drawn at. --fit-route overrides all four of these from the
  // route itself, which is what the device does when a route is picked.
  double lat = request.lat;
  double lon = request.lon;
  uint8_t heading = request.heading;
  int zoom = request.zoom;
  int16_t anchorX = style.markerXPx;
  int16_t anchorY = request.markerY != 0 ? request.markerY : style.markerYPx;

  if (request.fitRoute && route) {
    MapRouteFit::Result fit;
    if (route->computeFit(SCREEN_WIDTH, SCREEN_HEIGHT, fit)) {
      lat = fit.anchorLat;
      lon = fit.anchorLon;
      heading = fit.heading;
      zoom = fit.zoomStep;
      // An overview is not a follow frame: it has no look-ahead to reserve, so
      // the anchor is the middle of the screen rather than the marker ladder's
      // rung.
      anchorX = SCREEN_WIDTH / 2;
      anchorY = SCREEN_HEIGHT / 2;
      result.routeFitRan = true;
      result.routeFits = fit.fits;
      result.routeFitHeading = fit.heading;
      result.routeFitZoomStep = fit.zoomStep;
      result.routeFitRequiredMpp = fit.requiredMpp;
      result.routeFitLat = fit.anchorLat;
      result.routeFitLon = fit.anchorLon;
    } else {
      std::fprintf(stderr, "warning: the route fit failed -- the point array read short\n");
    }
  }

  const MapViewport::ZoomStep& lod = MapViewport::kZoomLadder[zoom];
  result.lodZoom = lod.z;
  const int16_t markerY = anchorY;
  result.markerY = markerY;

  proj.reset(lat, lon, anchorX, markerY, heading, MapViewport::mppMercFor(zoom, lat));

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
  view.markerX = anchorX;
  view.markerY = markerY;
  view.heading = static_cast<MapHeading>(heading);
  // Same rung rule the device applies (MapViewport::ZoomStep::buildings), so the
  // laptop preview shows what the panel will show rather than a nicer version of
  // it. Draws buildings at rung 0 only.
  const int rung = std::clamp(request.zoom, 0, MapViewport::kZoomStepCount - 1);
  view.drawBuildings = request.drawBuildings.value_or(MapViewport::kZoomLadder[rung].buildings);
  view.drawBuiltUp = MapViewport::kZoomLadder[rung].builtUp;
  // The rung's own label ceiling, so this pane thins names exactly where the
  // panel does (MapViewport::ZoomStep::maxLabels).
  view.maxLabels = MapViewport::kZoomLadder[rung].maxLabels;

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
  // Same screen test the device applies, from the same style, so the golden
  // render is what proves the test drops nothing visible
  // (MapTileSource::Config).
  if (request.rejectOffScreen) {
    config.screenWidth = SCREEN_WIDTH;
    config.screenHeight = SCREEN_HEIGHT;
    config.rejectMarginPx = mapStyleMaxStrokePx(style);
  }
  source->begin(config);

  result.sourceBytes = sizeof(MapTileSource);

  // Everything resident is already allocated. What the render itself costs
  // on top of that is what the reset..read window measures, and the answer
  // should be nothing.
  // Label scratch on the stack, deliberately: it is ~3.2 KB and the heap probe
  // below is meant to catch allocations the *render* makes, so the one buffer
  // the render is allowed to need must not be one of them. On the device
  // MapActivity owns the same struct as a member (MapLabels.h).
  MapLabelScratch labels;

  HeapProbe::reset();
  MapRenderer::render(canvas, *source, view, style, route.get(), nullptr, nullptr, &labels);
  // render() does not draw the marker (MapActivity draws its own mode-specific
  // one). This preview has no travel mode, so it draws the style's puck
  // explicitly -- except in a route overview, which is framed on the route and
  // has no fix in it. A puck at the screen centre there would claim the rider is
  // standing in the middle of their own route.
  const bool drawPuck = !result.routeFitRan && request.drawMarker;
  if (drawPuck) MapRenderer::drawMarker(canvas, view.markerX, view.markerY, view.heading, style);
  result.peakHeapDuringRender = HeapProbe::peakBytes();
  result.allocsDuringRender = HeapProbe::allocCount();

  // The source counts every record it hands out, and the renderer asks for the
  // road layer MapRenderer::kRoadPasses times. What a reader of these numbers
  // wants is ways in the picture, i.e. one walk's worth.
  result.waysDrawn = source->waysEmitted() / MapRenderer::kRoadPasses;
  result.waysFiltered = source->waysFiltered() / MapRenderer::kRoadPasses;
  result.waysOffScreen = source->waysOffScreen() / MapRenderer::kRoadPasses;
  result.crc32Skipped = source->crc32Skipped();
  result.placesDrawn = source->placesEmitted();
  result.labelsPlaced = labels.placed;
  result.labelsDropped = labels.dropped;
  result.bytesRead = source->bytesRead();
  result.tilesLoaded = static_cast<int>(source->tilesOpened());
  result.tilesMissing = static_cast<int>(source->tilesUnavailable());
  result.missingMask = source->unavailableMask();
  if (route) result.routeBytesRead = route->bytesRead();

  // Off by default, and deliberately: the committed golden PPM is the only
  // safety net the streaming refactor has, and it is never regenerated to
  // make a test pass. The device always hatches (MapActivity); this flag is
  // here so the same drawing can be eyeballed on the laptop first.
  if (request.drawHatch && result.missingMask != 0) {
    for (uint32_t index = 0; index < range.count() && index < 32; ++index) {
      if ((result.missingMask & (1u << index)) == 0) continue;
      MapHatch::drawTile(canvas, proj, range.z, range.colAt(index), range.rowAt(index));
    }
    if (drawPuck) MapRenderer::drawMarker(canvas, view.markerX, view.markerY, view.heading, style);
  }

  return result;
}
