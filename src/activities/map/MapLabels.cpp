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

// The positions a label is tried in, around its dot. Four cardinal then four
// diagonal -- see the table built in draw().
constexpr int kPlacementCount = 8;

// Sampling step for the ink test, in screen pixels. Every second pixel in both
// axes is a quarter of the reads, and the test is a comparison between eight
// boxes of the same size, so the undercount is identical in all eight and
// cancels. 1 px would be exact and four times the cost for an answer that
// decides nothing differently; 4 px starts stepping over a 1 px hairline road,
// which is the very thing the test exists to keep uncovered.
constexpr int kInkStepPx = 2;

// How much more ink a cartographically preferred position may carry and still
// beat a cleaner one, in per mille of its own box. 2 % of a 110 x 28 box is
// about 60 pixels -- roughly one thin road crossing a corner.
//
// Not zero on purpose. With an exact minimum, two positions that differ by a
// single dithered pixel of woodland swap places, and a name that moves from the
// right of its dot to below it between two redraws of the same ground reads as
// a different map rather than a tidier one.
constexpr int kInkTolerancePermille = 20;

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
// A name laid out ready to draw: one line, or two when the whole name does not
// fit on one and a word boundary saves it.
struct FittedLabel {
  static constexpr int kMaxLines = 2;
  char line[kMaxLines][MapLabelCandidate::kNameLen + 4] = {};
  int width[kMaxLines] = {0, 0};
  int lines = 0;
  int lineHeight = 0;
  // Pixels held open between the two lines, so the mark itself sits in the
  // middle of the block: name above, symbol on the point, height below. The
  // layout a paper map gives a summit. 0 means the lines are adjacent, which
  // is what a wrapped name wants.
  int gapPx = 0;

  int height() const { return lineHeight * lines + gapPx; }
  int width_() const { return width[0] > width[1] ? width[0] : width[1]; }
};

// U+2026, which the built-in faces carry (lib/EpdFont/builtinFonts, interval
// table entry 0x2026) -- so this cannot silently draw a missing glyph box.
constexpr char kEllipsis[] = "\xE2\x80\xA6";

bool measureInto(IMapCanvas& canvas, FittedLabel& fitted, const int index, const int sizePx, const bool bold) {
  int height = 0;
  if (!canvas.measureText(fitted.line[index], sizePx, bold, fitted.width[index], height)) return false;
  if (fitted.width[index] <= 0 || height <= 0) return false;
  fitted.lineHeight = height;
  return true;
}

// Shortens with an ellipsis, cutting on a codepoint boundary so a multi-byte
// character is never halved -- a half character is a corrupt name, not a
// shorter one.
bool ellipsise(IMapCanvas& canvas, const char* name, const int sizePx, const bool bold, const int maxWidthPx,
               FittedLabel& fitted) {
  size_t length = std::strlen(name);
  while (length > 0) {
    --length;
    while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80) --length;
    if (length == 0) break;
    std::snprintf(fitted.line[0], sizeof(fitted.line[0]), "%.*s%s", static_cast<int>(length), name, kEllipsis);
    if (!measureInto(canvas, fitted, 0, sizePx, bold)) return false;
    if (fitted.width[0] <= maxWidthPx) {
      fitted.lines = 1;
      return true;
    }
  }
  return false;
}

