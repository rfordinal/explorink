#pragma once

#include <cstdint>

// The follow-the-marker policy: when a fresh fix may move the marker inside the
// map already on the panel, and when the map underneath has to be redrawn.
//
// A full viewport reset costs tile reads off the SD card, a full-screen
// MapRenderer pass and a whole-panel waveform -- the better part of two seconds
// and the single most expensive thing the map screen does. A fix that has moved
// the rider 20 metres does not need any of it: the map on the panel is still
// correct, only one 64x64 pixel patch of it is wrong. Moving the marker inside
// the existing frame and refreshing just that rectangle
// (GfxRenderer::displayBufferWindow) is the cheap case, and it is meant to be
// the common one.
//
// Everything here is integer screen arithmetic with no dependency on the
// renderer, the projection or the HAL, so it is unit-tested on the host
// (test/map_follow). MapActivity does the projecting and the drawing; this
// decides.
namespace MapFollow {

// How close to the screen edge the marker may come before the map is redrawn
// around it. One marker ring (54 px) plus slack: a marker inside this frame is
// always fully on screen, so the patch save/restore never straddles the edge,
// and there is still real map ahead of the rider to look at.
inline constexpr int16_t kKeepInMarginPx = 80;

// Heading change that forces a redraw on its own, in 22.5 degree steps. The map
// is drawn track-up, so the frame on the panel is only correct for the heading
// it was drawn with; a rider who has turned 90 degrees is looking at a map
// oriented for the road they left. Below this the marker's own arrow shows the
// turn (it is drawn relative to the frame's heading) and the map stays.
inline constexpr uint8_t kMaxHeadingDriftSteps = 4;

// Movement below this is not worth a waveform. At 6 m/px a fix every 10 m moves
// the marker under 2 px, and the Android bridge sends on distance, not on a
// timer (docs/android-install.md) -- without this floor a slow rider would pay
// a panel refresh for a marker that visibly did not move.
inline constexpr int16_t kMinMovePx = 8;

// Windowed refreshes are differential and leave ghosting behind. Force a clean
// full frame after this many moves -- docs/firmware-implementation-plan.md's
// open decision 4 ("every 10-20 marker updates, needs on-device tuning").
inline constexpr uint16_t kMaxPartialMoves = 12;

// The same budget for a frame the route holds, where a forced clean frame is the
// only thing left that can interrupt a leg (see Request::routeHoldsFrame).
//
// A leg costs about `leg length / (kMinMovePx * metres-per-pixel)` marker moves,
// so this number and the zoom rung are one choice: 160 covers the 3.4 km Baba
// pass at 3 m/px (142 moves) and anything shorter or coarser comfortably. A leg
// that would need more has to be drawn at a coarser rung rather than allowed to
// interrupt.
//
// **This number has NOT been checked against real ghosting, and cannot be from
// the host.** `CMD:SCREENSHOT` dumps the framebuffer, which is clean by
// construction; ghosting is what the panel does to it, so the only instrument is
// somebody looking at the panel. docs/route-navigation.md's open list says what
// to look at. If 160 windowed refreshes in a row turn out to smear, lower this
// and let the rung rule pick a coarser frame -- the mechanism does not change,
// only the number.
inline constexpr uint16_t kRouteFramePartialMoves = 160;

enum class Action : uint8_t {
  // The fix landed close enough to where the marker already is that nothing is
  // drawn and the panel is not touched at all.
  Skip,
  // Erase the marker from the frame, redraw it at the new spot, refresh only
  // the rectangles involved. No tile reads, no MapRenderer pass.
  MoveMarker,
  // Full viewport reset: new bbox around the fix, marker back at its ladder
  // anchor, map re-oriented to the new heading.
  ReAnchor,
};

struct Request {
  // Where the new fix projects to through the projection the frame on the panel
  // was drawn with.
  int16_t fixX = 0;
  int16_t fixY = 0;
  // Where the marker is drawn right now.
  int16_t drawnX = 0;
  int16_t drawnY = 0;
  int16_t screenWidth = 0;
  int16_t screenHeight = 0;
  // Heading the frame was drawn with, and the heading of the new fix.
  uint8_t anchorHeadingStep = 0;
  uint8_t fixHeadingStep = 0;
  // Marker moves since the last full frame, and how many are allowed before a
  // clean one is forced.
  uint16_t partialMoves = 0;
  uint16_t partialMoveBudget = kMaxPartialMoves;

  // True while a route owns the frame's orientation, which makes a heading
  // change no reason at all to redraw -- docs/route-navigation.md, "The
  // decision".
  //
  // The rider is following a planned route, so the frame is oriented to the
  // route and not to them. Their own direction is still on screen: the marker is
  // an arrow drawn with relativeHeadingStep() against this frame, so a rider who
  // turns 90 degrees gets an arrow 90 degrees off up inside a map that has not
  // moved. Turning the whole picture to say the same thing costs a full-panel
  // waveform and hands back a map the rider has to re-read.
  //
  // Measured on a switchback pass before this existed: three full rotations
  // across 3.4 km, the last of them back to the orientation the first one had.
  // On a road that reverses direction every 200 m no drift threshold helps,
  // because the road really does turn further than any threshold worth having.
  bool routeHoldsFrame = false;
};

// Shortest distance between two 16-step headings, in steps (0..8). Wraps: N and
// NNW are one step apart, not fifteen.
uint8_t headingDriftSteps(uint8_t a, uint8_t b);

// A heading expressed relative to the frame's own orientation, which is what
// the marker arrow is drawn with: 0 means "the way the map is facing", so a
// track-up frame drawn for this fix puts the arrow straight up.
uint8_t relativeHeadingStep(uint8_t fixHeadingStep, uint8_t anchorHeadingStep);

// True while the marker is far enough from every screen edge to keep following
// inside this frame.
bool insideKeepIn(int16_t x, int16_t y, int16_t screenWidth, int16_t screenHeight);

Action decide(const Request& request);

}  // namespace MapFollow
