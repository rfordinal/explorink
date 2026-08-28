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

  // `out` gets this mask grown by `radius` pixels in every direction -- the white
  // outline, built in mask space so it costs one extra rotated pass rather than
  // eight per radius (which is what a halo drawn by redrawing the text costs).
  //
  // **The output box grows by the radius on each side.** Reusing this mask's own
  // size would clip the outermost ring of the halo against the text box, which is
  // invisible at radius 1 -- glyphs rarely touch their own box edge -- and obvious
  // at 2. Growing symmetrically also keeps the centre aligned, so the blit needs
  // no separate offset.
  //
  // Returns false when the grown mask would not fit, and the caller must then
  // draw no halo rather than a clipped one.
  bool dilateInto(MapTextMask& out, const int radius) const {
    if (radius <= 0) return false;
    const int grownW = w + 2 * radius;
    const int grownH = h + 2 * radius;
    if (!out.fits(grownW, grownH)) return false;
    out.reset(grownW, grownH);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        if (!get(x, y)) continue;
        for (int oy = -radius; oy <= radius; ++oy) {
          for (int ox = -radius; ox <= radius; ++ox) out.set(x + radius + ox, y + radius + oy);
        }
      }
    }
    return true;
  }
};

// The rotation basis for text whose glyphs' top should point along (upX, upY).
//
// **Built from `up` alone, and that is the whole point.** The first version took
// the reading direction and the up direction separately -- right = the contour's
// tangent, down = away from the higher ground -- and those two are not
// independent: that pair has determinant -1, so it is a reflection and every
// number came out mirrored. Seen on the panel-sized render, 2026-08-26, after it
// had already been wrong once in the other direction.
//
// From `up` there is only one answer and it cannot be a reflection:
//
//     down  = -up
//     right = (-up.y, up.x)     determinant = up.x^2 + up.y^2, always positive
//
// The reading direction falls out of it rather than being chosen, which is
// correct: which way along a contour its points happen to be stored is arbitrary.
//
// Scale is 1/1024ths. Returns false when `up` is zero, and the caller must then
// draw upright rather than draw nothing -- a number with no orientation still
// says its height.
// Integer square root, floor, one bit at a time: 16 iterations for any input, no
// float, and -- unlike the `while (root * root < len2) ++root;` this replaces --
// no loop whose length depends on the value. That version was a hang: `up` comes
// from two neighbouring contour vertices, only the chosen one is required to be
// on screen (MapRenderer::bestLabelVertex), and a tile-projected neighbour can be
// millions of pixels away. At |up| around 46,341 the square overflows int32,
// `root * root` wraps negative, the condition never goes false, and the render
// task spins until the watchdog resets the device.
inline uint32_t mapTextIsqrt(uint32_t value) {
  uint32_t rest = value;
  uint32_t root = 0;
  uint32_t bit = 1u << 30;
  while (bit > rest) bit >>= 2;
  while (bit != 0) {
    if (rest >= root + bit) {
      rest -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

inline bool mapTextBasisFromUp(const int32_t upX, const int32_t upY, int& rightX, int& rightY, int& downX,
                               int& downY) {
  // Only the direction matters, so shrinking the vector is free -- and it is the
  // guard that keeps the square inside int32 for any input the projection can
  // produce. 16,384^2 * 2 is 536 M, comfortably under 2^31, and halving reaches
  // that from INT32_MAX in 17 steps.
  int32_t ax = upX;
  int32_t ay = upY;
  while (ax > 16384 || ax < -16384 || ay > 16384 || ay < -16384) {
    ax /= 2;
    ay /= 2;
  }
  const int32_t len2 = ax * ax + ay * ay;
  if (len2 <= 0) {
    rightX = 1024;
    rightY = 0;
    downX = 0;
    downY = 1024;
    return false;
  }
  // Ceil, not floor: the basis has to reach 1024 rather than exceed it, or the
  // blit's transpose-inverse can step outside the mask.
  int32_t root = static_cast<int32_t>(mapTextIsqrt(static_cast<uint32_t>(len2)));
  if (root * root < len2) ++root;
  rightX = static_cast<int>(-ay * 1024 / root);
  rightY = static_cast<int>(ax * 1024 / root);
  downX = static_cast<int>(-ax * 1024 / root);
  downY = static_cast<int>(-ay * 1024 / root);
  return true;
}

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
