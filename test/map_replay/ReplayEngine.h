#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "MapFollow.h"
#include "MapProjection.h"
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

  // Fills Result::events with one entry per packet. Off by default: a 1097-
  // packet run has no reason to carry a 1097-entry vector when only the
  // summary counts are wanted (every sweep cell, for instance).
  bool recordEvents = false;
};

// One packet's outcome, in replay order -- for --events, not the summary.
struct Event {
  int packetIndex = 0;
  // "skip" | "move" | "reanchor"
  const char* action = "";
  // "" for skip/move; "heading" | "budget" | "keep-in" for reanchor.
  const char* reason = "";
  int16_t x = 0;
  int16_t y = 0;
  // partialMoves at the moment this was decided (before any reset).
  int movesIn = 0;
  // The frame's own heading at decision time (before a reanchor's reset) --
  // what "up" meant on screen for this fix. A fix drawn at (x, y) pointing
  // (fixHeadingStep - anchorHeadingStep) steps off "up" is where the rider
  // actually was and faced, whether or not the device redrew or moved its
  // own marker to show it (tools/render_ride_video.py's --track mode).
  uint8_t anchorHeadingStep = 0;
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
  // Re-anchors where the after-the-fact classification disagreed with the
  // branch decide() actually took. Non-zero means the reason column in every
  // table built from the old classifier is wrong for that many rows -- see
  // MapFollow::Reason.
  int reasonMismatches = 0;
  // Every packet's outcome, in order. Empty unless Config::recordEvents.
  std::vector<Event> events;
};

// The same state machine, one packet at a time, for a consumer that has to do
// something between packets -- draw the frame (test/map_window), or wait for
// the wall clock to catch up with the ride.
//
// `replay()` below is nothing but this class in a loop. That is deliberate:
// there is one copy of MapActivity's follow state, so the hardware baseline in
// hardware-baseline.txt gates the window exactly as it gates the summary. A
// second implementation for the window would look right and drift.
class Stepper {
 public:
  explicit Stepper(const Config& config);

  // What this packet did, plus what a consumer needs to draw it.
  struct Step {
    // "init" | "skip" | "move" | "reanchor" -- same strings as Event::action.
    const char* action = "";
    // "" for init/skip/move; "heading" | "budget" | "keep-in" for reanchor.
    //
    // Classified after the fact, the way tools/replay_ride.py does, so the
    // summary columns stay comparable with hardware-baseline.txt. Where the
    // true branch matters, read `trueReason` instead.
    const char* reason = "";
    // The branch decide() actually took (MapFollow::Reason). Can disagree with
    // `reason` above: decide() tests heading before keep-in, the classifier
    // tests keep-in first, so a fix that satisfies both is labelled differently
    // by the two. Result::reasonMismatches counts those.
    MapFollow::Reason trueReason = MapFollow::Reason::None;
    // The map module's own log line for this decision, formatted by
    // MapFollow::formatDecisionLog() -- the same bytes the device's LOG_DBG
    // prints. Empty for "init", which decide() never sees.
    char log[160] = {0};
    // Where the marker is now. For a reanchor this is the post-reset anchor,
    // not the fix projected through the frame that was just thrown away --
    // same choice, and same reason, as Event's x/y.
    int16_t x = 0;
    int16_t y = 0;
    // partialMoves at decision time, before any reset.
    int movesIn = 0;
    // The heading "up" meant on screen when this was decided.
    uint8_t anchorHeadingStep = 0;
    // Which way the marker glyph points now: the fix's heading relative to the
    // frame's, which is 0 on a reanchor by construction.
    uint8_t markerHeadingStep = 0;
    // Set on init and reanchor: the whole picture changed, and these three are
    // the view to render. Clear on skip and move -- the frame still stands.
    bool frameChanged = false;
    double frameLat = 0.0;
    double frameLon = 0.0;
    uint8_t frameHeadingStep = 0;
  };

  Step step(const RideLog::Packet& packet);

  // A viewport reset the rider asked for rather than one decide() called for:
  // a ladder step, a mode switch, a Refresh (MapActivity.h, "A viewport reset").
  // Re-anchors on the last fix and zeroes the partial-move budget, exactly what
  // renderViewport() does when a menu action triggers it.
  //
  // Deliberately NOT counted in Result: that structure mirrors what the device
  // logs and what hardware-baseline.txt gates, and those are decisions. A
  // consumer that cares about panel cost counts this itself -- it is a full
  // refresh like any other.
  //
  // False before the first packet: there is no fix to re-anchor on yet.
  bool reAnchorOnLastFix(Step& out);

  // A ladder step. Changes what the next frame is projected at; the caller is
  // expected to follow it with reAnchorOnLastFix(), same as the device, where a
  // step is a full-screen refresh (MapRideMode.h).
  void setZoomStep(int zoomStep);

  // The marker ladder's rung, i.e. the anchor row. A mode switch moves this on
  // the device too (kDefaultMarkerStepForMode), and each mode remembers its own.
  void setMarkerStep(int markerStep);

  // Counts so far. Complete only once every packet has been stepped.
  const Result& result() const { return result_; }

  // Only worth calling when Config::recordEvents is set and the packet count is
  // known up front -- replay() does, a live window does not.
  void reserveEvents(std::size_t packets);

 private:
  void renderViewport(double lat, double lon, uint8_t headingStep);

  Config config_;
  Result result_;

  // MapActivity's follow state (MapActivity.h's proj_, anchorHeading_,
  // markerDrawnX_/Y_, partialMoves_).
  MapProjection proj_;
  bool viewportDrawn_ = false;
  uint8_t anchorHeading_ = 0;
  int16_t markerDrawnX_ = 0;
  int16_t markerDrawnY_ = 0;
  uint16_t partialMoves_ = 0;

  // Off the ladders, not off a literal -- MapActivity.cpp:1830 and
  // MapViewport.h:kAnchorScreenX, which reads the compiled style.
  int16_t markerY_ = 0;
  int16_t anchorX_ = 0;

  // The last fix stepped, rounded exactly as step() rounds it. What a
  // rider-requested reset re-anchors on -- the device has no other position to
  // use either.
  double lastLat_ = 0.0;
  double lastLon_ = 0.0;
  uint8_t lastHeadingStep_ = 0;

  int packetIndex_ = 0;
};

Result replay(const std::vector<RideLog::Packet>& packets, const Config& config);

}  // namespace ReplayEngine
