#pragma once

#include <cstdint>

#include "IMapCanvas.h"
#include "IMapSource.h"
#include "MapStyle.h"

// Place-name labels: which names get drawn, where, and what they are not
// allowed to cover. docs/place-labels.md has the reasoning; this file has the
// mechanism.
//
// Two things make this different from every other layer MapRenderer draws.
// First, a label has to be *decided* rather than just drawn: two names that
// overlap are worse than one name, so placement needs to know what is already
// on screen. Second, it needs the text width, which only the canvas knows
// (IMapCanvas::measureText).
//
// The knowledge of what is on screen is kept as a coarse occupancy grid rather
// than by re-reading any layer. Roads and buildings are deliberately NOT in it:
// a name is meant to sit over the map -- that is what the halo or the box is
// for -- and tracking road pixels would cost a second roads pass over the SD
// card for no decision. The route is in it, because the route is the one thing
// on screen a label must not hide.

// One bit per kCellPx square of screen. 8 px is about half a label's height, so
// a box test lands on 3-4 rows of cells: fine enough that a name beside the
// route is not rejected for a cell it barely touches, coarse enough that the
// whole grid is 1,250 bytes instead of a 48 KB shadow framebuffer.
//
// Sized for the longest screen edge in both axes (800 x 800 cells' worth) so
// the same grid serves any orientation without knowing which one is up.
// Everything outside the canvas's own drawable rect is simply never marked or
// tested -- MapLabels::draw() clamps to it first.
class MapOccupancyGrid {
 public:
  static constexpr int kCellPx = 8;
  static constexpr int kMaxEdgePx = 800;
  static constexpr int kCols = kMaxEdgePx / kCellPx;
  static constexpr int kRows = kMaxEdgePx / kCellPx;

  void clear();

  // Marks every cell the rectangle touches. Screen pixels, clipped to the grid.
  void markRect(int x, int y, int width, int height);

  // Marks the cells a thick line covers -- the route, `halfWidth` px either
  // side of its centre line. Coordinates are int32 because a route point can be
  // a long way off screen (IMapRouteSource.h); the segment is clipped to the
  // grid before anything is walked, so an off-screen tail costs a few compares
  // and no loop iterations.
  void markSegment(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int halfWidth);

  // True if any cell the rectangle touches is set.
  bool anySet(int x, int y, int width, int height) const;

  // Cells set inside the rectangle, and how many it touches in total. Returns
  // 0/0 for a rectangle wholly off the grid.
  void coverage(int x, int y, int width, int height, int& outSet, int& outTotal) const;

 private:
  bool cell(int col, int row) const { return (bits_[index(col, row) >> 3] >> (index(col, row) & 7)) & 1; }
  void setCell(int col, int row) { bits_[index(col, row) >> 3] |= static_cast<uint8_t>(1 << (index(col, row) & 7)); }
  static int index(int col, int row) { return row * kCols + col; }

  uint8_t bits_[(kCols * kRows + 7) / 8] = {};
};

// One place that wants a label. Copied out of the source's own buffer, because
// MapPlaceRef::name is only valid until the next nextPlace() call (IMapSource.h)
// and placement cannot start until the whole layer has been walked -- the
// rank-first order is the whole point.
struct MapLabelCandidate {
  static constexpr int kNameLen = 32;
  int16_t x = 0;
  int16_t y = 0;
  uint8_t rank = 0;
  uint32_t distSq = 0;
  char name[kNameLen] = "";
};

// The label pass's whole working set, owned by the caller.
//
// ~3.8 KB, which is why it is not a local: the render task's stack budget is
// 256 bytes of locals per the RAM rules, and MapRenderer holds no state of its
// own by design (MapRenderer.h). MapActivity allocates one in onEnter() when
// the style draws labels at all and frees it in onExit(); test/map_preview
// keeps one on the host stack. Passing nullptr to render() draws dots and no
// labels, which is exactly what the firmware did before this existed.
struct MapLabelScratch {
  // Candidates held before placement.
  //
  // Twelve was wrong, and wrong in a way the label count did not show: the set is
  // kept nearest-first, so on a wide rung -- 88 places in range, measured on the
  // panel at rung 6 -- the twelve nearest all sat around the marker and the top
  // half of the screen was never even considered for a name. The cap has to be
  // past the number of places that can share a screen, not past the number of
  // labels that fit, or it silently decides *where* labels may go.
  //
  // 32 costs 1,280 bytes of the budget above and covers every rung measured so
  // far. A frame with more on-screen places than this still names the nearest 32,
  // which is the right thing to lose.
  static constexpr int kMaxCandidates = 32;

  MapOccupancyGrid route;
  MapOccupancyGrid taken;
  MapLabelCandidate candidates[kMaxCandidates];
  uint8_t count = 0;
  // Filled in by draw(), for the debug readout and the preview's stderr line.
  uint8_t placed = 0;
  uint8_t dropped = 0;

  void reset() {
    route.clear();
    taken.clear();
    count = 0;
    placed = 0;
    dropped = 0;
  }
};

namespace MapLabels {

// Offers one place to the candidate set, keeping the best kMaxCandidates by
// (rank, then distance to the anchor). Cheap enough to call for every place in
// the viewport: no allocation, and the array is kMaxCandidates entries long.
//
// `anchorX/anchorY` is the viewport anchor (MapStyle::markerXPx/markerYPx), not
// the live marker. Label layout must not move when a GPS packet arrives, or the
// map would need a redraw per fix instead of per viewport reset
// (docs/map-render-spec.md, "the map underneath stays byte-identical").
void offer(MapLabelScratch& scratch, const MapPlaceRef& place, int anchorX, int anchorY);

// Places and draws what fits. Call after the route has been drawn (so
// scratch.route is marked) and after the place dots, since a label sits over
// the map but a dot must not be hidden by another place's label box.
//
// `rungMaxLabels` is the zoom rung's own ceiling
// (MapViewport::ZoomStep::maxLabels, carried in MapViewState); the smaller of it
// and the style's `max_labels` wins. Two caps rather than one because they
// answer different questions: the rung knows how much ground is on the panel and
// therefore how many names that ground deserves, the style knows how many the
// renderer may afford. Default 255 means "rung has no opinion".
void draw(IMapCanvas& canvas, MapLabelScratch& scratch, const MapStyle& style, uint8_t rungMaxLabels = 255);

}  // namespace MapLabels
