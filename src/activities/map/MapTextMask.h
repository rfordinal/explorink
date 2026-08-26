#pragma once

#include <cstdint>

// A small 1bpp bitmap of rendered text, and the rotation that puts it on screen
// at any angle.
//
// **Why a mask at all.** A contour's height number has to sit along the contour,
// at the line's own bearing at that spot -- that is what makes the number and the
// piece of line under it read as one thing, and it is how the reader gets the
// slope direction without counting. Quarter turns cannot do it: a contour running
// south-east gets a number turned due south, and the mismatch is exactly what a
// map reader notices first.
//
// Neither font path can rotate freely, and neither should learn to: the device's
// glyph renderer is the text path every screen draws through, and the host's is a
// preview rasteriser. So both are asked only for "this string as a bitmap", and
// the rotation happens here, once, in code both sides compile.
//
// **Inverse mapping, not forward.** Walking source pixels and rotating each one
// leaves holes at most angles -- a 1 px stem becomes a dotted line. Walking
// destination pixels and asking where each came from cannot: every destination
// pixel is written exactly once. Nearest neighbour, no filtering, because the
// panel has two values and a filtered edge would have to become a dither
// (docs/map-render-spec.md, "1-bit rules").
struct MapTextMask {
  // 80 x 32 covers a five-digit height at any face this map uses, and costs 320
  // bytes. A string that does not fit is not drawn rather than clipped: half a
  // number is a wrong number.
  static constexpr int kMaxW = 80;
  static constexpr int kMaxH = 32;

  uint8_t bits[(kMaxW * kMaxH + 7) / 8] = {};
  int w = 0;
  int h = 0;

  void reset(const int width, const int height) {
    w = width < 0 ? 0 : (width > kMaxW ? kMaxW : width);
    h = height < 0 ? 0 : (height > kMaxH ? kMaxH : height);
    for (uint8_t& byte : bits) byte = 0;
  }

  bool fits(const int width, const int height) const { return width <= kMaxW && height <= kMaxH; }

  void set(const int x, const int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const int index = y * kMaxW + x;
    bits[index >> 3] |= static_cast<uint8_t>(1 << (index & 7));
  }

  bool get(const int x, const int y) const {
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    const int index = y * kMaxW + x;
    return ((bits[index >> 3] >> (index & 7)) & 1) != 0;
  }

  // `out` gets this mask grown by one pixel in all eight directions -- the white
  // outline, built in mask space so it costs one extra rotated pass instead of
  // eight (which is what a halo drawn by redrawing the text nine times costs).
  void dilateInto(MapTextMask& out) const {
    out.reset(w, h);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        if (!get(x, y)) continue;
        for (int oy = -1; oy <= 1; ++oy) {
          for (int ox = -1; ox <= 1; ++ox) out.set(x + ox, y + oy);
        }
      }
    }
  }
};

// Draw `mask` centred on (centreX, centreY), rotated by the orthonormal basis
// (rightX, rightY) / (downX, downY), each in 1/1024ths. `plot(x, y)` receives
// every destination pixel that lands inside the glyphs.
//
// Fixed point rather than float: the ESP32-C3 has no FPU, and this runs per
// destination pixel.
template <typename Plot>
void mapTextMaskBlit(const MapTextMask& mask, const int centreX, const int centreY, const int rightX,
                     const int rightY, const int downX, const int downY, Plot plot) {
  if (mask.w <= 0 || mask.h <= 0) return;
  const int halfW = mask.w / 2;
  const int halfH = mask.h / 2;

  // The destination box: the rotated corners of the mask, so no pixel of it is
  // ever outside the range walked below.
  const int cornerU[4] = {-halfW, mask.w - halfW, -halfW, mask.w - halfW};
  const int cornerV[4] = {-halfH, -halfH, mask.h - halfH, mask.h - halfH};
  int minX = 0, maxX = 0, minY = 0, maxY = 0;
  for (int i = 0; i < 4; ++i) {
    const int px = (cornerU[i] * rightX + cornerV[i] * downX) >> 10;
    const int py = (cornerU[i] * rightY + cornerV[i] * downY) >> 10;
    if (i == 0 || px < minX) minX = px;
    if (i == 0 || px > maxX) maxX = px;
    if (i == 0 || py < minY) minY = py;
    if (i == 0 || py > maxY) maxY = py;
  }

  // One pixel of slack: the corner arithmetic truncates, and a stem clipped by
  // one row is the kind of thing nobody notices until it is on the glass.
  for (int py = minY - 1; py <= maxY + 1; ++py) {
    for (int px = minX - 1; px <= maxX + 1; ++px) {
      // Inverse rotation is the transpose, the basis being orthonormal.
      const int u = ((px * rightX + py * rightY) >> 10) + halfW;
      const int v = ((px * downX + py * downY) >> 10) + halfH;
      if (u < 0 || v < 0 || u >= mask.w || v >= mask.h) continue;
      if (!mask.get(u, v)) continue;
      plot(centreX + px, centreY + py);
    }
  }
}
