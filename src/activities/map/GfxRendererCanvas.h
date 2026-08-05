#pragma once

#include <algorithm>
#include <cmath>

#include "GfxRenderer.h"
#include "IMapCanvas.h"
#include "MapStroke.h"

// Real-firmware IMapCanvas implementation: forwards each call to the real
// GfxRenderer, after clipping it to the screen. Counterpart to
// test/map_preview/PpmCanvas (the native preview implementation) --
// MapRenderer's drawing logic is identical in both, only this adapter
// differs.
//
// Thick lines are decomposed here into one-pixel lines rather than handed to
// GfxRenderer::drawLine(lineWidth), which offsets its copies downward in y and
// would leave every north-south road one pixel wide -- see MapStroke.h.
//
// Clipping lives here rather than in MapRenderer because it is a property of
// this output surface, and because GfxRenderer::drawPixel answers an
// out-of-range pixel with a LOG_ERR. Map geometry is loaded for the whole
// tile range, which is deliberately wider than the screen, so an unclipped
// map pass emits tens of thousands of error lines over USB CDC and spends
// far longer logging them than drawing. PpmCanvas::setPixel drops
// out-of-range pixels silently, so clipping here changes no rendered pixel
// -- it only stops the flood.
class GfxRendererCanvas : public IMapCanvas {
 public:
  explicit GfxRendererCanvas(GfxRenderer& renderer) : renderer_(renderer) {}

  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, MapInk ink) override {
    const int maxX = renderer_.getScreenWidth() - 1;
    const int maxY = renderer_.getScreenHeight() - 1;
    const MapStroke::Stack stack = MapStroke::stackFor(x1, y1, x2, y2, lineWidth);
    // Clipped per copy, not once for the bundle: each copy is its own segment,
    // and a wide road along a screen edge has some copies on screen and some
    // off it.
    for (int i = 0; i < stack.count; ++i) {
      const int k = stack.first + i;
      const int offsetX = stack.alongY ? 0 : k;
      const int offsetY = stack.alongY ? k : 0;
      int cx1 = x1 + offsetX, cy1 = y1 + offsetY, cx2 = x2 + offsetX, cy2 = y2 + offsetY;
      if (!clipToRect(cx1, cy1, cx2, cy2, maxX, maxY)) continue;
      renderer_.drawLine(cx1, cy1, cx2, cy2, ink == MapInk::Black);
    }
  }

  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, MapInk ink) override {
    // Place dots come from a tile range wider than the viewport, so most of
    // them fall off screen. Rounded-rect fill has no clipped form here, so a
    // dot is drawn only when it fits whole. A place near the screen edge is
    // a label-layout question (docs/map-render-spec.md, off-screen place
    // chevrons), not something to half-draw.
    if (!fullyOnScreen(x, y, width, height)) return;
    renderer_.fillRoundedRect(x, y, width, height, cornerRadius, ink == MapInk::Black ? Color::Black : Color::White);
  }

  void fillSpan(int x1, int x2, int y, MapAreaTone tone) override {
    if (tone == MapAreaTone::None) return;
    if (y < 0 || y >= renderer_.getScreenHeight()) return;
    if (x2 < x1) {
      const int swap = x1;
      x1 = x2;
      x2 = swap;
    }
    x1 = std::max(x1, 0);
    x2 = std::min(x2, renderer_.getScreenWidth() - 1);
    if (x2 < x1) return;

    // GfxRenderer's own dithered fill for the tones it already has: one call
    // for the whole run, and the pattern is the same screen-space one
    // MapTone::inkAt mirrors. Its phase comes from the absolute coordinates, so
    // splitting a fill into spans cannot shift it.
    if (MapTone::hasNativeDither(tone)) {
      renderer_.fillRectDither(x1, y, x2 - x1 + 1, 1, colorFor(tone));
      return;
    }
    // Stipple has no GfxRenderer equivalent, so it is painted pixel by pixel.
    // Only every third pixel is touched, which is what makes that affordable.
    for (int x = x1; x <= x2; ++x) {
      if (MapTone::inkAt(x, y, tone)) renderer_.drawPixel(x, y, true);
    }
  }

  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, MapInk ink) override {
    for (int i = 0; i < numPoints; ++i) {
      if (!onScreen(xPoints[i], yPoints[i])) return;
    }
    renderer_.fillPolygon(xPoints, yPoints, numPoints, ink == MapInk::Black);
  }

 private:
  // Grey is coming: a second branch is adding the panel's real grey levels, and
  // when it lands a tone should map to one of those instead of to a dither
  // pattern. That swap belongs here and in PpmCanvas, behind MapAreaTone --
  // nothing above this line needs to know which it got.
  static Color colorFor(const MapAreaTone tone) {
    switch (tone) {
      case MapAreaTone::Solid:
        return Color::Black;
      case MapAreaTone::Dark:
        return Color::DarkGray;
      case MapAreaTone::Light:
        return Color::LightGray;
      case MapAreaTone::Stipple:
      case MapAreaTone::None:
        break;
    }
    return Color::Clear;
  }

  bool onScreen(int x, int y) const {
    return x >= 0 && y >= 0 && x < renderer_.getScreenWidth() && y < renderer_.getScreenHeight();
  }

  bool fullyOnScreen(int x, int y, int width, int height) const {
    return x >= 0 && y >= 0 && x + width <= renderer_.getScreenWidth() && y + height <= renderer_.getScreenHeight();
  }

  static int outcode(double x, double y, int maxX, int maxY) {
    int code = 0;
    if (x < 0.0) code |= 1;
    if (x > maxX) code |= 2;
    if (y < 0.0) code |= 4;
    if (y > maxY) code |= 8;
    return code;
  }

  // Cohen-Sutherland, run in doubles and rounded once at the end. Endpoints
  // are clipped, not pixels: a road segment can start thousands of pixels
  // off screen, and per-pixel rejection would still walk every one of them.
  // The intersections are computed in doubles because the cross-products
  // overflow 32 bits well inside the int16 coordinate range the projection
  // produces.
  static bool clipToRect(int& outX1, int& outY1, int& outX2, int& outY2, int maxX, int maxY) {
    double x1 = outX1, y1 = outY1, x2 = outX2, y2 = outY2;
    int c1 = outcode(x1, y1, maxX, maxY);
    int c2 = outcode(x2, y2, maxX, maxY);

    // Four edges, so four replacements is the true worst case; the guard is
    // only there so no rounding pathology can spin here forever.
    for (int guard = 0; guard < 8; ++guard) {
      if ((c1 | c2) == 0) {
        outX1 = std::clamp(static_cast<int>(std::lround(x1)), 0, maxX);
        outY1 = std::clamp(static_cast<int>(std::lround(y1)), 0, maxY);
        outX2 = std::clamp(static_cast<int>(std::lround(x2)), 0, maxX);
        outY2 = std::clamp(static_cast<int>(std::lround(y2)), 0, maxY);
        return true;
      }
      if ((c1 & c2) != 0) return false;  // both endpoints outside the same edge

      const int c = c1 != 0 ? c1 : c2;
      const double dx = x2 - x1;
      const double dy = y2 - y1;
      double nx = 0.0, ny = 0.0;
      if (c & 8) {  // below the bottom edge
        nx = x1 + dx * (maxY - y1) / dy;
        ny = maxY;
      } else if (c & 4) {  // above the top edge
        nx = x1 + dx * (0.0 - y1) / dy;
        ny = 0.0;
      } else if (c & 2) {  // right of the right edge
        ny = y1 + dy * (maxX - x1) / dx;
        nx = maxX;
      } else {  // left of the left edge
        ny = y1 + dy * (0.0 - x1) / dx;
        nx = 0.0;
      }

      if (c == c1) {
        x1 = nx;
        y1 = ny;
        c1 = outcode(x1, y1, maxX, maxY);
      } else {
        x2 = nx;
        y2 = ny;
        c2 = outcode(x2, y2, maxX, maxY);
      }
    }
    return false;
  }

  GfxRenderer& renderer_;
};
