#pragma once

// Minimal drawing surface MapRenderer needs. Keeps MapRenderer free of any
// HAL/GfxRenderer dependency, so the exact same drawing logic (what to draw,
// where) runs both in the native preview (test/map_preview/PpmCanvas) and the
// real firmware (MapActivity's GfxRendererCanvas adapter, wrapping the real
// GfxRenderer) -- only this thin adapter differs between the two.
class IMapCanvas {
 public:
  virtual ~IMapCanvas() = default;

  virtual void drawLine(int x1, int y1, int x2, int y2, int lineWidth) = 0;
  virtual void fillRoundedRect(int x, int y, int width, int height, int cornerRadius) = 0;
  virtual void fillPolygon(const int* xPoints, const int* yPoints, int numPoints) = 0;
};
