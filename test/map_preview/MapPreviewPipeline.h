#pragma once

#include <cstdint>
#include <string>

#include "MapProjection.h"
#include "MapRenderer.h"

// Shared by test/map_preview (the CLI tool) and the golden-file test
// (test/map_tile_reader) -- one place that turns "a coordinate plus a tiles
// directory" into a projected MapViewState, so the golden test renders
// through exactly the same path the CLI does.
struct MapPreviewResult {
  MapViewState state;
  int tilesLoaded = 0;
  int tilesMissing = 0;
  long smallestTileBytes = -1;
  long largestTileBytes = -1;
  uint8_t lodZoom = 0;
};

// heading: 0-15. zoom: 0-4 (docs/map-data-spec.md zoom ladder). Reads only
// the roads and places layers of every tile the viewport touches, skipping
// water/buildings/junctions via the layer directory
// (docs/prototype-plan.md, P2: "Read only what you draw").
MapPreviewResult buildMapPreview(const std::string& tilesDir, double lat, double lon, uint8_t heading, int zoom);
