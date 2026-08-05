#pragma once

#include <cstdint>

// Flat tone for an area fill, as a screen-space pixel pattern.
//
// This is the 1-bit answer to "shade the built-up bits". A pattern with a
// period of two pixels reads as a uniform grey at arm's length, while hatch
// lines read as scratches -- and at map zooms a building is a handful of pixels
// across, far too small to carry a line pattern at all.
//
// The patterns are GfxRenderer's own, not new ones: `Color::LightGray` is
// `x % 2 == 0 && y % 2 == 0` and `Color::DarkGray` is `(x + y) % 2 == 0`
// (GfxRenderer.cpp, drawPixelDither specialisations). Reusing them means the
// device fill can go straight through `fillRectDither` per span, and the two
// canvases cannot disagree about the phase.
//
// **Anchored in screen space, never to the shape.** That is what makes two
// buildings a metre apart share one tone instead of each starting its own
// pattern, which is the difference between a village reading as a built-up area
// and as noise.
enum class MapAreaTone : uint8_t {
  None = 0,  // draw no fill at all
  Stipple,   // 1 pixel in 9 -- a fine dotted texture, the lightest readable tone
  Light,     // 1 pixel in 4
  Dark,      // 1 pixel in 2, checkerboard
  Solid,     // every pixel -- for a shape too small to carry a texture
};

namespace MapTone {

// Whether this pixel is inked for `tone`. Mirrors GfxRenderer's dither
// specialisations exactly (GfxRenderer.cpp, drawPixelDither<Color::LightGray>
// and <Color::DarkGray>); if those ever change, change this with them or the
// laptop preview stops matching the panel.
inline bool inkAt(const int x, const int y, const MapAreaTone tone) {
  switch (tone) {
    case MapAreaTone::Solid:
      return true;
    case MapAreaTone::Dark:
      return ((x + y) & 1) == 0;
    case MapAreaTone::Light:
      return (x & 1) == 0 && (y & 1) == 0;
    case MapAreaTone::Stipple:
      // Period 3, so it does not line up with the period-2 tones above and
      // cannot be mistaken for a faded one. GfxRenderer has no equivalent, so
      // this one is drawn pixel by pixel on the device rather than through
      // fillRectDither -- see GfxRendererCanvas::fillSpan.
      return (x % 3) == 0 && (y % 3) == 0;
    case MapAreaTone::None:
      break;
  }
  return false;
}

// True when the device can paint this tone with GfxRenderer's own dithered
// fill, i.e. one call per span instead of one per pixel.
inline bool hasNativeDither(const MapAreaTone tone) {
  return tone == MapAreaTone::Light || tone == MapAreaTone::Dark || tone == MapAreaTone::Solid;
}

}  // namespace MapTone
