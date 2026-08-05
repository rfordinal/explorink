#pragma once

#include <cstdint>

// The grey encoding, on its own, with no renderer and no panel attached, so it
// can be reasoned about and unit-tested on the host. GrayscaleFrame.h wraps it
// in drawing calls; docs/eink-grayscale.md is the hardware mechanism.

// Panel levels, dark to light. Values are the encoder's own numbering
// (GfxRenderer.cpp:448: 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white)
// read the other way round, so the enum reads as "how much ink".
enum class GrayShade : uint8_t { White = 0, LightGray = 1, DarkGray = 2, Black = 3 };

// Base = the BW frame the panel starts from. Msb = "this pixel is some grey".
// Lsb = "and it is the darker grey". The names are the grey-level bit, not the
// RAM plane (docs/eink-grayscale.md, "Naming trap": LSB is BW RAM 0x24, MSB is
// RED RAM 0x26).
enum class GrayPass : uint8_t { Base, Lsb, Msb };

// Does `shade` lay down ink (base pass) or ask for a nudge (plane passes)?
//
// Base: black AND both greys are ink. A grey pixel is a black pixel the planes
// then lighten, which is why losing the grey reads black, not white.
// Msb: set for both greys -- it is the superset.
// Lsb: set for dark grey only, the stronger nudge.
constexpr bool grayInks(const GrayShade shade, const GrayPass pass) {
  switch (pass) {
    case GrayPass::Base:
      return shade != GrayShade::White;
    case GrayPass::Msb:
      return shade == GrayShade::LightGray || shade == GrayShade::DarkGray;
    case GrayPass::Lsb:
      return shade == GrayShade::DarkGray;
  }
  return false;
}

// The pixel state to draw `shade` with in `pass`. Framebuffer: true = black.
// Plane: a SET bit asks for the nudge and drawPixel sets the bit with state
// FALSE (GfxRenderer.cpp:452-457), so plane passes invert. Getting this
// backwards is silent: the panel nudges the complement of what was drawn.
constexpr bool grayPixelState(const GrayShade shade, const GrayPass pass) {
  return pass == GrayPass::Base ? grayInks(shade, pass) : !grayInks(shade, pass);
}

// The two plane bits as the controller sees them, for the LUT slot table in
// docs/eink-grayscale.md: black/white 00 (no drive), light grey 10, dark grey 11.
constexpr bool grayPlaneBit(const GrayShade shade, const GrayPass pass) { return !grayPixelState(shade, pass); }
