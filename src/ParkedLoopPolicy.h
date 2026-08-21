#pragma once

#include <stdint.h>

// How long the main loop should sleep at the end of an iteration.
//
// The whole point of this file is that the decision is a **pure function of
// four facts**, with no hardware in it, so a host test can pin the table and
// fail in CI when someone adds a per-tick timer to the map screen without
// having read docs/power-idle-sleep.md, "S2's missing half". That is guard
// item 2 of that file's "How this gets guarded" list, and the only one of the
// five that fires before a build reaches a device.
//
// What it is NOT: a power saving on its own. Slowing the tick only pays once
// light sleep is enabled (CONFIG_PM_ENABLE), because a delay() with PM off is
// a busy wait at full clock. This function exists so the policy is written
// down and testable before that build exists, not so it can be switched on
// early.

namespace parked_loop {

// The two cadences, in milliseconds. Kept as data rather than baked into the
// policy because **the parked number is not measured yet**: experiment 3
// reports the light-sleep residency of today's 100 Hz loop, and that number
// decides whether the cadence has to grow at all
// (docs/power-idle-sleep.md, "The open question this design turns on").
// Until then this default is 10 ms -- today's behaviour, i.e. no change --
// and the parked value below is a placeholder the measurement replaces.
struct Cadence {
  uint32_t activeMs = 10;  // src/main.cpp's delay(10) today
  uint32_t parkedMs = 10;  // placeholder: same as active until measured
};

// Everything the decision is allowed to look at. Four facts, all of them
// already known to the main loop.
struct Inputs {
  // The activity says nothing is moving: no fix has changed the view, no
  // popup is timing out, no redraw is pending.
  bool parked = false;
  // Work is waiting -- a tile the device asked for, a queued sync step. Not
  // the same as transferActive: queued work means the device wants a tick soon
  // even though nothing is on the wire yet.
  bool queuedWork = false;
  // Bytes are moving. Throughput is latency-bound on our own tick rate, so
  // this always wins.
  bool transferActive = false;
  // Since the last button, touch or tilt. A rider working the menu gets the
  // fast tick whatever else is true -- the same reason main.cpp restores full
  // clock on any input (src/main.cpp:836).
  uint32_t msSinceInput = 0;
};

// A rider who just pressed something is still interacting. Matches the CPU
// throttle's own idle threshold -- HalPowerManager::IDLE_POWER_SAVING_MS is
// 3000 ms (lib/hal/HalPowerManager.h:29-33) -- so the loop does not park
// while the clock is still full, which would be the one combination that
// costs latency and saves nothing.
constexpr uint32_t kRecentInputMs = 3000;

// The whole policy. Three ways to earn the fast tick; parking is what is left.
constexpr uint32_t tickMs(const Inputs& in, const Cadence& cadence = {}) {
  if (in.transferActive) return cadence.activeMs;
  if (in.queuedWork) return cadence.activeMs;
  if (in.msSinceInput < kRecentInputMs) return cadence.activeMs;
  if (!in.parked) return cadence.activeMs;
  return cadence.parkedMs;
}

// True when the loop is running slower than the active cadence, i.e. the
// parked branch above was taken. The field tripwire (guard item 3) checks the
// same condition against power.csv's `loops` column after a run; this is the
// in-firmware half of the same question.
constexpr bool isParked(const Inputs& in, const Cadence& cadence = {}) {
  return tickMs(in, cadence) > cadence.activeMs;
}

}  // namespace parked_loop
