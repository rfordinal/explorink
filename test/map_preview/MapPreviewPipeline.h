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
  uint32_t placesDrawn = 0;
  uint8_t lodZoom = 0;
  uint32_t col0 = 0, row0 = 0, col1 = 0, row1 = 0;
  long smallestTileBytes = -1;
  long largestTileBytes = -1;

  // The streaming path's whole RAM cost, split into its two honest halves.
  // `sourceBytes` is sizeof(MapTileSource) -- reader buffer, one way's point
  // scratch, one place name, one path -- allocated once before rendering.
  // `peakHeapDuringRender` is a measured high-water mark of C++ operator new
  // over the render itself (HeapProbe.h); it should be zero, because the
  // renderer holds nothing.
  size_t sourceBytes = 0;
  size_t peakHeapDuringRender = 0;
  size_t allocsDuringRender = 0;
};

MapPreviewResult renderMapPreview(const MapPreviewRequest& request, IMapCanvas& canvas);
