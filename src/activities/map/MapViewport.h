#pragma once

#include <cstdint>

#include "MapProjection.h"
#include "MapRideMode.h"
#include "MapStyleDefaults.h"

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
  // Whether this rung draws the buildings layer at all.
  //
  // A rung decision, not a style one: mapstyle.json says what a building looks
  // like, and this says where one is worth drawing. Set false and the layer is
  // never opened, so it costs no card read either
  // (MapRenderer, MapStyle::buildingsEnabled).
  //
  // Only rung 0 draws them, decided by the maintainer 2026-08-06. At 1 m/px a
  // building is 21 px across and is what the rider is looking at -- that rung
  // exists for "where exactly am I". At 3 m/px it is 7 px, and measured on
  // hardware the layer cost 4,122 ms of that rung's 7,463: 55 % of the slowest
  // rung on the ladder, for texture. Rungs 2-4 read tiles that carry no
  // buildings at all (mapbuilder/build_config.json), so false there only skips
  // an empty pass.
  //
  // This is the one place the device's drawing depends on the zoom rung.
  // docs/map-data-spec.md's "the device needs no zoom-dependent style" is
  // amended by it, and says so.
  bool buildings;
  // Whether this rung draws the built-up landuse wash.
  //
  // The mirror of `buildings`, and for the same reason read backwards: at rung 0
  // individual buildings are the thing being looked at, and a wash under them
  // would be a second grey adding nothing (docs/map-data-spec.md's original
  // argument for leaving built-up out of the detail LOD entirely). From rung 1
  // out there are no buildings, and the wash is what says "village" -- without
  // it that rung showed roads in empty white.
  //
  // The tile carries built-up at every LOD since 2026-08-06
  // (mapbuilder/build_config.json); this decides which rung draws it. Not a read
  // saving -- the landuse layer is read for forest regardless -- so it is a
  // drawing decision only, and landuse is a few KB.
  bool builtUp;
  // Marker size at this rung, in eighths of the full marker (8 = the 54 px ring
  // MapActivity has always drawn). A per-rung *drawing* decision, same kind as
  // the two above and in this table for the same reason: one row per rung, no
  // second table to keep in step.
  //
  // Why it varies at all: the marker is a fixed pixel object over ground that
  // shrinks under it. At 1 m/px the 54 px ring covers 54 m and is a marker; at
  // 45 m/px it covers 2.4 km, which is most of a valley -- it stops pointing at
  // a place and starts hiding one. The coarse rungs get a smaller one
  // (maintainer's call 2026-08-12, "pri z5 a z6 mi bude stačiť menší").
  //
  // It also pays for itself twice over: the marker's saved patch box scales
  // with it, so a move at rung 6 saves, restores and refreshes a quarter of the
  // pixels a move at rung 0 does.
  uint8_t markerScale8;
  // How far the marker must move, in screen pixels, before a partial refresh is
  // worth a waveform at this rung (MapFollow::Request::minMovePx).
  //
  // Also a per-rung number, and for the mirror of the reason above: a pixel is
  // worth more ground the further out the rung is. One fixed floor of 8 px was
  // 8 m at rung 0 and 360 m at rung 6 -- so the rider who could most use a
  // steady trickle of updates got the fewest. These numbers hold the ground
  // step roughly level (12 m, 30, 48, 96, 120, 96, 90) instead of the pixel
  // step, which is what a rider actually experiences.
  //
  // Bigger at the near rungs than the old 8 on purpose: at 1 m/px a refresh
  // every 8 m is a lot of waveforms for a marker crawling across the panel, and
  // the maintainer prefers a visibly bigger jump there ("v pripade z0 a z1 to
  // bude vizualne vacsi skok"). Unverified as a comfort call -- it is a number
  // to look at on a ride, not one measured.
  uint8_t minMovePx;
  // Most place names this rung may draw, capped again by the style's own
  // `max_labels` (docs/place-labels.md). A per-rung drawing decision, same kind
  // as the three above.
  //
  // Why it varies: a rung's cap should follow how much ground is on the panel,
  // not how many places happen to be in the tile range. Rung 0 shows 480 x 800 m
  // -- one settlement, and a dozen names there would be a dozen names for one
  // village. Rung 6 shows 24 x 40 km, where a dozen names is a map of the region
  // and the whole reason to be at that rung. Maintainer's call 2026-08-12:
  // "pri z0 urcite je zbytocne mat 12 labelov, pri z6 to uz moze mat zmysel".
  //
  // Unverified as a comfort call, like `minMovePx`: these are numbers to look at
  // on a ride, not measured ones. What *is* measured is the cost -- 94 ms for
  // five labels at rung 6 (docs/place-labels.md) -- so the cap is about
  // legibility, not about time.
  uint8_t maxLabels;
};

