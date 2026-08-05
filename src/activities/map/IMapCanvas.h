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
};
