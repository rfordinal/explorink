#pragma once

#include <cstdint>

#include "MapAreaFill.h"
#include "MapClassEnum.h"

// The style numbers MapRenderer draws with. Every length is in device pixels
// against the 480x800 screen, exactly as in data/mapstyle.json (see
// docs/mapstyle.json.md).
//
// data/mapstyle.json is the source; MapStyleDefaults.h holds the one compiled
// instance, generated from it by scripts/gen_mapstyle.py at build time. The
// device reads no style file at runtime -- same arrangement as
// MapModeMaskDefaults.h.
//
// A struct rather than free constants, because the renderer is also linked
// into the laptop-side preview (test/map_preview). One argument makes "which
// style produced this pixel" answerable there; free constants would only be
// answerable by rebuilding and hoping.
struct MapStyle {
  // Road line width per class_id -- index with MapClassId, whose slot count
  // this array matches. 0 means the class is not drawn, which is how
  // mapstyle.json's `hidden: true` and a disabled roads layer arrive here.
  //
  // Widths are whole pixels: IMapCanvas::drawLine takes an int line width, so
  // the style's 0.83px unclassified road is 1px on the panel. The generator
  // rounds, and never rounds a visible class down to 0.
  uint8_t roadWidthPx[kClassEnumSlots];

  // Casing thickness per class, layers.roads.rules[].casing_px. Above 0 the
  // road is drawn as a black stroke at the full width with a white stroke
  // `2 * casing` narrower inside it, i.e. two black edges with the road left
  // white between them -- how a main road reads as bigger than a side street
  // with no colour to spend (docs/map-render-spec.md, "What must be drawn").
  // 0 means a solid black line of the full width.
  uint8_t roadCasingPx[kClassEnumSlots];

  // layers.buildings. A ring is drawn as an optional outline plus a hatch --
  // never a solid fill, which on 1-bit swallows the roads around it
  // (docs/map-render-spec.md).
  //
  // `buildingsEnabled` false means the layer is never even read. That is a
  // performance decision, not a cosmetic one: buildings were 277 KB of the
  // 364 KB a four-tile viewport read (docs/map-data-spec.md, "RAM budget"), so
  // "read it and skip drawing" would pay the whole SD cost for nothing.
  bool buildingsEnabled;
  uint8_t buildingOutlinePx;
  MapAreaFill::Pattern buildingHatch;
  uint8_t buildingHatchSpacingPx;

  // layers.water. Lines (waterways) and areas (lakes) come from the same layer;
  // an area is a closed ring (IMapSource.h).
  //
  // One width for every waterway, because the tile format carries no water
  // class -- mapstyle.json's river/stream/canal rules cannot reach the device
  // and the layer default is what applies. Same reason `waterEnabled` gates the
  // read rather than the draw.
  bool waterEnabled;
  uint8_t waterLinePx;
  MapAreaFill::Pattern waterHatch;
  uint8_t waterHatchSpacingPx;

  // Village/town dot, layers.places.dot_radius_px doubled. 0 means places are
  // not drawn.
  uint8_t placeDotDiameterPx;

  // device.marker_x_px / marker_y_px -- the viewport anchor, which is also
  // where the marker itself sits (MapViewport::kAnchorScreenX/Y).
  int16_t markerXPx;
  int16_t markerYPx;

  // Position puck, layers.position: a white disc of this radius under a black
  // ring of `puckRingPx`, with a black heading arrow `puckArrowPx` long inside
  // it. Radius 0 draws the bare arrow and no disc -- a bare triangle vanishes
  // into a thick route, so that is a debug shape rather than the spec's.
  uint8_t puckRadiusPx;
  uint8_t puckRingPx;
  uint8_t puckArrowPx;
};
