#include "ReplayEngine.h"

#include <cmath>

#include "MapProjection.h"
#include "MapViewport.h"

namespace ReplayEngine {
namespace {

// Degrees -> the int32 1e7 fixed point everything downstream of the BLE packet
// carries (MapCommandParser.h:47-50), rounded first to the decimal places the
// caller says the fix arrived with.
int32_t toE7(double degrees, int decimals) {
  const double scale = std::pow(10.0, decimals);
  const double rounded = std::round(degrees * scale) / scale;
  return static_cast<int32_t>(std::llround(rounded * 1e7));
}

}  // namespace

Stepper::Stepper(const Config& config)
    : config_(config),
      markerY_(MapViewport::markerYForStep(config.markerStep)),
      anchorX_(MapViewport::kAnchorScreenX) {}

void Stepper::reserveEvents(std::size_t packets) { result_.events.reserve(packets); }

// renderViewport()'s three state effects (MapActivity.cpp:1850, 2001-2004).
// No route is loaded on any of these rides, so frameHeadingFor() is the
// identity (MapActivity.cpp:1799-1801) and the fix's own heading is "up".
void Stepper::renderViewport(double lat, double lon, uint8_t headingStep) {
  proj_.reset(lat, lon, anchorX_, markerY_, headingStep, MapViewport::mppMercFor(config_.zoomStep, lat));
  anchorHeading_ = headingStep;
  markerDrawnX_ = anchorX_;
  markerDrawnY_ = markerY_;
  partialMoves_ = 0;
  viewportDrawn_ = true;
}

Stepper::Step Stepper::step(const RideLog::Packet& packet) {
  const int packetIndex = packetIndex_++;
  ++result_.packets;

  const double lat = static_cast<double>(toE7(packet.lat, config_.coordDecimals)) / 1e7;
  const double lon = static_cast<double>(toE7(packet.lon, config_.coordDecimals)) / 1e7;

  Step out;

  if (!viewportDrawn_) {
    // MapActivity.cpp:1550-1553: no followable frame yet, so this fix builds
    // one and decide() is never asked. Not counted as a re-anchor -- the
    // device logs nothing for it either, so the hardware baseline does not
    // count it (tools/replay_ride.py:176), and runFrames() filters this "init"
    // action out for the same reason. It still needs an event of its own,
    // action != "reanchor" so no summary counter moves, purely so a per-packet
    // consumer (--track) has a real anchor to render its very first background
    // from instead of guessing.
    renderViewport(lat, lon, packet.headingStep);
    if (config_.recordEvents) {
      result_.events.push_back({packetIndex, "init", "", anchorX_, markerY_, 0, packet.headingStep});
    }
    out.action = "init";
    out.x = anchorX_;
    out.y = markerY_;
    out.anchorHeadingStep = packet.headingStep;
    out.frameChanged = true;
    out.frameLat = lat;
    out.frameLon = lon;
    out.frameHeadingStep = packet.headingStep;
    return out;
  }

  double mercX = 0.0, mercY = 0.0;
  MapProjection::lonLatToMerc(lat, lon, mercX, mercY);
  int16_t fixX = 0, fixY = 0;
  proj_.projectMerc(mercX, mercY, fixX, fixY);

  MapFollow::Request request;
  request.fixX = fixX;
  request.fixY = fixY;
  request.drawnX = markerDrawnX_;
  request.drawnY = markerDrawnY_;
  request.screenWidth = config_.screenWidth;
  request.screenHeight = config_.screenHeight;
  request.anchorHeadingStep = anchorHeading_;
  request.fixHeadingStep = packet.headingStep;
  request.partialMoves = partialMoves_;
  request.partialMoveBudget = config_.partialMoveBudget;
  request.routeHoldsFrame = false;  // ride mode, no route on any of these logs
  request.headingDriftLimitSteps = config_.headingDriftLimitSteps;
  request.minPartialMovesForHeadingReAnchor = config_.minPartialMovesForHeadingReAnchor;

  const int movesInBefore = static_cast<int>(partialMoves_);
  const uint8_t anchorHeadingAtDecision = anchorHeading_;

  out.movesIn = movesInBefore;
  out.anchorHeadingStep = anchorHeadingAtDecision;
  // A fix drawn at (x, y) pointing (fixHeadingStep - anchorHeadingStep) steps
  // off "up" is where the rider actually was and faced -- the frame is
  // track-up, so the glyph carries only the difference.
  out.markerHeadingStep = static_cast<uint8_t>((packet.headingStep - anchorHeadingAtDecision) & 0x0F);

  switch (MapFollow::decide(request)) {
    case MapFollow::Action::Skip:
      ++result_.skips;
      if (config_.recordEvents) {
        result_.events.push_back({packetIndex, "skip", "", fixX, fixY, movesInBefore, anchorHeadingAtDecision});
      }
      out.action = "skip";
      out.x = markerDrawnX_;
      out.y = markerDrawnY_;
      break;
    case MapFollow::Action::MoveMarker:
      ++result_.moves;
      markerDrawnX_ = fixX;
      markerDrawnY_ = fixY;
      ++partialMoves_;
      if (config_.recordEvents) {
        result_.events.push_back({packetIndex, "move", "", fixX, fixY, movesInBefore, anchorHeadingAtDecision});
      }
      out.action = "move";
      out.x = fixX;
      out.y = fixY;
      break;
    case MapFollow::Action::ReAnchor: {
      ++result_.reAnchors;
      // Classified from the state decide() was asked with, before the reset.
      const bool inside = MapFollow::insideKeepIn(fixX, fixY, config_.screenWidth, config_.screenHeight);
      const char* reason;
      if (!inside) {
        ++result_.keepInAnchors;
        reason = "keep-in";
      } else if (partialMoves_ >= config_.partialMoveBudget) {
        ++result_.budgetAnchors;
        reason = "budget";
      } else {
        ++result_.headingAnchors;
        result_.headingMovesIn.push_back(static_cast<int>(partialMoves_));
        if (partialMoves_ <= 1) ++result_.thrashAnchors;
        reason = "heading";
      }
      if (config_.recordEvents) {
        // Post-reset position, not fixX/fixY: those are the fix projected
        // through the *old* frame -- exactly the drift that triggered this
        // reset, not where anything lands in the new one. After
        // renderViewport() the marker is always back at (anchorX, markerY)
        // with the fix's own heading as "up" (relative drift 0 by
        // construction) -- record that, or a --track consumer draws the
        // dot at a position that has nothing to do with the frame it just
        // rendered (found by eye, 2026-08-08: the dot landed nowhere near
        // the marker on every reanchor with more than a couple of moves in).
        result_.events.push_back(
            {packetIndex, "reanchor", reason, anchorX_, markerY_, movesInBefore, packet.headingStep});
      }
      renderViewport(lat, lon, packet.headingStep);
      out.action = "reanchor";
      out.reason = reason;
      out.x = anchorX_;
      out.y = markerY_;
      out.anchorHeadingStep = packet.headingStep;
      out.markerHeadingStep = 0;  // track-up: after a reset the rider faces screen-up
      out.frameChanged = true;
      out.frameLat = lat;
      out.frameLon = lon;
      out.frameHeadingStep = packet.headingStep;
      break;
    }
  }

  return out;
}

Result replay(const std::vector<RideLog::Packet>& packets, const Config& config) {
  Stepper stepper(config);
  if (config.recordEvents) stepper.reserveEvents(packets.size());
  for (const RideLog::Packet& packet : packets) stepper.step(packet);
  return stepper.result();
}

}  // namespace ReplayEngine
