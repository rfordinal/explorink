#include "MapLabels.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// The occupancy grid
// ---------------------------------------------------------------------------

namespace {

// Cell range a pixel span covers, clamped to the grid. Returns false when the
// span is wholly outside it.
bool cellRange(const int start, const int length, const int cellCount, int& outLo, int& outHi) {
  if (length <= 0) return false;
  const int last = start + length - 1;
  if (last < 0 || start >= cellCount * MapOccupancyGrid::kCellPx) return false;
  outLo = std::max(0, start / MapOccupancyGrid::kCellPx);
  outHi = std::min(cellCount - 1, last / MapOccupancyGrid::kCellPx);
  return outLo <= outHi;
}

}  // namespace

void MapOccupancyGrid::clear() { std::memset(bits_, 0, sizeof(bits_)); }

void MapOccupancyGrid::markRect(const int x, const int y, const int width, const int height) {
  int colLo = 0, colHi = 0, rowLo = 0, rowHi = 0;
  if (!cellRange(x, width, kCols, colLo, colHi)) return;
  if (!cellRange(y, height, kRows, rowLo, rowHi)) return;
  for (int row = rowLo; row <= rowHi; ++row) {
    for (int col = colLo; col <= colHi; ++col) setCell(col, row);
  }
}

bool MapOccupancyGrid::anySet(const int x, const int y, const int width, const int height) const {
  int colLo = 0, colHi = 0, rowLo = 0, rowHi = 0;
  if (!cellRange(x, width, kCols, colLo, colHi)) return false;
  if (!cellRange(y, height, kRows, rowLo, rowHi)) return false;
  for (int row = rowLo; row <= rowHi; ++row) {
    for (int col = colLo; col <= colHi; ++col) {
      if (cell(col, row)) return true;
    }
  }
  return false;
}

void MapOccupancyGrid::coverage(const int x, const int y, const int width, const int height, int& outSet,
                               int& outTotal) const {
  outSet = 0;
  outTotal = 0;
  int colLo = 0, colHi = 0, rowLo = 0, rowHi = 0;
  if (!cellRange(x, width, kCols, colLo, colHi)) return;
  if (!cellRange(y, height, kRows, rowLo, rowHi)) return;
  for (int row = rowLo; row <= rowHi; ++row) {
    for (int col = colLo; col <= colHi; ++col) {
      ++outTotal;
      if (cell(col, row)) ++outSet;
    }
  }
}

void MapOccupancyGrid::markSegment(const int32_t x1, const int32_t y1, const int32_t x2, const int32_t y2,
                                   const int halfWidth) {
  // Clip first, in doubles: a route point can be 2e8 px off screen
  // (IMapRouteSource.h), and stepping along the un-clipped segment would walk
  // hundreds of millions of samples to mark nothing.
  const double lo = -static_cast<double>(halfWidth);
  const double hiX = static_cast<double>(kCols * kCellPx) + halfWidth;
  const double hiY = static_cast<double>(kRows * kCellPx) + halfWidth;

  double ax = static_cast<double>(x1), ay = static_cast<double>(y1);
  const double dx = static_cast<double>(x2) - ax;
  const double dy = static_cast<double>(y2) - ay;

  // Liang-Barsky: shrink the parameter interval [t0, t1] against the four
  // edges. A degenerate (zero-length) segment falls through to the point case.
  double t0 = 0.0, t1 = 1.0;
  const double deltas[4] = {-dx, dx, -dy, dy};
  const double distances[4] = {ax - lo, hiX - ax, ay - lo, hiY - ay};
  for (int edge = 0; edge < 4; ++edge) {
    const double p = deltas[edge];
    const double q = distances[edge];
    if (p == 0.0) {
      if (q < 0.0) return;  // parallel to this edge and outside it
      continue;
    }
    const double r = q / p;
    if (p < 0.0) {
      if (r > t1) return;
      if (r > t0) t0 = r;
    } else {
      if (r < t0) return;
      if (r < t1) t1 = r;
    }
  }

  const double startX = ax + dx * t0;
  const double startY = ay + dy * t0;
  const double endX = ax + dx * t1;
  const double endY = ay + dy * t1;
  const double spanX = endX - startX;
  const double spanY = endY - startY;
  const double length = std::sqrt(spanX * spanX + spanY * spanY);

  // Half a cell per step, so no cell along the line can be stepped over.
  const double step = kCellPx / 2.0;
  const int samples = static_cast<int>(length / step) + 1;
  const int side = 2 * halfWidth + 1;
  for (int i = 0; i <= samples; ++i) {
    const double t = samples == 0 ? 0.0 : static_cast<double>(i) / samples;
    const int px = static_cast<int>(std::lround(startX + spanX * t));
    const int py = static_cast<int>(std::lround(startY + spanY * t));
    markRect(px - halfWidth, py - halfWidth, side, side);
  }
}

