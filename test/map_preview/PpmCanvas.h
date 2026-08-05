#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "IMapCanvas.h"

// Native-only IMapCanvas implementation: rasterizes into an in-memory 1
// byte/pixel buffer (0 = white, 1 = black) and dumps it as a binary PPM
// (P6) image -- no image library, no dependency on the real GfxRenderer/HAL
// stack.
//
// Close to the device's output, not provably byte-identical to it. Same
// coordinates, same widths, and the same thick-line decomposition
// (MapStroke.h, shared with GfxRendererCanvas), but the one-pixel line
// underneath is this file's Bresenham rather than GfxRenderer's, so a diagonal
// can differ by a pixel here and there. Good enough to judge a style by; not a
// substitute for looking at the panel once.
class PpmCanvas : public IMapCanvas {
 public:
  PpmCanvas(int width, int height);

  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, MapInk ink) override;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, MapInk ink) override;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, MapInk ink) override;

  bool writePpm(const std::string& path) const;

  // Raw 1 byte/pixel buffer, for tests that compare two renders against each
  // other rather than against the committed PPM.
  const std::vector<uint8_t>& pixels() const { return pixels_; }

 private:
  void setPixel(int x, int y, MapInk ink);
  void drawThinLine(int x1, int y1, int x2, int y2, MapInk ink);

  int width_;
  int height_;
  std::vector<uint8_t> pixels_;  // width_ * height_, 0 = white, 1 = black

  // Scanline scratch for fillPolygon, reserved once in the constructor.
  // A local vector here would allocate on every polygon and show up in the
  // HeapProbe measurement as if the map path had done it (HeapProbe.h).
  std::vector<int> crossings_;
};
