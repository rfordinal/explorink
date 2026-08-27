#pragma once

#include <cstdint>

#include "IMapCanvas.h"
#include "IMapPointSource.h"
#include "IMapRouteSource.h"
#include "IMapSource.h"
#include "MapHeading.h"
#include "MapLabels.h"
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
  // Which rung of the zoom ladder this frame is drawn at.
  //
  // It is here so the caller can pick the style for it -- mapStyleFor(mode,
  // zoomStep) in MapStyleTable.h. render() itself never reads it: it is handed
  // the resolved style and cannot tell which rung produced it, which is what
  // keeps "the style says what is drawn" true with no second rung table beside
  // it.
  //
  // Three fields used to live here instead -- drawBuildings, drawBuiltUp and
  // maxLabels, each copied out of MapViewport::ZoomStep. They are now
  // `when` blocks in data/mapstyle.json (layers.buildings.enabled, the built-up
  // landuse rule, layers.places.max_labels), so the rung's drawing decisions and
  // the style's are one table that is edited in one place.
  uint8_t zoomStep = 0;
};

// Wall time each layer of one render() call spent, in milliseconds.
//
// The caller supplies the clock. MapRenderer is compiled for the host as well
// as the device (test/map_preview links the same .cpp), and the host build has
// no Arduino `millis()`; a plain function pointer keeps that dependency at the
// call site instead of in here. A null `nowMs` -- or a null MapRenderTiming --
// means no timing is taken and nothing is called.
//
// Why per layer and not one number for the frame: the frame time is already on
// the debug readout (`MapActivity.cpp`, `%lums`), and it cannot answer the only
// question worth asking, which is *which layer*. See
// docs/optimization/01-render-pipeline.md, step 1.
struct MapRenderTiming {
  uint32_t (*nowMs)() = nullptr;
  uint32_t landuseMs = 0;
  uint32_t contoursMs = 0;
  uint32_t buildingsMs = 0;
  uint32_t waterMs = 0;
  uint32_t roadsMs = 0;
  uint32_t routeMs = 0;
  uint32_t placesMs = 0;
  // The POI marks from the point layer. Its own field because it is its own
  // files: a shard is a separate open per 39 km of ground, so a slow points
  // pass is a card-access question and never a geometry one.
  uint32_t pointsMs = 0;
  // Place-name layout and drawing, which is neither free nor part of the places
  // walk: the halo is the same string re-drawn eight times per ring radius
  // (MapLabels.cpp), so this is the field that says whether the halo is
  // affordable at a given size.
  uint32_t labelsMs = 0;

  // Two counts rather than times, and they ride in the timing struct because
  // that is the one thing the renderer already fills and MapActivity already
  // logs. Without them a run has no way to say whether the relief layer drew
  // anything: `contoursMs` is 0 both when the layer is off and when it is on and
  // empty, and an e2e check against a screenshot cannot tell a missing layer
  // from a flat one (docs/simulator.md, "End to end against real tiles").
  uint32_t contourLines = 0;
  uint32_t contourLabels = 0;
};

// Nearest named place to the marker, by screen-pixel distance, picked while
// render() is already walking the places layer for its dots -- no second
// pass, no second SD seek (IMapSource.h: "begin*() ... The second pass is a
// second seek", the exact cost this avoids paying for a lookup the marker
// dots' own walk already had the data for).
//
// Two slots, not one, because the tile format has no admin hierarchy -- a
// suburb carries no link to the city it is part of
// (mapbuilder/build_config.json's place_ranks is a flat rank, not a tree).
// `fine` is the nearest rank>=2 point (village/suburb/hamlet/farm), `coarse`
// the nearest rank<=1 (city/town). A caller wanting "Karlova Ves, Bratislava"
// shows both when both are set, falls back to whichever one is, and shows
// nothing when neither loaded tile carries a name near the marker.
struct MapNearestPlaces {
  static constexpr int kNameBufferLen = 40;
  char fineName[kNameBufferLen] = "";
  char coarseName[kNameBufferLen] = "";
  bool hasFine = false;
  bool hasCoarse = false;
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
  // `route` is the loaded trip, or nullptr when the rider skipped the picker.
  // Drawn between the road fills and the place dots, which is the draw order
  // docs/map-data-spec.md fixes -- the route has to sit over the roads it
  // follows, and the place dots over the route.
  //
  // A second source rather than a layer of the first, because a route is one
  // file that outlives any viewport while IMapSource is a range of tiles
  // (IMapRouteSource.h).
  //
  // `timing` is optional instrumentation and changes no pixel. Pass nullptr --
  // the default -- and the clock is never read.
  //
  // `nearestOut` is optional and also changes no pixel: it is filled in from
  // the same places walk that draws the dots, reset to empty first. Passing
  // it forces the places layer open even when `style.placeDotDiameterPx == 0`
  // (dots hidden) -- the header wants a name lookup regardless of whether the
  // style also draws a dot for it.
  //
  // `labels` is the place-name pass's working set, owned by the caller because
  // it is ~3.2 KB and this class holds no state (MapLabels.h). Pass nullptr and
  // the map gets place dots and no names -- the behaviour before labels
  // existed. Passing it forces the places layer open the same way `nearestOut`
  // does, since the names come off that walk.
  // `points` is the POI layer (safety points, later landmarks), or nullptr when
  // the rider has it off or no shard covers the viewport. Drawn after the place
  // dots and before the names: a square is a bigger mark than a dot and must
  // not be buried under one, and a name must still win over both, since a label
  // is the only thing here that is placed rather than simply drawn.
  static void render(IMapCanvas& canvas, IMapSource& source, const MapViewState& state, const MapStyle& style,
                     IMapRouteSource* route = nullptr, MapRenderTiming* timing = nullptr,
                     MapNearestPlaces* nearestOut = nullptr, MapLabelScratch* labels = nullptr,
                     IMapPointSource* points = nullptr);

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
