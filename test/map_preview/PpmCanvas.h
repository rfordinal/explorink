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
  void fillSpan(int x1, int x2, int y, MapAreaTone tone) override;

  // Real font metrics and real glyphs, out of the same built-in Noto Sans the
  // device draws with -- see PreviewFont.h for what is shared and what is only
  // close.
  bool measureText(const char* utf8, int sizePx, bool bold, int& outWidth, int& outHeight) override;
  void drawText(int x, int y, const char* utf8, int sizePx, bool bold, MapInk ink) override;
  void drawTextTurned(int centreX, int centreY, const char* utf8, int sizePx, bool bold, MapInk ink,
                      MapTextTurn turn) override;

  // The whole canvas: the preview has no header band to keep clear (the device
  // adapter does -- GfxRendererCanvas's minY).
  void drawableRect(int& outX, int& outY, int& outWidth, int& outHeight) const override;

  bool writePpm(const std::string& path) const;

  // Loads a canvas back from a PPM this class itself wrote (binary P6,
  // black=(0,0,0)/white=(255,255,255) exactly, no other bilevel-canvas
  // producer promised) -- for stamping something onto an already-rendered
  // frame without re-running the geometry pass that produced it (see
  // test/map_preview's marker_stamp: renders the map once per real redraw,
  // draws MapRenderer::drawMarker() at wherever the marker actually is once
  // per packet on a copy). Width/height must match the canvas already
  // constructed with; returns false on any mismatch or malformed header
  // rather than guessing.
  bool readPpm(const std::string& path);

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
