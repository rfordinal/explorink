#pragma once

#include <cstdint>

// Flat tone for an area fill, as a screen-space pixel pattern.
//
// This is the 1-bit answer to "shade the built-up bits". A pattern with a short
// period reads as a uniform grey at arm's length, while hatch lines read as
// scratches -- and at map zooms a building is a handful of pixels across, far too
// small to carry a line pattern at all.
//
// **Anchored in screen space, never to the shape.** That is what makes two
// buildings a metre apart share one tone instead of each starting its own
// pattern, which is the difference between a village reading as a built-up area
// and as noise.
//
// Four of the values are fixed patterns. Everything else in the range is a **dot
// grid with its period packed into the value**: one inked pixel per period x
// period cell, optionally with alternate dot rows offset by half a period. That
// packing is what lets a style name a density directly -- `tone: dots,
// tone_period_px: 5` -- without the tone growing a second field that every
// signature taking a tone would have to carry (IMapCanvas::fillSpan, MapAreaFill,
// six fields of MapStyle).
enum class MapAreaTone : uint8_t {
  None = 0,   // draw no fill at all
  Light = 1,  // 1 pixel in 4, GfxRenderer's own LightGray dither
  Dark = 2,   // 1 pixel in 2, checkerboard, GfxRenderer's DarkGray
  Solid = 3,  // every pixel -- for a shape too small to carry a texture

  // Dot grids: kDotsBase + period * 2 + stagger, period 2..15.
  kDotsBase = 16,
  Dense = 20,           // period 2, 1 in 4 -- same density as Light, different phase
  DenseStagger = 21,
  Stipple = 22,         // period 3, 1 in 9
  StippleStagger = 23,
  Micro = 24,           // period 4, 1 in 16 -- the lightest that still reads as a surface
  MicroStagger = 25,
};

namespace MapTone {

inline constexpr int kMinDotPeriod = 2;
inline constexpr int kMaxDotPeriod = 15;

// The tone for a dot grid of this period. `stagger` offsets alternate dot rows by
// half the period, which breaks the vertical alignment of a perfectly regular
// grid while keeping the density identical.
//
// **Which of the two is better is a panel question and is open.** A regular grid
// is clean and is also the thing that can beat against the panel's own pixel
// structure, or against the period of a hatch or a neighbouring area's dots
// (maintainer's call, 2026-08-26).
constexpr MapAreaTone dots(const int period, const bool stagger) {
  const int clamped = period < kMinDotPeriod ? kMinDotPeriod : (period > kMaxDotPeriod ? kMaxDotPeriod : period);
  return static_cast<MapAreaTone>(static_cast<uint8_t>(MapAreaTone::kDotsBase) + clamped * 2 + (stagger ? 1 : 0));
}

constexpr bool isDots(const MapAreaTone tone) {
  return static_cast<uint8_t>(tone) >= static_cast<uint8_t>(MapAreaTone::kDotsBase) + kMinDotPeriod * 2;
}

// Period of the dot grid, or 0 for a tone that is not one. A caller filling a
// span uses this to skip whole rows and to step along x by the period instead of
// testing every pixel: at 1 in 16 that is one write per sixteen columns rather
// than sixteen tests.
constexpr int dotPeriod(const MapAreaTone tone) {
  if (!isDots(tone)) return 0;
  return (static_cast<uint8_t>(tone) - static_cast<uint8_t>(MapAreaTone::kDotsBase)) / 2;
}

constexpr bool isStaggered(const MapAreaTone tone) {
  return isDots(tone) && ((static_cast<uint8_t>(tone) - static_cast<uint8_t>(MapAreaTone::kDotsBase)) & 1) != 0;
}

static_assert(dotPeriod(MapAreaTone::Stipple) == 3, "Stipple is a period-3 dot grid");
static_assert(dotPeriod(MapAreaTone::Micro) == 4, "Micro is a period-4 dot grid");
static_assert(isStaggered(MapAreaTone::MicroStagger), "MicroStagger staggers");
static_assert(!isStaggered(MapAreaTone::Micro), "Micro does not");
static_assert(dots(4, false) == MapAreaTone::Micro, "dots() and the named values agree");
static_assert(dots(3, true) == MapAreaTone::StippleStagger, "dots() and the named values agree");
static_assert(!isDots(MapAreaTone::Solid), "the fixed patterns are not dot grids");

// The x offset of this row's dots, 0 or half the period.
constexpr int rowOffset(const int y, const MapAreaTone tone) {
  const int period = dotPeriod(tone);
  if (period == 0 || !isStaggered(tone)) return 0;
  return ((y / period) & 1) * (period / 2);
}

// Whether this pixel is inked for `tone`. The two period-2 patterns mirror
// GfxRenderer's dither specialisations exactly (GfxRenderer.cpp,
// drawPixelDither<Color::LightGray> and <Color::DarkGray>); if those ever change,
// change these with them or the laptop preview stops matching the panel.
inline bool inkAt(const int x, const int y, const MapAreaTone tone) {
  const int period = dotPeriod(tone);
  if (period > 0) {
    if ((y % period) != 0) return false;
    return (((x + rowOffset(y, tone)) % period) == 0);
  }
  switch (tone) {
    case MapAreaTone::Solid:
      return true;
    case MapAreaTone::Dark:
      return ((x + y) & 1) == 0;
    case MapAreaTone::Light:
      return (x & 1) == 0 && (y & 1) == 0;
    default:
      break;
  }
  return false;
}

// True when the device can paint this tone with GfxRenderer's own dithered fill,
// i.e. one call per span instead of one per pixel.
inline bool hasNativeDither(const MapAreaTone tone) {
  return tone == MapAreaTone::Light || tone == MapAreaTone::Dark || tone == MapAreaTone::Solid;
}

// First inked x at or after `x`, on a row that carries dots. Returns x unchanged
// for a tone with no dot grid.
inline int firstInkedX(const int x, const int y, const MapAreaTone tone) {
  const int period = dotPeriod(tone);
  if (period == 0) return x;
  const int offset = rowOffset(y, tone);
  const int mod = ((x + offset) % period + period) % period;
  return mod == 0 ? x : x + (period - mod);
}

}  // namespace MapTone
