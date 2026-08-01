#pragma once

#include "GfxRenderer.h"
#include "IMapCanvas.h"

// Real-firmware IMapCanvas implementation: forwards each call straight to
// the real GfxRenderer. Counterpart to test/map_preview/PpmCanvas (the
// native preview implementation) -- MapRenderer's drawing logic is
// identical in both, only this adapter differs.
class GfxRendererCanvas : public IMapCanvas {
 public:
  explicit GfxRendererCanvas(GfxRenderer& renderer) : renderer_(renderer) {}

  void drawLine(int x1, int y1, int x2, int y2, int lineWidth) override {
    renderer_.drawLine(x1, y1, x2, y2, lineWidth, true);
  }

  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius) override {
    renderer_.fillRoundedRect(x, y, width, height, cornerRadius, Color::Black);
  }

  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints) override {
    renderer_.fillPolygon(xPoints, yPoints, numPoints, true);
  }

 private:
  GfxRenderer& renderer_;
};
