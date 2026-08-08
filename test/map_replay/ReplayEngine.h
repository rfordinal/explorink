#pragma once

#include <cstdint>
#include <vector>

#include "MapFollow.h"
#include "RideLog.h"

// MapActivity's follow state machine, on the host, around the real
// MapFollow::decide().
//
// What is reproduced here, and where it lives on the device:
//
// - `applyFix()` (MapActivity.cpp:1534-1602) projects each new fix through the
//   projection the *currently drawn frame* was built with -- `proj_` is not
//   re-created per fix (MapActivity.cpp:1554-1562). That is the crux of the
//   whole follow design (docs/map-follow.md, "The heading decides the frame,
//   once") and the easiest thing to get wrong in a reimplementation.
// - `renderViewport()` (MapActivity.cpp:1803-1850, 2001-2004) resets the
//   projection with the fix's own heading as "up", puts the marker back on its
//   ladder anchor, and zeroes the partial-move budget.
// - `moveMarker()` (MapActivity.cpp:1524-1526) leaves the marker where the fix
//   landed and spends one partial move.
//
// What is NOT reproduced, because it is not a decision -- it is drawing:
// tile reads, the MapRenderer pass, the panel waveform, the marker patch. The
// tool answers "how often would the map be redrawn", not "what does it look
// like" and not "how long does it take".
//
// **Assumed, and the one place this can differ from the device**: every marker
// move is taken to succeed. On the device a rejected `displayBufferWindow()`
// falls back to a full refresh and zeroes `partialMoves_`
// (MapActivity.cpp:1512-1522). That path logs `marker window rejected`; it did
// not appear in the hardware runs this harness is gated against.
namespace ReplayEngine {

struct Config {
  int zoomStep = 4;    // MapViewport::kZoomLadder index; 4 = 20 m/px
  int markerStep = 2;  // MapViewport::kMarkerLadder index; what replay_ride.py sends
  int16_t screenWidth = 480;
  int16_t screenHeight = 800;

  // Decimal places the fix is rounded to before it becomes int32 1e7 fixed
  // point. 5 is what `tools/replay_ride.py:163` types at the map console
  // (`pos %.5f %.5f`), so 5 is what the hardware baseline was measured with.
  // 7 is what a real BLE packet carries (MapCommandParser.h:47-50).
  int coordDecimals = 5;

  // The three thresholds, defaulting to the firmware's own constants. Passed
  // straight into MapFollow::Request, which is what makes a sweep possible
  // without one build per value (MapFollow.h, Request::headingDriftLimitSteps).
  uint8_t headingDriftLimitSteps = MapFollow::kMaxHeadingDriftSteps;
  uint16_t minPartialMovesForHeadingReAnchor = MapFollow::kMinPartialMovesForHeadingReAnchor;
  uint16_t partialMoveBudget = MapFollow::kMaxPartialMoves;
};

struct Result {
  int packets = 0;
  int skips = 0;
  int moves = 0;
  int reAnchors = 0;
  // Why each re-anchor happened, classified exactly the way
  // tools/replay_ride.py:181-187 classifies the device's own log line: inside
  // the keep-in box with budget left means heading, outside means keep-in,
  // otherwise budget.
  int headingAnchors = 0;
  int keepInAnchors = 0;
  int budgetAnchors = 0;
  // Marker moves since the last full frame, one entry per heading re-anchor.
  std::vector<int> headingMovesIn;
  // Of those, how many had 0 or 1 moves in -- the thrash signal
  // (docs/map-follow.md, "Heading thrash, round two").
  int thrashAnchors = 0;
};

Result replay(const std::vector<RideLog::Packet>& packets, const Config& config);

}  // namespace ReplayEngine
