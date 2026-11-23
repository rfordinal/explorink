#pragma once

#include <BoardConfig.h>
#include <HalGPIO.h>

// Where the on-screen button hint boxes sit on a panel that is not an X4 or an X3.
//
// The X4 and X3 position arrays in each theme are hand-tuned to the physical
// keys under the glass -- the box has to sit above the key it names, so those
// numbers are never derived from anything. Every other panel has no keys under
// the boxes at all (T5 S3 Pro has none; X4 Pro has two on the side), so there
// the boxes are the buttons, and the only requirement is that they land on the
// screen in the same arrangement. Scaling the X4's 480x800 portrait layout does
// that for any panel size and keeps one set of numbers to maintain.
namespace HintGeometry {

// The X4's portrait screen, the reference every scaled layout comes from.
constexpr int kRefWidth = 480;
constexpr int kRefHeight = 800;

inline int scaleX(int v, int screenWidth) { return v * screenWidth / kRefWidth; }
inline int scaleY(int v, int screenHeight) { return v * screenHeight / kRefHeight; }

// True when this panel needs the scaled layout rather than a hand-tuned array.
inline bool scaled(int screenWidth) { return !gpio.deviceIsX3() && screenWidth != kRefWidth; }

// Fill out[0..3] with the left edge of each front hint box and return the box
// width. x4Positions/x3Positions/refBoxWidth are the theme's own numbers.
inline int frontRow(int screenWidth, const int* x4Positions, const int* x3Positions, int refBoxWidth, int out[4]) {
  const int* source = gpio.deviceIsX3() ? x3Positions : x4Positions;
  if (!scaled(screenWidth)) {
    for (int i = 0; i < 4; i++) out[i] = source[i];
    return refBoxWidth;
  }
  for (int i = 0; i < 4; i++) out[i] = scaleX(x4Positions[i], screenWidth);
  return scaleX(refBoxWidth, screenWidth);
}

// The panel in portrait logical coordinates. GfxRenderer derives the same pair
// from the panel dimensions (portrait width is the panel's short side), but
// ThemeMetrics is a singleton with no renderer to ask, so it reads the board
// profile directly.
inline int portraitWidth() { return BoardConfig::ACTIVE.displayHeight; }
inline int portraitHeight() { return BoardConfig::ACTIVE.displayWidth; }

// Scale a theme metric written against the X4's 480px-wide portrait screen.
// A no-op on the X4 and the X3, whose metrics are the hand-tuned originals.
inline int scaleMetric(int v) { return scaled(portraitWidth()) ? scaleX(v, portraitWidth()) : v; }

// The vertical counterpart. A box's Y follows the taller axis, which on a panel
// with a different aspect ratio is not the same factor as its X.
inline int scaleMetricY(int v) { return scaled(portraitWidth()) ? scaleY(v, portraitHeight()) : v; }

}  // namespace HintGeometry
