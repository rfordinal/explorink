#pragma once

#include <cstdint>

#include "MapAreaClass.h"
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
  // A tone and a hatch are alternatives, not a stack: the style picks one by
  // its `fill`. A tone is what a built-up area wants (MapAreaTone.h); the hatch
  // is kept because a big single area -- a lake, a forest -- can carry lines.
  MapAreaTone buildingTone;
  MapAreaFill::Pattern buildingHatch;
  uint8_t buildingHatchSpacingPx;

  // layers.water. Lines (waterways) and areas (lakes) come from the same layer;
  // an area is a closed ring (IMapSource.h).
  //
  // Width per water class (MapWaterClass: unknown, river, stream, lake), so a
  // river can be wider than a ditch. That became possible on 2026-08-05, when
  // the builder started writing a water class byte; before that every water
  // record was class 0 and mapstyle.json's river/stream rules could not reach
  // the device at all. 0 means that class is not drawn.
  //
  // `waterEnabled` gates the read, not the draw -- see buildings above.
  bool waterEnabled;
  uint8_t waterLinePx[kWaterClassSlots];
  MapAreaTone waterTone;
  MapAreaFill::Pattern waterHatch;
  uint8_t waterHatchSpacingPx;

  // layers.landuse. Forest and built-up areas share one tile layer and are
  // drawn at different depths -- built-up under everything, forest above it --
  // so the renderer walks the layer once per class (docs/map-data-spec.md,
  // "A tile is a storage unit, not a render unit").
  //
  // Indexed by MapLanduseClass. Class 0 is unused: the builder only writes a
  // record whose tags mapped to a real class.
  bool landuseEnabled;
  uint8_t landuseOutlinePx[kLanduseClassSlots];
  MapAreaTone landuseTone[kLanduseClassSlots];
  MapAreaFill::Pattern landuseHatch[kLanduseClassSlots];
  uint8_t landuseHatchSpacingPx[kLanduseClassSlots];

  // Village/town dot, layers.places.dot_radius_px doubled. 0 means places are
  // not drawn.
  uint8_t placeDotDiameterPx;

  // layers.route. The route is distinguished from the roads by width alone --
  // there is no colour on 1-bit e-ink -- so this is deliberately wider than any
  // road class. 0 means the route is not drawn even when one is loaded.
  //
  // The arrow is the filled head at the far end (docs/map-render-spec.md item
  // 3): without it the line does not say which way round the route runs.
  // `routeArrowLenPx` is tip-to-base, `routeArrowWidthPx` is the base. Both are
  // device pixels and do not scale with mpp: a screen decoration has to stay
  // legible at every zoom rung. Either at 0 draws the line and no head.
  uint8_t routeWidthPx;
  uint8_t routeArrowLenPx;
  uint8_t routeArrowWidthPx;

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

// The widest stroke this style can draw around a way's own geometry, in device
// pixels.
//
// This is the margin an off-screen test has to allow: a way whose points all
// sit just off the panel can still put ink on it, because a stroke is drawn
// centred on the line. MapTileSource uses it to decide what cannot possibly be
// visible (Config::rejectMarginPx).
//
// The **full** width, not half of it: half is what a centred stroke actually
// extends, and doubling that is one cheap pixel of paranoia against a rounding
// step or an off-centre stack (MapStroke::stackFor's `first` biases odd counts
// by half a pixel).
//
// Area fills are not in here on purpose. A tone or a hatch is clipped to the
// ring that carries it (MapAreaFill), so it never reaches past the geometry;
// only outlines and line strokes do.
inline uint8_t mapStyleMaxStrokePx(const MapStyle& style) {
  uint8_t widest = 0;
  const auto take = [&widest](uint8_t candidate) {
    if (candidate > widest) widest = candidate;
  };
  for (uint8_t i = 0; i < kClassEnumSlots; ++i) take(style.roadWidthPx[i]);
  for (uint8_t i = 0; i < kWaterClassSlots; ++i) take(style.waterLinePx[i]);
  for (uint8_t i = 0; i < kLanduseClassSlots; ++i) take(style.landuseOutlinePx[i]);
  take(style.buildingOutlinePx);
  take(style.routeWidthPx);
  return widest;
}
