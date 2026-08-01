#pragma once

#include "MapRenderer.h"

// Hardcoded/mock view shared by MapActivity (real firmware) and
// test/map_preview (native preview) -- single source of truth, so the
// preview always shows exactly what MapActivity will draw. Delete this once
// real base map + route loading (mapbuilder format) and the BLE position
// feed land -- see docs/firmware-implementation-plan.md.
//
// Proportional to screen size so it stays correct across all 4
// orientations (never assume 480x800).
inline MapViewState buildMockMapViewState(int screenWidth, int screenHeight) {
  MapViewState state;
  const auto x = [screenWidth](float frac) { return static_cast<int16_t>(screenWidth * frac); };
  const auto y = [screenHeight](float frac) { return static_cast<int16_t>(screenHeight * frac); };

  state.roadPolyline = {
      {x(0.50f), y(0.98f)}, {x(0.48f), y(0.75f)}, {x(0.54f), y(0.57f)},
      {x(0.42f), y(0.40f)}, {x(0.38f), y(0.20f)}, {x(0.46f), y(0.05f)},
  };
  state.villageDots = {{x(0.48f), y(0.75f)}, {x(0.38f), y(0.20f)}};

  // Marker anchored near the bottom edge, per the viewport model in
  // docs/architecture-plan.md: track-up rotation always points travel
  // direction up, so keeping the marker low maximizes look-ahead room.
  state.markerX = x(0.49f);
  state.markerY = y(0.88f);
  state.heading = MapHeading::N;
  return state;
}
