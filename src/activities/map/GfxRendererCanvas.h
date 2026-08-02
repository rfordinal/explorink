#pragma once

#include <algorithm>
#include <cmath>

#include "GfxRenderer.h"
#include "IMapCanvas.h"

// Real-firmware IMapCanvas implementation: forwards each call to the real
// GfxRenderer, after clipping it to the screen. Counterpart to
// test/map_preview/PpmCanvas (the native preview implementation) --
// MapRenderer's drawing logic is identical in both, only this adapter
// differs.
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

  void drawLine(int x1, int y1, int x2, int y2, int lineWidth) override {
    // drawLine(lineWidth) stacks `lineWidth` one-pixel lines downward in y,
    // so the lowest of them must still land on screen.
    const int maxY = renderer_.getScreenHeight() - lineWidth;
    if (maxY < 0) return;
    if (!clipToRect(x1, y1, x2, y2, renderer_.getScreenWidth() - 1, maxY)) return;
    renderer_.drawLine(x1, y1, x2, y2, lineWidth, true);
  }

  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius) override {
    // Place dots come from a tile range wider than the viewport, so most of
    // them fall off screen. Rounded-rect fill has no clipped form here, so a
    // dot is drawn only when it fits whole. A place near the screen edge is
    // a label-layout question (docs/map-render-spec.md, off-screen place
    // chevrons), not something to half-draw.
    if (!fullyOnScreen(x, y, width, height)) return;
    renderer_.fillRoundedRect(x, y, width, height, cornerRadius, Color::Black);
  }

  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints) override {
    for (int i = 0; i < numPoints; ++i) {
      if (!onScreen(xPoints[i], yPoints[i])) return;
    }
    renderer_.fillPolygon(xPoints, yPoints, numPoints, true);
  }

 private:
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
