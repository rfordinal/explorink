#pragma once

#include <cstdint>

#include "IMapCanvas.h"
#include "IMapSource.h"
#include "MapHeading.h"
#include "MapStyle.h"

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
  // How many times render() walks the road layer: black strokes for every
  // road, then white fills inside the cased ones. Published because a caller
  // counting ways off IMapSource's cumulative counters needs it to get back to
  // "ways in the picture" (MapTileSource::waysEmitted).
  static constexpr int kRoadPasses = 2;

  // `style` carries every length drawn here (MapStyle.h). It is a parameter
  // rather than a compiled-in constant so the laptop preview and the device
  // can be pointed at the same numbers and be checked against each other --
  // both pass kDefaultMapStyle (MapStyleDefaults.h) in normal use.
  static void render(IMapCanvas& canvas, IMapSource& source, const MapViewState& state, const MapStyle& style);

  // The style's position puck: white disc, black ring, heading arrow. Drawn
  // by callers with no travel mode of their own -- test/map_preview, which has
  // no hike/cycle/ride distinction to render differently. MapActivity does not
  // call this; see MapActivity::drawPositionMarker(), which draws a
  // mode-specific marker instead.
  //
  // Also the call that puts the marker back after the missing-tile hatch: the
  // set of missing tiles is only known once the source has walked them, so the
  // hatch lands after render() and can cover the marker.
  static void drawMarker(IMapCanvas& canvas, int16_t x, int16_t y, MapHeading heading, const MapStyle& style);
};
