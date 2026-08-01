#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "IMapCanvas.h"
#include "MapHeading.h"

// Screen-space (pixels), not geo-coordinates -- whatever loads the base
// map/route (mapbuilder format, later) is responsible for projecting into
// this space before handing it to MapRenderer.
struct MapViewState {
  std::vector<std::pair<int16_t, int16_t>> roadPolyline;
  std::vector<std::pair<int16_t, int16_t>> villageDots;
  int16_t markerX = 0;
  int16_t markerY = 0;
  MapHeading heading = MapHeading::N;
};

// Draws base map (road, village dots) then the position marker, in that
// order, onto whatever IMapCanvas it's given. No hardware/HAL dependency --
// this is what both the native preview and MapActivity call.
class MapRenderer {
 public:
  static void render(IMapCanvas& canvas, const MapViewState& state);

 private:
  static void drawMarker(IMapCanvas& canvas, int16_t x, int16_t y, MapHeading heading);
};
