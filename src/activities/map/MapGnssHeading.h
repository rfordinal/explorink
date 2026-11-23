#pragma once

#include <cstdint>

// Turning a GNSS course into the heading the map draws.
//
// Pure arithmetic, no Arduino, no driver: same shape as MapFollow::decide()
// next door, and for the same reason -- it is a decision worth testing on the
// host, and one a second client (iOS, a simulator, a replay tool) has to be
// able to reproduce exactly. MapActivity owns the state and calls in.
//
// ## Why this exists at all
//
// Thesis V35 puts the navigation head in the phone, and the maintainer's
// standing instruction is not to add device-side hysteresis for what the phone
// already stabilises. This is the exception the plan anticipated: a board with
// its own receiver has no phone to put it in (docs/gnss-to-map-plan.md, step 4).
//
// It is kept to the smallest thing that works -- a speed gate with hysteresis
// and a deadband on the step. No filtering of the course, no dead reckoning
// across a dropout, no per-mode tuning. Each of those is a thing to add once
// somebody has ridden with this one and can say what it gets wrong.
namespace MapGnssHeading {

// The gate for believing the course at all, in km/h, and the hysteresis around
// it. A receiver at rest reports a course and it is noise.
//
// **THE NUMBER BELOW IS KNOWN TO BE TOO LOW. Do not read it as a working
// threshold.** It was chosen from a single stationary reading of 1.3 km/h
// (2026-08-31) treated as a noise floor. It is not one. A stationary indoor run
// on 2026-09-01 reported speeds up to **23.7 km/h** with the course swinging
// 206 -> 252 -> 21 -> 148 degrees across a minute, all at quality 1, 8
// satellites and HDOP 1.4 -- so every gate below 24 km/h passes indoor
// multipath, and no threshold on instantaneous speed alone can separate this
// from a rider.
//
// What is NOT known is how bad this is outdoors with real sky, which is the
// case the product cares about. Until somebody rides with it, this gate is a
// placeholder that behaves correctly for a rider and wrongly for a parked
// device indoors. See docs/gnss.md, "The gate was chosen on a number that was
// not a noise floor".
//
// Two thresholds, not one, because a single one flutters: a rider sitting at
// exactly the gate would flip between moving and stopped on every fix, and a
// flip that changes the step rotates the whole frame -- a full e-ink redraw,
// about a second.
//
// 3.0 rather than a motorbike-shaped 5.0 **because of Hike mode**: walking is
// about 4 to 5 km/h and a gate at 5 would leave a hiker permanently without a
// heading. It still clears the 1.3 km/h noise floor. First cut, chosen on those
// two numbers and NOT yet judged on the device.
inline constexpr float kMovingKmh = 3.0f;
inline constexpr float kHoldingKmh = 2.0f;

// Degrees a course must travel past a step boundary before the drawn step
// follows. Steps are 22.5 degrees wide (16 of them, MapHeading), so a course
// sitting on a boundary would otherwise flip the step back and forth on noise
// alone, and every flip is a frame rotation. 6 degrees is about a quarter of a
// step: enough to stop the flutter, small enough that a real turn still lands
// within one fix.
inline constexpr float kDeadbandDeg = 6.0f;

// Carried between fixes by the caller. `moving` is the gate's hysteresis and
// means "believe the course", not "the rider is in motion" -- between the two
// thresholds it keeps whatever it was. `step` is what the panel is showing:
// 0 (north) until the rider first moves, which is what the map showed before a
// heading was derived at all.
struct State {
  bool moving = false;
  uint8_t step = 0;
};

// The step to draw for this fix. Updates `state`.
uint8_t stepFor(float speedKmh, float courseDegrees, State& state);

}  // namespace MapGnssHeading
