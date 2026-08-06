#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "IMapCanvas.h"
#include "MapStyle.h"

// Shared by test/map_preview (the CLI tool) and the golden-file test
// (test/map_tile_reader) -- one place that turns "a coordinate plus a tiles
// directory" into a rendered canvas, so the golden test renders through
// exactly the same path the CLI does.
//
// Nothing here holds map geometry. It picks the viewport's tile range,
// builds a MapTileSource over it, and hands that to MapRenderer, which
// pulls one record at a time (see src/activities/map/IMapSource.h).
struct MapPreviewRequest {
  std::string tilesDir;
  double lat = 0.0;
  double lon = 0.0;
  uint8_t heading = 0;  // 0-15
  int zoom = 0;         // 0-4, docs/map-data-spec.md zoom ladder

  // Marker's screen row, i.e. the vertical anchor. 0 means "take the style's
  // device.marker_y_px", which is what the CLI renders at. The device instead
  // takes this off the marker-height ladder (MapViewport::kMarkerLadder),
  // which is why it is a free parameter here rather than a constant.
  int16_t markerY = 0;

  // Style to draw with. nullptr means kDefaultMapStyle, i.e. the compiled
  // data/mapstyle.json -- the same numbers the firmware build gets, which is
  // the point of previewing here at all. The golden test pins its own frozen
  // style instead, so a style edit cannot break a fixture that exists to
  // guard the tile pipeline.
  const MapStyle* style = nullptr;

  // Render-time mode filter, `mask & (1 << class_id)`. All ones is every
  // class, which is what the golden test renders and what the CLI does
  // unless --mode is given.
  uint32_t classMask = 0xFFFFFFFFu;

  // Drop geometry whose bounding box cannot reach the screen before projecting
  // it (MapTileSource::Config::screenWidth). On by default, exactly as the
  // device does it. False exists for one reason: a test that renders the same
  // view both ways and asserts the pixels match
  // (MapOffScreenReject in test/map_tile_reader/).
  bool rejectOffScreen = true;

  // Render exactly one named tile instead of the whole tile range the
  // viewport touches. Only useful for quoting a per-tile RAM figure against
  // a per-tile figure from the old pipeline; leave it off for real previews.
  bool singleTile = false;
  uint32_t tileCol = 0;
  uint32_t tileRow = 0;

  // Draw the missing-tile hatch (src/activities/map/MapHatch.h). Off by
  // default so the committed golden PPM stays byte-identical -- the device
  // always hatches; this is for eyeballing the same drawing on the laptop.
  bool drawHatch = false;

  // A .tir route file to draw over the tiles, or empty for none
  // (../../../docs/route-file-spec.md in the parent xteink repo). This is what
  // makes route styling a two-second laptop edit rather than a flash.
  std::string routePath;

  // Frame the whole route instead of the lat/lon/heading/zoom above: run
  // MapRouteFit over the loaded route and use what it picks. Exactly what the
  // device does when the rider selects a route (MapActivity's route overview),
  // so a fit can be checked on a real route and real tiles without hardware.
  //
  // With this set, lat/lon are ignored and the anchor is the screen centre.
  bool fitRoute = false;
};

struct MapPreviewResult {
  // The marker row actually rendered at -- the request's value, or the
  // style's when the request left it at 0.
  int16_t markerY = 0;
  int tilesLoaded = 0;
  int tilesMissing = 0;
  // Bit per tile of the range, column-major -- what MapActivity hatches.
  uint32_t missingMask = 0;
  uint32_t waysDrawn = 0;
  uint32_t waysFiltered = 0;  // read off the card, dropped by classMask
  // Read off the card, dropped because the way's own bounding box cannot reach
  // the screen (MapTileSource::waysOffScreen). Every record read is exactly one
  // of drawn, filtered or off-screen, which is what the mode-filter test
  // asserts.
  uint32_t waysOffScreen = 0;
  // Layer crc32 checks skipped because that (tile, layer) pair already passed in
  // this frame (MapTileSource::crc32Skipped). Non-zero means the repeat check is
  // gone; each one is a full read of that layer's bytes not paid.
  uint32_t crc32Skipped = 0;
  uint32_t placesDrawn = 0;
  uint8_t lodZoom = 0;
  uint32_t col0 = 0, row0 = 0, col1 = 0, row1 = 0;
  long smallestTileBytes = -1;
  long largestTileBytes = -1;

  // Real bytes read off the "card" across every layer and every pass. This is
  // the number that makes turning the buildings layer on an informed decision
  // rather than a cosmetic one -- buildings were 277 KB of the 364 KB a
  // four-tile viewport read (docs/map-data-spec.md, "RAM budget").
  uint32_t bytesRead = 0;

  // The streaming path's whole RAM cost, split into its two honest halves.
  // `sourceBytes` is sizeof(MapTileSource) -- reader buffer, one way's point
  // scratch, one place name, one path -- allocated once before rendering.
  // `peakHeapDuringRender` is a measured high-water mark of C++ operator new
  // over the render itself (HeapProbe.h); it should be zero, because the
  // renderer holds nothing.
  size_t sourceBytes = 0;
  size_t peakHeapDuringRender = 0;
  size_t allocsDuringRender = 0;

  // The route, when one was given. `routeLoaded` false with a non-empty
  // routePath means the file was rejected -- bad magic, wrong version, or a
  // failed crc -- and nothing was drawn, which is deliberate: half a route ends
  // somewhere it does not.
  bool routeLoaded = false;
  std::string routeName;
  uint32_t routePoints = 0;
  uint32_t routeBytesRead = 0;
  // Filled when fitRoute was set and the fit ran. `routeFits` false means even
  // the coarsest rung could not hold the whole route, and the frame shows as
  // much of it as 20 m/px allows.
  bool routeFitRan = false;
  bool routeFits = false;
  uint8_t routeFitHeading = 0;
  uint8_t routeFitZoomStep = 0;
  double routeFitRequiredMpp = 0.0;
  double routeFitLat = 0.0;
  double routeFitLon = 0.0;
};

MapPreviewResult renderMapPreview(const MapPreviewRequest& request, IMapCanvas& canvas);
