#include "MapFollow.h"

namespace MapFollow {

uint8_t headingDriftSteps(uint8_t a, uint8_t b) {
  const uint8_t diff = static_cast<uint8_t>((a - b) & 0x0F);
  return diff <= 8 ? diff : static_cast<uint8_t>(16 - diff);
}

uint8_t relativeHeadingStep(uint8_t fixHeadingStep, uint8_t anchorHeadingStep) {
  return static_cast<uint8_t>((fixHeadingStep - anchorHeadingStep) & 0x0F);
}

bool insideKeepIn(int16_t x, int16_t y, int16_t screenWidth, int16_t screenHeight) {
  return x >= kKeepInMarginPx && y >= kKeepInMarginPx && x < screenWidth - kKeepInMarginPx &&
         y < screenHeight - kKeepInMarginPx;
}

Action decide(const Request& request) {
  // Order matters. Every reason to redraw the whole frame is checked before the
  // movement floor, so a fix that is barely moving but has turned the rider (or
  // has run out of ghosting budget) still gets its redraw instead of being
  // skipped as "close enough".
  //
  // With a route holding the frame there is no heading check at all: the frame
  // is the route's, so the rider turning is not news about the picture, only
  // about the arrow drawn inside it (Request::routeHoldsFrame).
  if (!request.routeHoldsFrame && request.partialMoves >= kMinPartialMovesForHeadingReAnchor &&
      headingDriftSteps(request.fixHeadingStep, request.anchorHeadingStep) >= kMaxHeadingDriftSteps) {
    return Action::ReAnchor;
  }
  if (!insideKeepIn(request.fixX, request.fixY, request.screenWidth, request.screenHeight)) {
    return Action::ReAnchor;
  }
  if (request.partialMoves >= request.partialMoveBudget) {
    return Action::ReAnchor;
  }

  const int dx = request.fixX - request.drawnX;
  const int dy = request.fixY - request.drawnY;
  const int absDx = dx < 0 ? -dx : dx;
  const int absDy = dy < 0 ? -dy : dy;
  if (absDx < kMinMovePx && absDy < kMinMovePx) {
    return Action::Skip;
  }
  return Action::MoveMarker;
}

}  // namespace MapFollow