// One line if the name fits, two if a word boundary makes it fit, an ellipsis
// if neither does.
//
// **Two lines are a placement change, not a drawing one.** The box is what the
// eight positions, the gap rule, the route test and the least-ink test all work
// on, so a wrapped name competes for twice the depth and loses more positions
// than a cut one would. That is the trade, and it is why the split is only
// taken when BOTH halves fit: a wrap that still needs an ellipsis costs the
// depth and saves nothing.
//
// Split on a space only. A hyphen looks like a candidate and is not: "Súľov-
// Hradná" broken there reads as two places, and a map has no room to explain
// that it is one.
bool fitName(IMapCanvas& canvas, const char* name, const int sizePx, const bool bold, const int maxWidthPx,
             FittedLabel& fitted) {
  fitted.lines = 0;
  std::snprintf(fitted.line[0], sizeof(fitted.line[0]), "%s", name);
  if (!measureInto(canvas, fitted, 0, sizePx, bold)) return false;
  if (maxWidthPx <= 0 || fitted.width[0] <= maxWidthPx) {
    fitted.lines = 1;
    return true;
  }

  // The split that balances the two lines best, so a name does not come out as
  // one long line over one short word. Measured per candidate rather than
  // guessed from character counts: the faces are proportional.
  const size_t length = std::strlen(name);
  int bestWider = -1;
  size_t bestSplit = 0;
  FittedLabel trial;
  for (size_t i = 1; i + 1 < length; ++i) {
    if (name[i] != ' ') continue;
    std::snprintf(trial.line[0], sizeof(trial.line[0]), "%.*s", static_cast<int>(i), name);
    std::snprintf(trial.line[1], sizeof(trial.line[1]), "%s", name + i + 1);
    if (!measureInto(canvas, trial, 0, sizePx, bold)) continue;
    if (!measureInto(canvas, trial, 1, sizePx, bold)) continue;
    if (trial.width[0] > maxWidthPx || trial.width[1] > maxWidthPx) continue;
    const int wider = trial.width[0] > trial.width[1] ? trial.width[0] : trial.width[1];
    if (bestWider < 0 || wider < bestWider) {
      bestWider = wider;
      bestSplit = i;
    }
  }
  if (bestWider >= 0) {
    std::snprintf(fitted.line[0], sizeof(fitted.line[0]), "%.*s", static_cast<int>(bestSplit), name);
    std::snprintf(fitted.line[1], sizeof(fitted.line[1]), "%s", name + bestSplit + 1);
    if (measureInto(canvas, fitted, 0, sizePx, bold) && measureInto(canvas, fitted, 1, sizePx, bold)) {
      fitted.lines = 2;
      return true;
    }
  }
  return ellipsise(canvas, name, sizePx, bold, maxWidthPx, fitted);
}

// Draws every line of a fitted label inside `box`, each line centred on the
// box's own width -- a two-line name reads as one block rather than as two
// left-aligned fragments.
void drawFitted(IMapCanvas& canvas, const FittedLabel& fitted, const int boxX, const int boxY, const int boxW,
                const int sizePx, const bool bold, const MapInk ink) {
  for (int i = 0; i < fitted.lines; ++i) {
    const int x = boxX + (boxW - fitted.width[i]) / 2;
    const int y = boxY + i * (fitted.lineHeight + (i > 0 ? fitted.gapPx : 0));
    canvas.drawText(x, y, fitted.line[i], sizePx, bold, ink);
  }
}

}  // namespace

namespace {

// Copies a name into a fixed buffer, cutting on a UTF-8 codepoint boundary.
//
// snprintf's "%s" cuts at a byte, so a name one byte too long could lose half
// of its last character and draw a replacement glyph mid-word. Same rule the
// writer already applies on the other side of the format
// (mapbuilder/tilegen/point_file.py, clamp_name).
void copyName(char* out, const int outLen, const char* name) {
  if (outLen <= 0) return;
  int n = 0;
  while (name[n] != '\0' && n < outLen - 1) ++n;
  // A continuation byte is 10xxxxxx. If the cut landed on one, walk back to
  // the lead byte and drop the whole sequence.
  if (name[n] != '\0') {
    while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
  }
  std::memcpy(out, name, static_cast<size_t>(n));
  out[n] = '\0';
}

}  // namespace

