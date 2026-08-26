#pragma once

#include <cstdint>

#include "MapAreaTone.h"

// Which of the panel's two values to write. There is no third: the map is drawn
// on 1-bit e-ink, and any "grey" here would be a dither pattern, which costs
// exactly the contrast this screen exists to keep (docs/map-render-spec.md,
// "1-bit rules").
//
// White is not a no-op. A road casing is a black stroke with a narrower white
// stroke inside it, and the position puck is a white disc under a black ring --
// both paint white over black that is already on the canvas.
enum class MapInk : uint8_t { Black, White };

// Quarter turns, and only quarter turns.
//
// A contour's height number should read with its top pointing uphill, which is
// what lets the slope direction be read off the number alone on a paper map. The
// bearing of a contour is any angle, so the honest thing would be to rotate the
// glyphs by that angle -- and at a 12 px line on a 1-bit grid that is the wrong
// trade: a smoothly rotated bitmap glyph is either holed (forward mapping) or
// blurred into a two-value grid it cannot represent (inverse mapping with any
// filtering). A quarter turn is an exact integer remap, so the digits stay as
// crisp as they are upright, and the reader still gets the up direction to
// within 45 degrees, which is all the number has to say.
//
// The turn is applied about the text's own centre, so a caller that knows where
// the number goes does not also have to know how wide it came out.
enum class MapTextTurn : uint8_t {
  None = 0,   // up is screen up
  Cw90 = 1,   // up points right
  Half = 2,   // up points down
  Ccw90 = 3,  // up points left
};

// Minimal drawing surface MapRenderer needs. Keeps MapRenderer free of any
// HAL/GfxRenderer dependency, so the exact same drawing logic (what to draw,
// where) runs both in the native preview (test/map_preview/PpmCanvas) and the
// real firmware (MapActivity's GfxRendererCanvas adapter, wrapping the real
// GfxRenderer) -- only this thin adapter differs between the two.
//
// `ink` is deliberately not defaulted. A default argument on a virtual binds
// statically, so an override could disagree with the interface about what "no
// colour given" means -- and on a surface where white is a real operation that
// is a wrong picture, not a compile error.
class IMapCanvas {
 public:
  virtual ~IMapCanvas() = default;

  virtual void drawLine(int x1, int y1, int x2, int y2, int lineWidth, MapInk ink) = 0;
  virtual void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, MapInk ink) = 0;
  virtual void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, MapInk ink) = 0;

  // One horizontal run of an area fill, x1..x2 inclusive, painted with `tone`
  // (MapAreaTone.h). A span rather than a rectangle because the shapes being
  // filled are arbitrary rings, and a span rather than a pixel because the
  // device can hand a whole run to GfxRenderer's dithered fill in one call.
  //
  // The tone's pattern is anchored to screen coordinates, never to the span, so
  // two shapes side by side share one texture.
  virtual void fillSpan(int x1, int x2, int y, MapAreaTone tone) = 0;

  // Text -- the one primitive with a font behind it, and the only one whose two
  // implementations cannot promise the same pixels: the device draws through
  // GfxRenderer's real EpdFont, the preview through its own host rasterizer of
  // the same font data (test/map_preview/PreviewFont.h). Metrics come from the
  // same font tables in both, so a label that fits here fits there.
  //
  // `sizePx` is the nominal size the style asked for (mapstyle.json's
  // `label_px`), not a guarantee: an implementation snaps it to a font it
  // actually has and reports what it got through measureText. Layout must
  // therefore always use the measured box, never sizePx.
  //
  // `x`, `y` is the TOP-LEFT of the measured box, not a baseline -- the label
  // placer works in boxes and nothing above this line knows what an ascender
  // is.
  //
  // measureText returns false when this canvas has no text at all, and the
  // renderer then draws dots and no labels. It must not draw anything.
  virtual bool measureText(const char* utf8, int sizePx, bool bold, int& outWidth, int& outHeight) = 0;
  virtual void drawText(int x, int y, const char* utf8, int sizePx, bool bold, MapInk ink) = 0;

  // The same text, quarter-turned about (centreX, centreY) -- see MapTextTurn.
  // `MapTextTurn::None` must draw exactly what drawText would draw with the box
  // centred on that point, so a caller can use this unconditionally.
  virtual void drawTextTurned(int centreX, int centreY, const char* utf8, int sizePx, bool bold, MapInk ink,
                             MapTextTurn turn) = 0;

  // The rectangle this canvas will actually accept ink in, in screen pixels.
  //
  // Every other primitive here is fire-and-forget: the canvas clips, and a
  // road half off screen is still a correct road. Labels are the first thing
  // that has to *decide* where to draw, and a label clipped in half says the
  // wrong name -- so the placer needs the bounds before it commits. On the
  // device this is narrower than the panel: the header band is off limits
  // (docs/map-header-status.md).
  virtual void drawableRect(int& outX, int& outY, int& outWidth, int& outHeight) const = 0;
};