// ---------------------------------------------------------------------------
// Candidate collection
// ---------------------------------------------------------------------------

namespace {

// Rank <= 1 is city/town, everything above is village/suburb/hamlet/farm
// (mapbuilder/tilegen/build_config.json's place_ranks). Same split
// MapRenderer::render() uses for MapNearestPlaces, and the same split the two
// label tiers key off.
constexpr uint8_t kMajorMaxRank = 1;

bool betterCandidate(const MapLabelCandidate& a, const MapLabelCandidate& b) {
  if (a.rank != b.rank) return a.rank < b.rank;
  return a.distSq < b.distSq;
}

// Half the ring of offsets a halo pass uses: eight directions at radius r.
// A full square dilation would be (2r+1)^2 - 1 passes over the glyphs, which
// at r=2 is 24 re-draws of every label -- eight is what a stroke of this
// weight needs to close up. Measured cost is in docs/place-labels.md.
struct HaloOffset {
  int dx;
  int dy;
};
constexpr HaloOffset kHaloRing[8] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};

struct Box {
  int x;
  int y;
  int w;
  int h;
};

Box inflate(const Box& box, const int by) { return Box{box.x - by, box.y - by, box.w + 2 * by, box.h + 2 * by}; }

bool contains(const Box& outer, const Box& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

// Fits `name` into `maxWidthPx`, truncating on a UTF-8 boundary and appending an
// ellipsis when it does not fit. Returns false when even one character plus the
// ellipsis is too wide, or when this canvas has no text at all.
bool fitName(IMapCanvas& canvas, const char* name, const int sizePx, const bool bold, const int maxWidthPx, char* out,
             const size_t outLen, int& outWidth, int& outHeight) {
  std::snprintf(out, outLen, "%s", name);
  if (!canvas.measureText(out, sizePx, bold, outWidth, outHeight)) return false;
  if (outWidth <= 0 || outHeight <= 0) return false;
  if (maxWidthPx <= 0 || outWidth <= maxWidthPx) return true;

  // U+2026, which the built-in Noto Sans faces carry (lib/EpdFont/builtinFonts,
  // interval table entry 0x2026) -- so this cannot silently draw a missing
  // glyph box.
  static constexpr char kEllipsis[] = "\xE2\x80\xA6";
  size_t length = std::strlen(name);
  while (length > 0) {
    // Step back off a continuation byte so a multi-byte character is never cut
    // in half -- a half character is a corrupt name, not a shorter one.
    --length;
    while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80) --length;
    if (length == 0) break;
    std::snprintf(out, outLen, "%.*s%s", static_cast<int>(length), name, kEllipsis);
    if (!canvas.measureText(out, sizePx, bold, outWidth, outHeight)) return false;
    if (outWidth <= maxWidthPx) return true;
  }
  return false;
}

}  // namespace

