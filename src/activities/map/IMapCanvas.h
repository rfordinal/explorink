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

  // The same text, centred on (centreX, centreY) and rotated freely.
  //
  // The rotation is given as an orthonormal basis in 1/1024ths -- where the
  // text's own +x (reading direction) and +y (down the glyphs) end up on screen.
  // A caller that has a direction vector already has this; a caller that has an
  // angle does not want to hand a float to a device with no FPU.
  //
  // `outline` non-zero draws the glyphs grown by one pixel in the OPPOSITE ink
  // first, so the text keeps a 1 px halo without the caller drawing it eight more
  // times. That is where a rotated label needs it most: it sits on the line it
  // names, at that line's angle.
  //
  // Both implementations rasterise into a MapTextMask and share one rotation
  // (MapTextMask.h), so the preview and the panel are the same arithmetic and not
  // merely the same intent.
  //
  // **Returns whether the text was actually inked**, and the caller has to look:
  // the mask has a fixed ceiling, so a string too large for it draws nothing, and
  // a halo too large draws the number with no outline -- black digits straight
  // onto the black line they name, which is the illegibility this call exists to
  // prevent. Both used to be silent while the caller still counted the label as
  // placed and marked its ground taken, so a phantom number blocked a place name
  // off ground nothing occupied.
  virtual bool drawTextRotated(int centreX, int centreY, const char* utf8, int sizePx, bool bold, MapInk ink,
                               int outline, int rightX, int rightY, int downX, int downY) = 0;

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
