#pragma once

#include <algorithm>
#include <cmath>

#include "GfxRenderer.h"
#include "IMapCanvas.h"
#include "MapStroke.h"
#include "fontIds.h"

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
//
// `minY` moves the top clip edge down from 0 -- the map screen's header is a
// fixed band the map itself must never draw into (docs/map-header-status.md),
// and clipping here means it genuinely does not: nothing above minY reaches
// GfxRenderer at all, rather than being drawn and then painted over. Default
// 0 keeps every other caller (test/map_preview has no header) unclipped at
// the top, same as before this existed.
class GfxRendererCanvas : public IMapCanvas {
 public:
  explicit GfxRendererCanvas(GfxRenderer& renderer, int minY = 0) : renderer_(renderer), minY_(minY) {}

  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, MapInk ink) override {
    const int maxX = renderer_.getScreenWidth() - 1;
    const int maxY = renderer_.getScreenHeight() - 1;
    const MapStroke::Stack stack = MapStroke::stackFor(x1, y1, x2, y2, lineWidth);

    // Does the whole bundle fit on the screen? If it does, no copy needs
    // clipping and none of them enters clipToRect at all.
    //
    // Worth the four compares: a road is drawn twice (MapRenderer::kRoadPasses)
    // and a 6 px road at 45 degrees is nine copies, so a segment used to pay
    // eighteen Cohen-Sutherland runs in `double` -- on a target with no FPU --
    // to learn eighteen times that it was already inside. Measured 2026-08-06:
    // roads were 2,146 ms of rung 2's 2,927.
    //
    // The per-copy path is still there and still correct: a wide road along a
    // screen edge genuinely has some copies on and some off, which is why this
    // is a fast path and not a replacement.
    const int loK = stack.first;
    const int hiK = stack.first + stack.count - 1;
    int boundsX1 = x1 < x2 ? x1 : x2;
    int boundsX2 = x1 < x2 ? x2 : x1;
    int boundsY1 = y1 < y2 ? y1 : y2;
    int boundsY2 = y1 < y2 ? y2 : y1;
    if (stack.alongY) {
      boundsY1 += loK;
      boundsY2 += hiK;
    } else {
      boundsX1 += loK;
      boundsX2 += hiK;
    }
    const bool wholeStackOnScreen = boundsX1 >= 0 && boundsY1 >= minY_ && boundsX2 <= maxX && boundsY2 <= maxY;

    for (int i = 0; i < stack.count; ++i) {
      const int k = stack.first + i;
      const int offsetX = stack.alongY ? 0 : k;
      const int offsetY = stack.alongY ? k : 0;
      int cx1 = x1 + offsetX, cy1 = y1 + offsetY, cx2 = x2 + offsetX, cy2 = y2 + offsetY;
      if (!wholeStackOnScreen && !clipToRect(cx1, cy1, cx2, cy2, maxX, maxY, minY_)) continue;
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
    if (y < minY_ || y >= renderer_.getScreenHeight()) return;
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

  // Place labels are drawn with a sans face already in flash -- proportional, no
  // serifs, Latin plus the diacritics Slovak place names need
  // (docs/place-labels.md, "The font"). The style asks for a line height in
  // device pixels and this picks the largest face that does not exceed it;
  // measureText then reports what it actually got, so nothing is laid out
  // against the wish.
  bool measureText(const char* utf8, int sizePx, bool bold, int& outWidth, int& outHeight) override {
    if (utf8 == nullptr || *utf8 == '\0') {
      outWidth = 0;
      outHeight = 0;
      return true;
    }
    const int fontId = fontIdForSize(sizePx);
    outWidth = renderer_.getTextWidth(fontId, utf8, styleFor(bold));
    // Line height, not the string's own ink height: every label on screen wants
    // the same box depth, or a name with no descender sits in a visibly
    // shallower box than the one next to it.
    outHeight = renderer_.getLineHeight(fontId);
    return outWidth > 0 && outHeight > 0;
  }

  // Faces a label may be drawn with, smallest line height first. The name of a
  // built-in font says its point size at 150 DPI, not its pixel height
  // (lib/EpdFont/scripts/fontconvert.py: ppem = size * 150 / 72), so "12" is a
  // 34 px line -- which is why the sizes here are read at runtime instead of
  // assumed from the names.
  //
  // Consequence worth knowing: the smallest Noto Sans built in is a 34 px line,
  // too tall for a compact map label, so today's map labels come out as the
  // Ubuntu UI face at 24/29 px. docs/place-labels.md, "The font", has what a
  // purpose-built map face would change and what it would cost.
  static constexpr int kLabelFontIds[] = {SMALL_FONT_ID,       UI_10_FONT_ID,       UI_12_FONT_ID,
                                          NOTOSANS_12_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_16_FONT_ID,
                                          NOTOSANS_18_FONT_ID};

  // GfxRenderer::drawText's y is the top of the ascender box (it adds the
  // ascender itself), which is exactly IMapCanvas's top-left contract.
  void drawText(int x, int y, const char* utf8, int sizePx, bool bold, MapInk ink) override {
    if (utf8 == nullptr || *utf8 == '\0') return;
    renderer_.drawText(fontIdForSize(sizePx), x, y, utf8, ink == MapInk::Black, styleFor(bold));
  }

  void drawableRect(int& outX, int& outY, int& outWidth, int& outHeight) const override {
    outX = 0;
    outY = minY_;
    outWidth = renderer_.getScreenWidth();
    outHeight = renderer_.getScreenHeight() - minY_;
  }

 private:
  static EpdFontFamily::Style styleFor(const bool bold) { return bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR; }

  // Largest face whose line height fits in `sizePx`, or the smallest face when
  // even that does not fit. Nearest-below rather than nearest, because a face
  // taller than the style asked for makes every label box taller than the style
  // author was looking at when they picked the number.
  int fontIdForSize(const int sizePx) const {
    int best = kLabelFontIds[0];
    int bestHeight = 0;
    for (const int fontId : kLabelFontIds) {
      const int height = renderer_.getLineHeight(fontId);
      if (height <= 0 || height > sizePx) continue;
      if (height > bestHeight) {
        bestHeight = height;
        best = fontId;
      }
    }
    return best;
  }

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
    return x >= 0 && y >= minY_ && x < renderer_.getScreenWidth() && y < renderer_.getScreenHeight();
  }

  bool fullyOnScreen(int x, int y, int width, int height) const {
    return x >= 0 && y >= minY_ && x + width <= renderer_.getScreenWidth() && y + height <= renderer_.getScreenHeight();
  }

  static int outcode(double x, double y, int maxX, int maxY, int minY) {
    int code = 0;
    if (x < 0.0) code |= 1;
    if (x > maxX) code |= 2;
    if (y < minY) code |= 4;
    if (y > maxY) code |= 8;
    return code;
  }

  // Cohen-Sutherland, run in doubles and rounded once at the end. Endpoints
  // are clipped, not pixels: a road segment can start thousands of pixels
  // off screen, and per-pixel rejection would still walk every one of them.
  // The intersections are computed in doubles because the cross-products
  // overflow 32 bits well inside the int16 coordinate range the projection
  // produces.
  static bool clipToRect(int& outX1, int& outY1, int& outX2, int& outY2, int maxX, int maxY, int minY = 0) {
    // Trivial accept, in integers, before a single `double` is touched. The
    // function used to convert all four coordinates and compute its outcodes in
    // `double` even for a segment wholly inside the screen -- for which the whole
    // body is a no-op that rounds its own inputs back to where they started.
    //
    // This is the second line of defence behind the bundle test in drawLine():
    // it catches the individual copies of a stack that straddles an edge, where
    // most copies are still inside.
    if (outX1 >= 0 && outX1 <= maxX && outY1 >= minY && outY1 <= maxY && outX2 >= 0 && outX2 <= maxX && outY2 >= minY &&
        outY2 <= maxY) {
      return true;
    }
    double x1 = outX1, y1 = outY1, x2 = outX2, y2 = outY2;
    int c1 = outcode(x1, y1, maxX, maxY, minY);
    int c2 = outcode(x2, y2, maxX, maxY, minY);

    // Four edges, so four replacements is the true worst case; the guard is
    // only there so no rounding pathology can spin here forever.
    for (int guard = 0; guard < 8; ++guard) {
      if ((c1 | c2) == 0) {
        outX1 = std::clamp(static_cast<int>(std::lround(x1)), 0, maxX);
        outY1 = std::clamp(static_cast<int>(std::lround(y1)), minY, maxY);
        outX2 = std::clamp(static_cast<int>(std::lround(x2)), 0, maxX);
        outY2 = std::clamp(static_cast<int>(std::lround(y2)), minY, maxY);
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
        nx = x1 + dx * (minY - y1) / dy;
        ny = minY;
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
        c1 = outcode(x1, y1, maxX, maxY, minY);
      } else {
        x2 = nx;
        y2 = ny;
        c2 = outcode(x2, y2, maxX, maxY, minY);
      }
    }
    return false;
  }

  GfxRenderer& renderer_;
  int minY_ = 0;
};
