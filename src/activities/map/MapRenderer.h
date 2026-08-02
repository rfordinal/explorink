#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "IMapCanvas.h"
#include "MapHeading.h"

// One roads-layer way, already projected to screen space. class_id/
// roughness/flags are carried straight from the .tib record (see
// docs/map-data-spec.md, "Way record") for future per-class styling; P2
// draws every way the same way regardless of them.
struct MapWay {
  uint8_t classId = 0;
  uint8_t roughness = 0;
  uint16_t flags = 0;
  std::vector<std::pair<int16_t, int16_t>> points;
};

// Screen-space (pixels), not geo-coordinates -- whatever loads the base
// map/route (mapbuilder format) is responsible for projecting into this
// space (MapProjection) before handing it to MapRenderer.
struct MapViewState {
  std::vector<MapWay> ways;
  std::vector<std::pair<int16_t, int16_t>> placeDots;
  int16_t markerX = 0;
  int16_t markerY = 0;
  MapHeading heading = MapHeading::N;
};

// Draws base map (roads, place dots) then the position marker, in that
// order, onto whatever IMapCanvas it's given. No hardware/HAL dependency --
// this is what both the native preview and MapActivity call.
class MapRenderer {
 public:
  static void render(IMapCanvas& canvas, const MapViewState& state);

 private:
  static void drawMarker(IMapCanvas& canvas, int16_t x, int16_t y, MapHeading heading);
};
