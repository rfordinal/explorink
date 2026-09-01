#include "MapGnssHeading.h"

#include <cmath>

namespace MapGnssHeading {

uint8_t stepFor(float speedKmh, float courseDegrees, State& state) {
  // The gate, with its two thresholds. Between them nothing changes, which is
  // the whole point of having two.
  if (speedKmh >= kMovingKmh) {
    state.moving = true;
  } else if (speedKmh < kHoldingKmh) {
    state.moving = false;
  }
  // Not moving: hold whatever is on the panel. The caller draws this exactly as
  // it would draw a derived one -- a held heading is not a missing heading.
  if (!state.moving) return state.step;

  // fmodf keeps a receiver reporting 360.0 from landing on step 16, and the
  // negative branch covers a receiver that reports a signed course. Neither has
  // been seen from this L76K; both are cheap.
  float course = fmodf(courseDegrees, 360.0f);
  if (course < 0.0f) course += 360.0f;

  // The deadband is measured against the step currently drawn, not against the
  // previous course. The question is "has the course moved far enough to be
  // worth rotating the frame", and the frame shows a step, not a course.
  const float heldCentre = static_cast<float>(state.step) * 22.5f;
  float delta = fabsf(course - heldCentre);
  if (delta > 180.0f) delta = 360.0f - delta;  // the short way round the circle
  if (delta < (22.5f / 2.0f) + kDeadbandDeg) return state.step;

  state.step = static_cast<uint8_t>(lroundf(course / 22.5f)) & 0x0F;
  return state.step;
}

}  // namespace MapGnssHeading
