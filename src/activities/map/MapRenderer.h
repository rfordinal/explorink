#pragma once

#include <cstdint>

#include "IMapCanvas.h"
#include "IMapSource.h"
#include "MapHeading.h"

// Everything the renderer needs that is not geometry. Small and resident by
// definition -- the marker is one point and the heading is one byte. All
// map geometry arrives through IMapSource, one record at a time, and is
// never held (see IMapSource.h for why).
//
// Screen-space (pixels), not geo-coordinates -- MapProjection does that
// conversion inside the source.
struct MapViewState {
  int16_t markerX = 0;
  int16_t markerY = 0;
  MapHeading heading = MapHeading::N;
};

// Draws the base map (roads, place dots) onto whatever IMapCanvas it's
// given. No hardware/HAL dependency -- this is what both the native preview
// and MapActivity call.
//
// Does NOT draw the position marker (see drawMarker() below) -- MapActivity
// draws its own mode-specific one straight through GfxRenderer instead,
// because it needs a white halo fill IMapCanvas cannot express.
//
// render() pulls: it holds no geometry of its own, so its RAM cost does not
// move with how much map is on screen.
class MapRenderer {
 public:
  static void render(IMapCanvas& canvas, IMapSource& source, const MapViewState& state);

  // The plain triangle marker, kept for callers with no mode concept of
  // their own -- test/map_preview draws it explicitly since it has no
  // hike/cycle/ride distinction to render differently. MapActivity does not
  // call this; see MapActivity::drawPositionMarker().
  static void drawMarker(IMapCanvas& canvas, int16_t x, int16_t y, MapHeading heading);
};
