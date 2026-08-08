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

Result replay(const std::vector<RideLog::Packet>& packets, const Config& config) {
  Result result;
  result.packets = static_cast<int>(packets.size());

  // MapActivity's follow state (MapActivity.h's proj_, anchorHeading_,
  // markerDrawnX_/Y_, partialMoves_).
  MapProjection proj;
  bool viewportDrawn = false;
  uint8_t anchorHeading = 0;
  int16_t markerDrawnX = 0;
  int16_t markerDrawnY = 0;
  uint16_t partialMoves = 0;

  // Both come off the ladders, not off a literal -- MapActivity.cpp:1830 and
  // MapViewport.h:kAnchorScreenX, which reads the compiled style.
  const int16_t markerY = MapViewport::markerYForStep(config.markerStep);
  const int16_t anchorX = MapViewport::kAnchorScreenX;

  // renderViewport()'s three state effects (MapActivity.cpp:1850, 2001-2004).
  // No route is loaded on any of these rides, so frameHeadingFor() is the
  // identity (MapActivity.cpp:1799-1801) and the fix's own heading is "up".
  const auto renderViewport = [&](double lat, double lon, uint8_t headingStep) {
    proj.reset(lat, lon, anchorX, markerY, headingStep, MapViewport::mppMercFor(config.zoomStep, lat));
    anchorHeading = headingStep;
    markerDrawnX = anchorX;
    markerDrawnY = markerY;
    partialMoves = 0;
    viewportDrawn = true;
  };

  for (const RideLog::Packet& packet : packets) {
    const double lat = static_cast<double>(toE7(packet.lat, config.coordDecimals)) / 1e7;
    const double lon = static_cast<double>(toE7(packet.lon, config.coordDecimals)) / 1e7;

    if (!viewportDrawn) {
      // MapActivity.cpp:1550-1553: no followable frame yet, so this fix builds
      // one and decide() is never asked. Not counted as a re-anchor -- the
      // device logs nothing for it either, so the hardware baseline does not
      // count it (tools/replay_ride.py:176).
      renderViewport(lat, lon, packet.headingStep);
      continue;
    }

    double mercX = 0.0, mercY = 0.0;
    MapProjection::lonLatToMerc(lat, lon, mercX, mercY);
    int16_t fixX = 0, fixY = 0;
    proj.projectMerc(mercX, mercY, fixX, fixY);

    MapFollow::Request request;
    request.fixX = fixX;
    request.fixY = fixY;
    request.drawnX = markerDrawnX;
    request.drawnY = markerDrawnY;
    request.screenWidth = config.screenWidth;
    request.screenHeight = config.screenHeight;
    request.anchorHeadingStep = anchorHeading;
    request.fixHeadingStep = packet.headingStep;
    request.partialMoves = partialMoves;
    request.partialMoveBudget = config.partialMoveBudget;
    request.routeHoldsFrame = false;  // ride mode, no route on any of these logs
    request.headingDriftLimitSteps = config.headingDriftLimitSteps;
    request.minPartialMovesForHeadingReAnchor = config.minPartialMovesForHeadingReAnchor;

    switch (MapFollow::decide(request)) {
      case MapFollow::Action::Skip:
        ++result.skips;
        break;
      case MapFollow::Action::MoveMarker:
        ++result.moves;
        markerDrawnX = fixX;
        markerDrawnY = fixY;
        ++partialMoves;
        break;
      case MapFollow::Action::ReAnchor: {
        ++result.reAnchors;
        // Classified from the state decide() was asked with, before the reset.
        const bool inside = MapFollow::insideKeepIn(fixX, fixY, config.screenWidth, config.screenHeight);
        if (!inside) {
          ++result.keepInAnchors;
        } else if (partialMoves >= config.partialMoveBudget) {
          ++result.budgetAnchors;
        } else {
          ++result.headingAnchors;
          result.headingMovesIn.push_back(static_cast<int>(partialMoves));
          if (partialMoves <= 1) ++result.thrashAnchors;
        }
        renderViewport(lat, lon, packet.headingStep);
        break;
      }
    }
  }

  return result;
}

}  // namespace ReplayEngine
