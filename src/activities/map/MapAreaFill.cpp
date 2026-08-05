#include "MapAreaFill.h"

#include <algorithm>

namespace {

// Where a scan line at `value` crosses the ring's edges, along the other axis.
// `horizontal` scans rows (value is y, crossings are x); otherwise columns.
//
// Half-open edge test (>= low, < high) so a vertex shared by two edges is
// counted once. Counting it twice pairs the wrong crossings and the hatch line
// leaks out of the ring -- the failure mode this whole function exists to
// avoid, and what test/map_area_fill checks for.
int collectCrossings(const int16_t* xs, const int16_t* ys, uint16_t pointCount, bool horizontal, int value, int* out,
                     int maxOut) {
  int count = 0;
  for (uint16_t i = 0; i + 1 < pointCount; ++i) {
    const int a = horizontal ? ys[i] : xs[i];
    const int b = horizontal ? ys[i + 1] : xs[i + 1];
    if (a == b) continue;
    const int low = a < b ? a : b;
    const int high = a < b ? b : a;
    if (value < low || value >= high) continue;

    const int aOther = horizontal ? xs[i] : ys[i];
    const int bOther = horizontal ? xs[i + 1] : ys[i + 1];
    // Doubles: the products overflow 32 bits well inside the int16 coordinate
    // range the projection produces, the same reason GfxRendererCanvas clips in
    // doubles.
    const double t = static_cast<double>(value - a) / (b - a);
    const int crossing = static_cast<int>(aOther + t * (bOther - aOther));
    if (count >= maxOut) break;
    out[count++] = crossing;
  }
  return count;
}

// Diagonal scan lines are indexed by c = x + y (diagonal) or c = x - y
// (antidiagonal), and cross an edge wherever that sum changes across it.
int collectDiagonalCrossings(const int16_t* xs, const int16_t* ys, uint16_t pointCount, bool antiDiagonal, int c,
                             int* outX, int maxOut) {
  int count = 0;
  for (uint16_t i = 0; i + 1 < pointCount; ++i) {
    const int a = antiDiagonal ? xs[i] - ys[i] : xs[i] + ys[i];
    const int b = antiDiagonal ? xs[i + 1] - ys[i + 1] : xs[i + 1] + ys[i + 1];
    if (a == b) continue;
    const int low = a < b ? a : b;
    const int high = a < b ? b : a;
    if (c < low || c >= high) continue;

    const double t = static_cast<double>(c - a) / (b - a);
    const int x = static_cast<int>(xs[i] + t * (xs[i + 1] - xs[i]));
    if (count >= maxOut) break;
    outX[count++] = x;
  }
  return count;
}

void ringBounds(const int16_t* xs, const int16_t* ys, uint16_t pointCount, int& minX, int& minY, int& maxX, int& maxY) {
  minX = maxX = xs[0];
  minY = maxY = ys[0];
  for (uint16_t i = 1; i < pointCount; ++i) {
    minX = std::min<int>(minX, xs[i]);
    maxX = std::max<int>(maxX, xs[i]);
    minY = std::min<int>(minY, ys[i]);
    maxY = std::max<int>(maxY, ys[i]);
  }
}

// One axis-aligned family of hatch lines, clipped to the ring.
void hatchAxis(IMapCanvas& canvas, const int16_t* xs, const int16_t* ys, uint16_t pointCount, bool horizontal,
               int spacingPx, int from, int to, MapInk ink) {
  int crossings[MapAreaFill::kMaxCrossings];
  // Anchored to a multiple of the spacing in screen space, not to the ring's
  // own bounding box: two adjacent buildings then share one hatch grid instead
  // of each starting its own, which is what makes a row of houses read as a
  // block rather than as noise.
  int start = from - (((from % spacingPx) + spacingPx) % spacingPx);
  for (int value = start; value <= to; value += spacingPx) {
    if (value < from) continue;
    const int count = collectCrossings(xs, ys, pointCount, horizontal, value, crossings, MapAreaFill::kMaxCrossings);
    if (count < 2) continue;
    std::sort(crossings, crossings + count);
    // Pairs, so an L-shaped or concave ring leaves its notch unfilled.
    for (int i = 0; i + 1 < count; i += 2) {
      if (horizontal) {
        canvas.drawLine(crossings[i], value, crossings[i + 1], value, 1, ink);
      } else {
        canvas.drawLine(value, crossings[i], value, crossings[i + 1], 1, ink);
      }
    }
  }
}

void hatchDiagonal(IMapCanvas& canvas, const int16_t* xs, const int16_t* ys, uint16_t pointCount, bool antiDiagonal,
                   int spacingPx, MapInk ink) {
  int minX, minY, maxX, maxY;
  ringBounds(xs, ys, pointCount, minX, minY, maxX, maxY);
  const int from = antiDiagonal ? minX - maxY : minX + minY;
  const int to = antiDiagonal ? maxX - minY : maxX + maxY;
  // Diagonal lines are 1.41 px apart for every 1 of index, so the index step
  // is scaled to keep the *visual* gap at spacingPx. 7/5 is 1.4 without
  // floating point, close enough for a hatch.
  const int step = std::max(1, spacingPx * 7 / 5);

  int crossX[MapAreaFill::kMaxCrossings];
  int start = from - (((from % step) + step) % step);
  for (int c = start; c <= to; c += step) {
    if (c < from) continue;
    const int count = collectDiagonalCrossings(xs, ys, pointCount, antiDiagonal, c, crossX, MapAreaFill::kMaxCrossings);
    if (count < 2) continue;
    std::sort(crossX, crossX + count);
    for (int i = 0; i + 1 < count; i += 2) {
      const int x1 = crossX[i];
      const int x2 = crossX[i + 1];
      // y follows from the line's own definition, so no second interpolation.
      const int y1 = antiDiagonal ? x1 - c : c - x1;
      const int y2 = antiDiagonal ? x2 - c : c - x2;
      canvas.drawLine(x1, y1, x2, y2, 1, ink);
    }
  }
}

}  // namespace