void MapLabels::offer(MapLabelScratch& scratch, const MapPlaceRef& place, const int anchorX, const int anchorY) {
  if (place.name == nullptr || place.name[0] == '\0') return;
  // A place whose dot is off the panel cannot be named -- draw() skips it -- so
  // letting it win a candidate slot only starves a place that could have been.
  // Same test draw() applies, moved to where it costs something.
  if (!scratch.onScreen(place.x, place.y)) return;

  MapLabelCandidate incoming;
  incoming.x = place.x;
  incoming.y = place.y;
  incoming.rank = place.rank;
  const long dx = static_cast<long>(place.x) - anchorX;
  const long dy = static_cast<long>(place.y) - anchorY;
  const long distSq = dx * dx + dy * dy;
  incoming.distSq = distSq > 0xFFFFFFFF ? 0xFFFFFFFFu : static_cast<uint32_t>(distSq);
  copyName(incoming.name, MapLabelCandidate::kNameLen, place.name);

  // The cell's own quota, checked before the global one. Without it the whole
  // buffer goes to whichever part of the panel happens to be densest, which
  // over Prague at rung 6 is the ring around the marker (MapLabels.h,
  // kPerCell). Inside a cell the order is unchanged: rank, then distance.
  const int cell = scratch.cellOf(place.x, place.y);
  if (cell >= 0) {
    int inCell = 0, worstInCell = -1;
    for (int i = 0; i < scratch.count; ++i) {
      if (scratch.cellOf(scratch.candidates[i].x, scratch.candidates[i].y) != cell) continue;
      ++inCell;
      worstInCell = i;  // the array is sorted, so the last match is the worst
    }
    if (inCell >= MapLabelScratch::kPerCell) {
      if (worstInCell < 0 || !betterCandidate(incoming, scratch.candidates[worstInCell])) return;
      // Drop the cell's worst so the incoming one can take its place, rather
      // than evicting the globally worst -- which would be in some other cell
      // and would undo the spread this cap exists to create.
      for (int i = worstInCell; i + 1 < scratch.count; ++i) scratch.candidates[i] = scratch.candidates[i + 1];
      --scratch.count;
    }
  }

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

namespace {
void markLandmark(MapLabelCandidate& candidate, const char* below, const int markHalfPx) {
  candidate.landmark = true;
  candidate.markHalfPx = static_cast<uint8_t>(markHalfPx);
  if (below != nullptr && below[0] != '\0') {
    std::snprintf(candidate.below, sizeof(candidate.below), "%s", below);
  } else {
    candidate.below[0] = '\0';
  }
}
}  // namespace

void MapLabels::offerLandmark(MapLabelScratch& scratch, const MapPointRef& point, const char* name, const char* below,
                              const int markHalfPx, const int anchorX, const int anchorY) {
  if (name == nullptr || name[0] == '\0') return;
  if (!scratch.onScreen(static_cast<int>(point.x), static_cast<int>(point.y))) return;

  MapPlaceRef asPlace;
  asPlace.x = static_cast<int16_t>(point.x);
  asPlace.y = static_cast<int16_t>(point.y);
  asPlace.rank = point.rank;
  asPlace.name = name;
  const uint8_t before = scratch.count;
  offer(scratch, asPlace, anchorX, anchorY);
  if (scratch.count == before) {
    // offer() either inserted or replaced; either way the new entry is the one
    // whose name matches and which is not yet marked. Finding it by position is
    // exact: two candidates cannot share a screen pixel and a name.
    for (int i = 0; i < scratch.count; ++i) {
      if (scratch.candidates[i].x == asPlace.x && scratch.candidates[i].y == asPlace.y) {
        markLandmark(scratch.candidates[i], below, markHalfPx);
        return;
      }
    }
    return;
  }
  for (int i = 0; i < scratch.count; ++i) {
    if (scratch.candidates[i].x == asPlace.x && scratch.candidates[i].y == asPlace.y) {
      markLandmark(scratch.candidates[i], below, markHalfPx);
      return;
    }
  }
}

void MapLabels::draw(IMapCanvas& canvas, MapLabelScratch& scratch, const MapStyle& style) {
  // Two budgets, never one. `placeMaxLabels` is the places' affordability
  // ceiling and `pointMaxLabels` is the landmarks'. They used to share a
  // single running total, so on a rung where places may draw four names the
  // landmarks got whatever was left of those four -- measured over Sulov
  // 2026-10-27: fourteen named landmarks on one panel, four labels drawn, and
  // the cap that stopped it belonged to the other layer (T-305).
  const uint8_t maxPlaceLabels = style.placeMaxLabels;
  const uint8_t maxPointLabels = style.pointMaxLabels;
  const bool drawsPlaceNames = maxPlaceLabels > 0 && (style.placeLabelPx != 0 || style.placeLabelMinorPx != 0);
  const bool drawsPointNames = maxPointLabels > 0 && style.pointLabelPx != 0;
  if (!drawsPlaceNames && !drawsPointNames) return;

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

  // Per-tier counters. Candidates are already sorted rank-first (sortKey
  // above), so the majors are offered before the minors and a major can never
  // be crowded out by a village that happened to fit first.
  int placedMajor = 0, placedMinor = 0, placedLandmark = 0;
  for (int i = 0; i < scratch.count; ++i) {
    // Stop only when BOTH budgets are spent. A landmark that is skipped
    // because the places are full would be a name lost to another layer's
    // ceiling.
    if (placedMajor + placedMinor >= maxPlaceLabels && placedLandmark >= maxPointLabels) break;
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

    const bool minor = !candidate.landmark && candidate.rank > kMajorMaxRank;
    // The tier's own cap. Tested before the name is measured and before a
    // position is searched for, so a tier that is full costs nothing per
    // candidate -- and a skipped candidate is not a *dropped* label: it was
    // never a candidate for this frame, the same reasoning as the off-screen
    // test above.
    if (candidate.landmark) {
      if (placedLandmark >= maxPointLabels) continue;
    } else if (placedMajor + placedMinor >= maxPlaceLabels ||
               (minor ? placedMinor >= style.placeMaxLabelsMinor : placedMajor >= style.placeMaxLabelsMajor)) {
      continue;
    }
    const int sizePx = candidate.landmark ? style.pointLabelPx : (minor ? style.placeLabelMinorPx : style.placeLabelPx);
    const bool bold =
        candidate.landmark ? style.pointLabelBold : (minor ? style.placeLabelMinorBold : style.placeLabelBold);
    if (sizePx == 0) continue;

    FittedLabel fitted;
    const int maxWidth = candidate.landmark ? style.pointLabelMaxWidthPx : style.placeLabelMaxWidthPx;
    if (!fitName(canvas, candidate.name, sizePx, bold, maxWidth, fitted)) {
      ++scratch.dropped;
      continue;
    }
    // A summit is set the way a paper map sets one: name above, the symbol on
    // the point, the height below. That needs the label block to STRADDLE the
    // mark rather than sit beside it, so the two lines are held apart by the
    // mark's own height.
    //
    // Only when the name fits on one line. A name that already wrapped has no
    // room for a third line, and the name is worth more than the number.
    bool straddle = false;
    if (candidate.below[0] != '\0' && fitted.lines == 1) {
      std::snprintf(fitted.line[1], sizeof(fitted.line[1]), "%s", candidate.below);
      if (measureInto(canvas, fitted, 1, sizePx, bold)) {
        fitted.lines = 2;
        fitted.gapPx = candidate.markHalfPx * 2 + 2;
        straddle = true;
      }
    }

    // The box a wrapped or straddling name competes for is deeper, and every
    // rule below -- the positions, the gap, the route overlap, the ink test --
    // works on this box.
    int textW = fitted.width_();
    int textH = fitted.height();

    // Eight positions, in preference order: right of the dot first (the
    // cartographic default for left-to-right text), then left, then below and
    // above, then the four diagonals. The diagonals matter more than they look
    // -- measured on the Zahorie route overview, four of six village names had
    // no cardinal slot free and the diagonals recovered them.
    //
    // The diagonal offset is the straight one scaled by ~0.7, so a diagonal
    // label sits the same distance from the dot as a cardinal one instead of
    // 1.4x further out.
    // The straddle is tried first and on its own: when it is legal it is the
    // right answer for a summit, so it does not compete on ink with eight
    // positions that put the name beside the mark instead of around it.
    const auto boxUsable = [&](const Box& box) {
      const Box knock = inflate(box, knockoutPad);
      if (!contains(drawable, knock)) return false;
      if (canvas.areaReserved(knock.x, knock.y, knock.w, knock.h)) return false;
      const Box spaced = inflate(knock, style.placeLabelGapPx);
      if (scratch.taken.anySet(spaced.x, spaced.y, spaced.w, spaced.h)) return false;
      int routeCells = 0, totalCells = 0;
      scratch.route.coverage(knock.x, knock.y, knock.w, knock.h, routeCells, totalCells);
      return !(totalCells > 0 && 100 * routeCells > style.placeLabelRouteOverlapPct * totalCells);
    };
    Box straddleBox{candidate.x - textW / 2, candidate.y - textH / 2, textW, textH};
    const bool straddlePlaced = straddle && boxUsable(straddleBox);
    if (straddle && !straddlePlaced) {
      // No room around the mark. Keep the name, drop the number: a stacked
      // name-over-number beside the mark would read as two labels.
      fitted.lines = 1;
      fitted.gapPx = 0;
      straddle = false;
      textW = fitted.width_();
      textH = fitted.height();
    }

    const int gap = style.placeLabelOffsetPx + (candidate.landmark ? candidate.markHalfPx : dotHalf);
    const int diag = gap * 7 / 10;
    const Box placements[kPlacementCount] = {
        {candidate.x + gap, candidate.y - textH / 2, textW, textH},
        {candidate.x - gap - textW, candidate.y - textH / 2, textW, textH},
        {candidate.x - textW / 2, candidate.y + gap, textW, textH},
        {candidate.x - textW / 2, candidate.y - gap - textH, textW, textH},
        {candidate.x + diag, candidate.y - diag - textH, textW, textH},
        {candidate.x + diag, candidate.y + diag, textW, textH},
        {candidate.x - diag - textW, candidate.y - diag - textH, textW, textH},
        {candidate.x - diag - textW, candidate.y + diag, textW, textH},
    };

    // Which of the eight to take is decided in two steps, and the order is the
    // point: the hard rules first, then least ink among whatever survived.
    //
    // Before 2026-09-15 the first position that passed the hard rules won
    // outright, and that is a coin toss about information: "right of the dot"
    // is as likely to land across a primary road as on empty field. The name is
    // drawn with a halo, so whatever it sits on is knocked out -- the position
    // choice is literally choosing which part of the map to erase.
    //
    // `ink` is per mille of the box's samples, so the eight are comparable even
    // though the edge-clamped ones take fewer samples.
    int ink[kPlacementCount] = {};
    bool usable[kPlacementCount] = {};
    int leastInk = 1001;
    for (int p = 0; p < kPlacementCount; ++p) {
      const Box& textBox = placements[p];
      const Box knockout = inflate(textBox, knockoutPad);
      if (!contains(drawable, knockout)) continue;
      // Screen furniture owns its pixels (MapChrome.h). Refusing the position
      // rather than the name is deliberate: there are eight of them, so a name
      // beside the scale bar usually just moves to its other side. Only a place
      // whose eight positions are all taken loses its label, and it is then
      // counted as dropped like any other.
      if (canvas.areaReserved(knockout.x, knockout.y, knockout.w, knockout.h)) continue;
      // Gap only against other labels: it is a spacing rule between names, not
      // a reason to refuse a name that reaches the edge of the screen.
      const Box spaced = inflate(knockout, style.placeLabelGapPx);
      if (scratch.taken.anySet(spaced.x, spaced.y, spaced.w, spaced.h)) continue;

      int routeCells = 0, totalCells = 0;
      scratch.route.coverage(knockout.x, knockout.y, knockout.w, knockout.h, routeCells, totalCells);
      if (totalCells > 0 && 100 * routeCells > style.placeLabelRouteOverlapPct * totalCells) continue;

      int inked = 0, samples = 0;
      if (scratch.inkTest) {
        canvas.inkCoverage(knockout.x, knockout.y, knockout.w, knockout.h, kInkStepPx, inked, samples);
        ++scratch.inkProbes;
        scratch.inkSamples += static_cast<uint32_t>(samples);
      }

      usable[p] = true;
      // No readback on this canvas -- and the switch above being off -- means
      // every position scores 0 and the pick below collapses to the first
      // usable one, the old first-fit behaviour exactly.
      ink[p] = samples > 0 ? inked * 1000 / samples : 0;
      if (ink[p] < leastInk) leastInk = ink[p];
      // Nothing can beat clean ground, and the tie-break below already prefers
      // the lowest index among equals, so the remaining positions cannot change
      // the answer. This is what keeps the cost near zero in open country: one
      // probe per label, same as the old first-fit.
      if (ink[p] == 0) break;
    }

    // Least ink wins, but only by a margin. Within kInkTolerancePermille the
    // cartographic order decides instead, because a name that jumps from the
    // right of its dot to below it for one per cent less ink makes the map look
    // unsettled between two redraws of the same place -- and the reader has to
    // re-find which dot each name belongs to.
    int bestIndex = -1;
    for (int p = 0; p < kPlacementCount; ++p) {
      if (!usable[p]) continue;
      if (ink[p] <= leastInk + kInkTolerancePermille) {
        bestIndex = p;
        break;
      }
    }

    const bool drew = straddlePlaced || bestIndex >= 0;
    if (drew) {
      const Box& textBox = straddlePlaced ? straddleBox : placements[bestIndex];
      const Box knockout = inflate(textBox, knockoutPad);
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
            drawFitted(canvas, fitted, textBox.x + offset.dx * radius, textBox.y + offset.dy * radius, textBox.w,
                       sizePx, bold, MapInk::White);
          }
        }
      }
      drawFitted(canvas, fitted, textBox.x, textBox.y, textBox.w, sizePx, bold, MapInk::Black);
      scratch.taken.markRect(knockout.x, knockout.y, knockout.w, knockout.h);
      ++scratch.placed;
      if (candidate.landmark) {
        ++placedLandmark;
      } else if (minor) {
        ++placedMinor;
      } else {
        ++placedMajor;
      }
    }
    if (!drew) ++scratch.dropped;
  }
}