void MapLabels::offer(MapLabelScratch& scratch, const MapPlaceRef& place, const int anchorX, const int anchorY) {
  if (place.name == nullptr || place.name[0] == '\0') return;

  MapLabelCandidate incoming;
  incoming.x = place.x;
  incoming.y = place.y;
  incoming.rank = place.rank;
  const long dx = static_cast<long>(place.x) - anchorX;
  const long dy = static_cast<long>(place.y) - anchorY;
  const long distSq = dx * dx + dy * dy;
  incoming.distSq = distSq > 0xFFFFFFFF ? 0xFFFFFFFFu : static_cast<uint32_t>(distSq);
  std::snprintf(incoming.name, MapLabelCandidate::kNameLen, "%s", place.name);

  if (scratch.count == MapLabelScratch::kMaxCandidates &&
      !betterCandidate(incoming, scratch.candidates[scratch.count - 1])) {
    return;
  }
  // Insertion sort into a twelve-entry array. Sorted on the way in rather than
  // at the end, because placement walks it in exactly this order and nothing
  // else ever reads it.
  int slot = scratch.count < MapLabelScratch::kMaxCandidates ? scratch.count : MapLabelScratch::kMaxCandidates - 1;
  while (slot > 0 && betterCandidate(incoming, scratch.candidates[slot - 1])) {
    scratch.candidates[slot] = scratch.candidates[slot - 1];
    --slot;
  }
  scratch.candidates[slot] = incoming;
  if (scratch.count < MapLabelScratch::kMaxCandidates) ++scratch.count;
}

