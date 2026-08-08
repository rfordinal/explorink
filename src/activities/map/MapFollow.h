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

// Heading drift alone does not force a redraw until at least this many partial
// moves have landed since the last full frame.
//
// Real-ride evidence, 2026-08-07 (three recorded rides, replayed against this
// decision on real hardware -- docs/rides/trailink-gps-2026080{7-142303,
// 7-173058,7-201221}.jsonl in the parent repo): 29 of 52 heading-triggered
// re-anchors had 0 or 1 marker moves since the last frame -- the marker had
// barely moved when the heading swung past the limit. That is GPS heading
// noise at low speed or a stop, not a turn in progress: a rider who really is
// turning while riding crosses this floor within a couple of fixes, so a
// genuine turn is delayed by moments, not withheld.
//
// **Trade-off, accepted deliberately.** A rider who turns while genuinely
// standing still (parked, stopped at a junction) no longer gets an immediate
// re-anchor -- the map stays oriented for the direction they arrived from
// until they move enough to cross this floor. `MapFollowTest.cpp`'s
// `HeadingDriftReAnchorsEvenStandingStill` used to hold the opposite
// deliberately (see its history); this is the maintainer's call to trade that
// guarantee for killing the standing-still thrash, made with the ride data
// above in hand, not a rediscovery of the same idea the phone's
// `HeadingTrend` dwell already tried and measured worse (docs/map-follow.md,
// "Heading thrash, and why the fix is not in this firmware") -- that dwell
// judged whether a *turn* was real from a 5-fix trend; this gate does not
// judge the turn at all, it only requires the marker to have actually moved
// since the last frame, which is a fact already tracked here, not an
// inference about the rider.
//
// **Unverified**: the number 2 itself. It kills the exact thrash pattern
// measured (0-1 moves in) without being proven optimal -- on-device tuning,
// same as `kMaxPartialMoves`, per `docs/optimization/07-power-and-lifecycle.md`.
inline constexpr uint16_t kMinPartialMovesForHeadingReAnchor = 2;

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
// so this number and the zoom rung are one choice. 1000 is 48 km of leg at
// 6 m/px, or 24 km at 3 m/px -- chosen by the maintainer 2026-08-07 with hiking
// in mind, where a leg is long and slow and a redraw mid-leg is worth avoiding.
//
// **160 of these in a row is measured clean. 1000 is not.** The panel held 160
// consecutive windowed refreshes with no clean frame four times over during one
// probe run and showed no ghosting at all, judged by eye. It could not be pushed
// past 160, because the firmware forces a clean frame at exactly this constant --
// so raising it is an extrapolation from 160, not a measurement, however good the
// evidence at 160 looks.
//
// **And it cannot be measured from a host.** `CMD:SCREENSHOT` dumps the
// framebuffer, which is clean by construction; ghosting is what the panel does to
// a frame, so a dump of a badly ghosted panel looks perfect. The only instrument
// is somebody looking at the device.
//
// To settle 1000: set it, flash, and ride a leg at a rung where it fits well
// inside the keep-in box (6 m/px on a 3.4 km leg gives ~71 moves a lap, so ~14
// laps with no keep-in interruption), then look at the panel **at the end**. If it
// smears, lower this; the moves-per-leg formula then says which rung a leg of a
// given length has to be drawn at, and nothing else changes.
inline constexpr uint16_t kRouteFramePartialMoves = 1000;

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
  // The two heading thresholds, per request, defaulting to the constants above.
  //
  // Same pattern and the same reason as `partialMoveBudget`: a caller that has
  // a reason to ask a different question gets to ask it, and a caller that does
  // not writes nothing and gets the firmware's own numbers. MapActivity never
  // sets either (MapActivity.cpp:1565-1578), so device behaviour is exactly what
  // the constants say.
  //
  // What sets them is the host replay harness (test/map_replay), which walks a
  // recorded ride through this very function at several threshold values in one
  // run. Without these fields that sweep needs one firmware build per value,
  // which is how `route_follow_sim.py` (parent repo) ended up re-implementing
  // this logic in Python and drifting from it.
  //
  // Cost is 3 bytes on one stack-local Request per fix, no heap, no flash data.
  uint8_t headingDriftLimitSteps = kMaxHeadingDriftSteps;
  uint16_t minPartialMovesForHeadingReAnchor = kMinPartialMovesForHeadingReAnchor;

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
