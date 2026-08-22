#include "MapFollow.h"

#include <cstdio>

namespace MapFollow {

uint8_t headingDriftSteps(uint8_t a, uint8_t b) {
  const uint8_t diff = static_cast<uint8_t>((a - b) & 0x0F);
  return diff <= 8 ? diff : static_cast<uint8_t>(16 - diff);
}

uint8_t relativeHeadingStep(uint8_t fixHeadingStep, uint8_t anchorHeadingStep) {
  return static_cast<uint8_t>((fixHeadingStep - anchorHeadingStep) & 0x0F);
}

bool insideKeepIn(int16_t x, int16_t y, int16_t screenWidth, int16_t screenHeight, int16_t marginPx) {
  return x >= marginPx && y >= marginPx && x < screenWidth - marginPx && y < screenHeight - marginPx;
}

const char* reasonName(Reason reason) {
  switch (reason) {
    case Reason::HeadingDrift:
      return "heading";
    case Reason::KeepIn:
      return "keep-in";
    case Reason::Budget:
      return "budget";
    case Reason::BelowMoveFloor:
      return "below the move floor";
    case Reason::Moved:
      return "moved";
    case Reason::None:
    default:
      return "";
  }
}

Action decide(const Request& request) {
  Reason ignored = Reason::None;
  return decide(request, ignored);
}

Action decide(const Request& request, Reason& outReason) {
  // Order matters. Every reason to redraw the whole frame is checked before the
  // movement floor, so a fix that is barely moving but has turned the rider (or
  // has run out of ghosting budget) still gets its redraw instead of being
  // skipped as "close enough".
  //
  // It also decides the reason when two checks would both fire, which is why
  // outReason is set here and not worked out afterwards -- see Reason.
  //
  // With a route holding the frame there is no heading check at all: the frame
  // is the route's, so the rider turning is not news about the picture, only
  // about the arrow drawn inside it (Request::routeHoldsFrame).
  if (!request.routeHoldsFrame && request.partialMoves >= request.minPartialMovesForHeadingReAnchor &&
      headingDriftSteps(request.fixHeadingStep, request.anchorHeadingStep) >= request.headingDriftLimitSteps) {
    outReason = Reason::HeadingDrift;
    return Action::ReAnchor;
  }
  if (!insideKeepIn(request.fixX, request.fixY, request.screenWidth, request.screenHeight, request.keepInMarginPx)) {
    outReason = Reason::KeepIn;
    return Action::ReAnchor;
  }
  if (request.partialMoves >= request.partialMoveBudget) {
    outReason = Reason::Budget;
    return Action::ReAnchor;
  }

  const int dx = request.fixX - request.drawnX;
  const int dy = request.fixY - request.drawnY;
  const int absDx = dx < 0 ? -dx : dx;
  const int absDy = dy < 0 ? -dy : dy;
  if (absDx < request.minMovePx && absDy < request.minMovePx) {
    outReason = Reason::BelowMoveFloor;
    return Action::Skip;
  }
  outReason = Reason::Moved;
  return Action::MoveMarker;
}

void formatDecisionLog(const Request& request, Action action, Reason reason, unsigned seq, char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  switch (action) {
    case Action::Skip:
      std::snprintf(out, outSize, "fix #%u skipped: %d,%d is under %d px from the marker", seq, (int)request.fixX,
                    (int)request.fixY, (int)request.minMovePx);
      return;
    case Action::MoveMarker:
      // partialMoves in the Request is the count *before* this move, and the
      // device's line prints the count after it -- moveMarker() increments
      // first. Add one here so both read the same.
      //
      // **This branch is the one place a second copy of a log line survives.**
      // MapActivity::moveMarker() still emits its own LOG_DBG, because it logs
      // from the state after the move and never sees a Request. The two texts
      // must be kept identical by hand; the skip and re-anchor lines have no
      // such twin.
      std::snprintf(out, outSize, "marker move to %d,%d (h%u rel %u), %u/%u before a clean frame", (int)request.fixX,
                    (int)request.fixY, (unsigned)request.fixHeadingStep,
                    (unsigned)relativeHeadingStep(request.fixHeadingStep, request.anchorHeadingStep),
                    (unsigned)request.partialMoves + 1u, (unsigned)request.partialMoveBudget);
      return;
    case Action::ReAnchor:
      std::snprintf(out, outSize, "fix #%u re-anchors: at %d,%d, heading %u vs frame's %u, %u moves in -- %s", seq,
                    (int)request.fixX, (int)request.fixY, (unsigned)request.fixHeadingStep,
                    (unsigned)request.anchorHeadingStep, (unsigned)request.partialMoves, reasonName(reason));
      return;
  }
  out[0] = '\0';
}

}  // namespace MapFollow