void MapAreaFill::hatchRing(IMapCanvas& canvas, const int16_t* xs, const int16_t* ys, const uint16_t pointCount,
                            const Pattern pattern, const int spacingPx, const MapInk ink) {
  if (pattern == Pattern::None || spacingPx <= 0 || pointCount < 4) return;

  int minX, minY, maxX, maxY;
  ringBounds(xs, ys, pointCount, minX, minY, maxX, maxY);

  switch (pattern) {
    case Pattern::Horizontal:
      hatchAxis(canvas, xs, ys, pointCount, true, spacingPx, minY, maxY, ink);
      break;
    case Pattern::Vertical:
      hatchAxis(canvas, xs, ys, pointCount, false, spacingPx, minX, maxX, ink);
      break;
    case Pattern::Cross:
      hatchAxis(canvas, xs, ys, pointCount, true, spacingPx, minY, maxY, ink);
      hatchAxis(canvas, xs, ys, pointCount, false, spacingPx, minX, maxX, ink);
      break;
    case Pattern::Diagonal:
      hatchDiagonal(canvas, xs, ys, pointCount, false, spacingPx, ink);
      break;
    case Pattern::AntiDiagonal:
      hatchDiagonal(canvas, xs, ys, pointCount, true, spacingPx, ink);
      break;
    case Pattern::None:
      break;
  }
}

void MapAreaFill::outlineRing(IMapCanvas& canvas, const int16_t* xs, const int16_t* ys, const uint16_t pointCount,
                              const int lineWidth, const MapInk ink) {
  if (lineWidth <= 0 || pointCount < 2) return;
  for (uint16_t i = 1; i < pointCount; ++i) {
    canvas.drawLine(xs[i - 1], ys[i - 1], xs[i], ys[i], lineWidth, ink);
  }
}