void MapLabels::draw(IMapCanvas& canvas, MapLabelScratch& scratch, const MapStyle& style,
                     const uint8_t rungMaxLabels) {
  const uint8_t maxLabels = style.placeMaxLabels < rungMaxLabels ? style.placeMaxLabels : rungMaxLabels;
  if (maxLabels == 0) return;
  if (style.placeLabelPx == 0 && style.placeLabelMinorPx == 0) return;

  int drawableX = 0, drawableY = 0, drawableW = 0, drawableH = 0;
  canvas.drawableRect(drawableX, drawableY, drawableW, drawableH);
  const Box drawable{drawableX, drawableY, drawableW, drawableH};
  if (drawable.w <= 0 || drawable.h <= 0) return;

  // The puck is drawn after the map and sits at the anchor, so nothing was on
  // the canvas to mark it. Block it out by hand, at its style size plus the
  // ring: a name under the "you are here" marker is the one label guaranteed to
  // be unreadable.
  const int puckReach = std::max<int>(style.puckRadiusPx + style.puckRingPx, style.puckArrowPx / 2) + 2;
  scratch.taken.markRect(style.markerXPx - puckReach, style.markerYPx - puckReach, 2 * puckReach, 2 * puckReach);

  const int dotHalf = style.placeDotDiameterPx / 2;
  const int knockoutPad = style.placeLabelBg ? style.placeLabelBgPadPx + style.placeLabelBgBorderPx
                                             : static_cast<int>(style.placeLabelHaloPx);

  for (int i = 0; i < scratch.count; ++i) {
    if (scratch.placed >= maxLabels) break;
    const MapLabelCandidate& candidate = scratch.candidates[i];

    // A place whose dot is off screen is not a dropped label. The tile range is
    // deliberately wider than the viewport (MapTileSource), so most places in a
    // frame are outside it -- naming those is the off-screen chevrons' job
    // (docs/map-render-spec.md item 6), not this pass's. Counting them as
    // dropped would make the debug numbers read as if the declutter rules were
    // rejecting everything.
    if (candidate.x < drawable.x || candidate.y < drawable.y || candidate.x >= drawable.x + drawable.w ||
        candidate.y >= drawable.y + drawable.h) {
      continue;
    }

    const bool minor = candidate.rank > kMajorMaxRank;
    const int sizePx = minor ? style.placeLabelMinorPx : style.placeLabelPx;
    const bool bold = minor ? style.placeLabelMinorBold : style.placeLabelBold;
    if (sizePx == 0) continue;

    char text[MapLabelCandidate::kNameLen + 4];
    int textW = 0, textH = 0;
    if (!fitName(canvas, candidate.name, sizePx, bold, style.placeLabelMaxWidthPx, text, sizeof(text), textW, textH)) {
      ++scratch.dropped;
      continue;
    }

    // Eight positions, in preference order: right of the dot first (the
    // cartographic default for left-to-right text), then left, then below and
    // above, then the four diagonals. Whichever fits first wins; there is no
    // scoring, because a label that fits anywhere is already better than no
    // label at all. The diagonals matter more than they look -- measured on the
    // Zahorie route overview, four of six village names had no cardinal slot
    // free and the diagonals recovered them.
    //
    // The diagonal offset is the straight one scaled by ~0.7, so a diagonal
    // label sits the same distance from the dot as a cardinal one instead of
    // 1.4x further out.
    const int gap = style.placeLabelOffsetPx + dotHalf;
    const int diag = gap * 7 / 10;
    const Box placements[8] = {
        {candidate.x + gap, candidate.y - textH / 2, textW, textH},
        {candidate.x - gap - textW, candidate.y - textH / 2, textW, textH},
        {candidate.x - textW / 2, candidate.y + gap, textW, textH},
        {candidate.x - textW / 2, candidate.y - gap - textH, textW, textH},
        {candidate.x + diag, candidate.y - diag - textH, textW, textH},
        {candidate.x + diag, candidate.y + diag, textW, textH},
        {candidate.x - diag - textW, candidate.y - diag - textH, textW, textH},
        {candidate.x - diag - textW, candidate.y + diag, textW, textH},
    };

    bool drew = false;
    for (const Box& textBox : placements) {
      const Box knockout = inflate(textBox, knockoutPad);
      if (!contains(drawable, knockout)) continue;
      // Gap only against other labels: it is a spacing rule between names, not
      // a reason to refuse a name that reaches the edge of the screen.
      const Box spaced = inflate(knockout, style.placeLabelGapPx);
      if (scratch.taken.anySet(spaced.x, spaced.y, spaced.w, spaced.h)) continue;

      int routeCells = 0, totalCells = 0;
      scratch.route.coverage(knockout.x, knockout.y, knockout.w, knockout.h, routeCells, totalCells);
      if (totalCells > 0 && 100 * routeCells > style.placeLabelRouteOverlapPct * totalCells) continue;

      if (style.placeLabelBg) {
        if (style.placeLabelBgBorderPx > 0) {
          canvas.fillRoundedRect(knockout.x, knockout.y, knockout.w, knockout.h, 0, MapInk::Black);
          const Box inner = inflate(knockout, -static_cast<int>(style.placeLabelBgBorderPx));
          canvas.fillRoundedRect(inner.x, inner.y, inner.w, inner.h, 0, MapInk::White);
        } else {
          canvas.fillRoundedRect(knockout.x, knockout.y, knockout.w, knockout.h, 0, MapInk::White);
        }
      } else {
        // Halo: the same string drawn in white around itself, once per ring
        // radius, before the black pass lands on top. Only the pixels the
        // letters need are knocked out, so the map keeps showing through
        // between them -- which is the whole reason this is the default rather
        // than the box.
        for (int radius = 1; radius <= style.placeLabelHaloPx; ++radius) {
          for (const HaloOffset& offset : kHaloRing) {
            canvas.drawText(textBox.x + offset.dx * radius, textBox.y + offset.dy * radius, text, sizePx, bold,
                            MapInk::White);
          }
        }
      }
      canvas.drawText(textBox.x, textBox.y, text, sizePx, bold, MapInk::Black);
      scratch.taken.markRect(knockout.x, knockout.y, knockout.w, knockout.h);
      ++scratch.placed;
      drew = true;
      break;
    }
    if (!drew) ++scratch.dropped;
  }
}