inline constexpr int kZoomStepCount = kMapZoomStepCount;

// Seven rungs, 1 to 45 m/px. A step maps to exactly one LOD, so no zoom
// value can sit on an LOD boundary and thrash SD reads.
//
// Steps 5 and 6 added 2026-08-12 for the regional view a long ride wants --
// docs/zoom-rungs.md and the parent repo's docs/coarse-zoom-plan.md. They read
// z11, the coarsest LOD that exists, past the point where z11 is the *natural*
// choice: at 45 m/px the screen spans more than two z11 tiles, which is why
// kMaxTiles below had to grow. The clean answer is a z10 LOD, which the tile
// format cannot address yet (int16 tile-local offsets, 39 km tile) -- these two
// rungs deliberately ship before that work, because the renders held up and the
// only number still missing is how long the reset takes on the panel.
inline constexpr ZoomStep kZoomLadder[kZoomStepCount] = {
    //  mpp   z  buildings  builtUp  marker/8  minMove  maxLabels
    {1.0, 13, true, false, 8, 12, 3},   // step 0, detail -- buildings, no wash under them
    {3.0, 13, false, true, 8, 10, 4},   // step 1, detail -- the wash instead of buildings
    {6.0, 12, false, true, 8, 8, 6},    // step 2, regional
    {12.0, 11, false, true, 8, 8, 8},   // step 3, overview
    {20.0, 11, false, true, 8, 6, 10},  // step 4, overview
    {32.0, 11, false, true, 6, 3, 12},  // step 5, overview -- z11 past its natural range
    {45.0, 11, false, true, 5, 2, 14},  // step 6, overview -- 24 x 40 km on the panel
};

// The ladder rung for a step, clamped -- same contract as markerYForStep(): a
// step arrives as a persisted byte or off a console command, and must not index
// past the table.
inline const ZoomStep& zoomStepAt(int step) {
  if (step < 0) return kZoomLadder[0];
  if (step >= kZoomStepCount) return kZoomLadder[kZoomStepCount - 1];
  return kZoomLadder[step];
}

// Label overhang, added to the geometry bbox before it is mapped to tiles.
inline constexpr double kMarginPx = 64.0;

// Worst case is 4x4, at rung 6. A computed count above this is a bug, not a
// state.
//
// It was 9 (3x3) while the ladder stopped at 20 m/px, which is inside what a
// z11 tile can serve. Rungs 5 and 6 read further than that: measured on the
// host renderer 2026-08-12 over Malé Karpaty, rung 5 loads 9 tiles and rung 6
// loads 12 unrotated. A rotated viewport at 45 m/px spans
// (933 + 2x64) px x 45 / cos(48.4 deg) = 80 km, which is 4.1 z11 tiles, so the
// range can be 4x4.
//
// This bounds a stack array of tile metadata, not tile data -- the renderer
// still reads one tile at a time (docs/map-memory.md). The host preview's RAM
// probe showed 6,736 B and zero heap allocations at every rung.
inline constexpr uint32_t kMaxTiles = 16;

// style.device.marker_x_px / marker_y_px, read from the compiled style
// (MapStyleDefaults.h) rather than repeated here -- editing the style file
// and rebuilding moves the anchor. The viewport re-anchors on the marker at
// this screen pixel on every reset, so the requested fix is both the anchor
// and the marker's own position.
//
// kAnchorScreenY is the style file's value and the default the native
// preview renders at. On the device the vertical anchor comes off the
// marker-height ladder below instead, which the buttons drive.
inline constexpr int16_t kAnchorScreenX = kDefaultMapStyle.markerXPx;

// How far one pan press moves the frame, as a percentage of the screen. 30 %
// rather than 50 %: half a screen jumps far enough that the rider loses the
// place they were looking at, and the cost per press (a tile read and a
// refresh) is the same at any step size (asked for on hardware 2026-08-17).
//
// Lives here rather than in MapActivity, where it started, because it is a
// viewport quantity like the anchor above and the ladders below -- and because
// MapActivity does not compile on the host, so a host tool that pans (the
// device window, test/map_window) could only have copied the number.
inline constexpr int kPanStepPercent = 30;
inline constexpr int16_t kAnchorScreenY = kDefaultMapStyle.markerYPx;

inline constexpr int kMarkerStepCount = kMapMarkerStepCount;

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

static_assert(kZoomStepCount == kMapZoomStepCount, "zoom ladder length is the zoom rung count");
static_assert(kMarkerStepCount == kMapMarkerStepCount, "marker ladder length is the marker rung count");

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
