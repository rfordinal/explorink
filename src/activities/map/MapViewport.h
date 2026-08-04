#pragma once

#include <cstdint>

#include "MapProjection.h"
#include "MapRideMode.h"

// The viewport arithmetic that turns "a coordinate, a heading and a zoom
// step" into "these tiles" -- docs/map-data-spec.md, "Zoom is a hardware
// button, so zoom is a ladder" and "Which tiles to load".
//
// Shared by MapActivity (device) and test/map_preview (laptop) on purpose.
// Two copies of this arithmetic would drift, and the drift would show up as
// "the golden test renders a different tile set than the device does",
// which is the one thing the golden test exists to rule out.
namespace MapViewport {

struct ZoomStep {
  double mpp;  // ground metres per pixel
  uint8_t z;   // tile zoom of the LOD this step reads
};

inline constexpr int kZoomStepCount = kMapLadderStepCount;

// Five rungs, 1 to 20 m/px. A step maps to exactly one LOD, so no zoom
// value can sit on an LOD boundary and thrash SD reads.
inline constexpr ZoomStep kZoomLadder[kZoomStepCount] = {
    {1.0, 13},   // step 0, detail
    {3.0, 13},   // step 1, detail
    {6.0, 12},   // step 2, regional
    {12.0, 11},  // step 3, overview
    {20.0, 11},  // step 4, overview
};

// Label overhang, added to the geometry bbox before it is mapped to tiles.
inline constexpr double kMarginPx = 64.0;

// Worst case is 3x3. A computed count above this is a bug, not a state.
inline constexpr uint32_t kMaxTiles = 9;

// style.device.marker_x_px / marker_y_px. The viewport re-anchors on the
// marker at this screen pixel on every reset, so the requested fix is both
// the anchor and the marker's own position.
//
// kAnchorScreenY is the style file's value and the default the native
// preview and the golden test render at. On the device the vertical anchor
// comes off the marker-height ladder below instead, which the buttons drive.
inline constexpr int16_t kAnchorScreenX = 230;
inline constexpr int16_t kAnchorScreenY = 620;

inline constexpr int kMarkerStepCount = kMapLadderStepCount;

// Marker-height ladder -- docs/architecture-plan.md, "Marker height is on the
// bottom buttons". Step 0 puts the marker at mid-screen (most road behind);
// step 4 puts it nearly on the bottom edge (most road ahead).
//
// **Read this as a look-ahead slider, not a marker position.** Right
// increases look-ahead, which moves the marker *down* the screen. A physical
// left/right button driving a vertical quantity reads backwards otherwise.
//
// A ladder for the same reason zoom is one: each step is a viewport reset and
// a full-screen refresh, so a few coarse rungs beat a smooth slider. Every
// rung is on screen -- a step that hides the puck is not a usable state, even
// though the style file is allowed to place the marker off-panel for tuning.
inline constexpr int16_t kMarkerLadder[kMarkerStepCount] = {400, 500, 600, 690, 760};

static_assert(kZoomStepCount == kMapLadderStepCount, "zoom ladder length is the shared rung count");
static_assert(kMarkerStepCount == kMapLadderStepCount, "marker ladder length is the shared rung count");

// The ladder value for a step, clamped. A step is a persisted byte, so a
// settings file edited by hand must not index off the end of the ladder.
inline int16_t markerYForStep(int step) {
  if (step < 0) return kMarkerLadder[0];
  if (step >= kMarkerStepCount) return kMarkerLadder[kMarkerStepCount - 1];
  return kMarkerLadder[step];
}

struct TileRange {
  uint8_t z = 0;
  uint32_t col0 = 0;
  uint32_t row0 = 0;
  uint32_t col1 = 0;
  uint32_t row1 = 0;

  uint32_t count() const { return (col1 - col0 + 1) * (row1 - row0 + 1); }
  uint32_t rowSpan() const { return row1 - row0 + 1; }
  // Same column-major index MapTileSource walks its range in, so an index
  // reported by the source maps back to a tile without a second convention.
  uint32_t colAt(uint32_t index) const { return col0 + index / rowSpan(); }
  uint32_t rowAt(uint32_t index) const { return row0 + index % rowSpan(); }
};

// mpp / cos(anchorLat) -- the single cosine paid per viewport reset. Mercator
// metres are not ground metres; at 48.5N one Mercator metre is 0.66 ground
// metres, and this is where that is paid for.
double mppMercFor(int zoomStep, double anchorLat);

// Rotate the screen rect by heading (already baked into `proj`), take its
// axis-aligned Mercator bbox, inflate by kMarginPx, map to the tile grid.
TileRange tileRangeFor(const MapProjection& proj, uint8_t z, int screenWidth, int screenHeight);

}  // namespace MapViewport
