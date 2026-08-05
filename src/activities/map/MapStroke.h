#pragma once

#include <cmath>

// How a thick map line is built out of one-pixel lines.
//
// This exists because GfxRenderer's own thick line is not usable for a map.
// GfxRenderer.cpp:713-717 draws `lineWidth` copies offset **downward in y
// only**, which is fine for the UI's horizontal rules and wrong for roads: a
// north-south road's copies all land on top of each other, so it stays one
// pixel wide however wide the style says it is, while an east-west road of the
// same class comes out full width. Per-class widths would have been visible on
// half the compass and invisible on the other half.
//
// Both IMapCanvas implementations share this, so the laptop preview and the
// device agree about how wide a road is and where its pixels go. GfxRenderer
// stays untouched -- it is inherited code and the UI depends on its current
// behaviour (TrailInk CLAUDE.md, "Treat inherited code as upstream's").
namespace MapStroke {

// Copies are offset along one axis, not along the true perpendicular.
//
// Offsetting along the perpendicular is the obvious approach and it stripes:
// on a diagonal the perpendicular is diagonal too, so consecutive copies land
// 1.41 px apart and the road comes out as parallel hairlines with white
// between them. Verified by looking at it, 2026-08-05.
//
// Stacking along the dominant axis cannot leave a gap, because consecutive
// copies are exactly one pixel apart in that axis and Bresenham fills every
// step of the other one. The count is scaled by len/major so the *perpendicular*
// thickness still comes out at `lineWidth`: a 45-degree road needs 1.41x as many
// copies as a horizontal one to look equally wide.
struct Stack {
  bool alongY = true;  // true: offsets are (0, k). false: (k, 0).
  int count = 1;
  int first = 0;  // k runs first .. first + count - 1
};

inline Stack stackFor(int x1, int y1, int x2, int y2, int lineWidth) {
  Stack stack;
  // A hairline is one Bresenham line, whatever the angle. Scaling the count
  // here would make a diagonal hairline two pixels thick, and "1 px" in a
  // style file means one pixel.
  if (lineWidth <= 1) return stack;

  const int dx = x2 > x1 ? x2 - x1 : x1 - x2;
  const int dy = y2 > y1 ? y2 - y1 : y1 - y2;
  const int major = dx > dy ? dx : dy;
  stack.alongY = dx >= dy;

  if (major == 0) {
    // Zero-length segment: no direction to be perpendicular to. `lineWidth`
    // stacked copies of a single pixel is a short bar, which is what a
    // one-point way already looks like.
    stack.count = lineWidth;
  } else {
    const double length = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
    const double scaled = lineWidth * length / major;
    // Round up: one copy too many is a road a fraction of a pixel too wide,
    // one too few is a visible gap.
    stack.count = static_cast<int>(std::ceil(scaled - 1e-9));
    if (stack.count < 1) stack.count = 1;
  }
  stack.first = -((stack.count - 1) / 2);
  return stack;
}

}  // namespace MapStroke
