#pragma once

#include <cstdint>

#include "IMapCanvas.h"
#include "MapFixTrust.h"
#include "MapMarkerMetrics.h"
#include "MapRideMode.h"

// The device's REAL position marker (ring + Hike's dot/hand or Cycle/Ride's
// heading arrow) -- for the laptop preview only.
//
// MapActivity::drawPositionMarker() (src/activities/map/MapActivity.cpp)
// draws this straight through GfxRenderer, not IMapCanvas, so it cannot be
// called from here without pulling in the whole activity (HAL, settings,
// i18n). This is a deliberate second copy of that geometry over IMapCanvas
// instead -- same reasoning as MapActivity.cpp's own kMarkerHeadingDir table,
// which is already a second copy of MapRenderer.cpp's kHeadingDir for the
// same "different drawing surface" reason. Keep the two in step: a change to
// what MapActivity::drawPositionMarker() draws belongs here too, or this pane
// stops being what docs/device-preview.md promises it is.
//
// Until 2026-09-19 this preview drew MapRenderer::drawMarker() instead -- a
// generic style puck, the same at every rung and every mode, and NOT what the
// device draws (MapStyle.h's puckRadiusPx/puckRingPx/puckArrowPx are dead on
// the device; only test/map_preview and marker_stamp ever read them). That
// meant a style tuned against this pane's marker had nothing to do with what
// a rider actually sees -- most visibly for Hike, whose dot the puck cannot
// even represent. See docs/device-preview.md, "The position marker".
namespace PreviewMarker {

void draw(IMapCanvas& canvas, int cx, int cy, uint8_t headingStep, MapRideMode mode,
          const MapFixTrust::MarkerStyle& style, const MarkerMetrics& m);

}  // namespace PreviewMarker
