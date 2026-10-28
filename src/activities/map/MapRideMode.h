#pragma once

#include <cstdint>

// The three travel modes, in one header because three unrelated layers need
// the same enum: the command grammar (`mode ride|hike|tourist`), the class mask
// the renderer filters with, and the per-mode ladder steps in
// CrossPointSettings. Nothing here is I/O, so all three can include it.
//
// The values are indices into a 3-entry array in every one of those places
// -- settings, masks, ladders -- so they are fixed. Adding a fourth mode
// means widening those arrays, not renumbering these.
// Slot 2 was `Cycle` until 2026-10-27. Cycle owned a class mask and nothing
// else -- no style variant in data/mapstyle.json ever gave it a mark of its
// own -- so the mode menu offered a third map that was ride's with cycleways
// added, under a name that promised a cyclist's map nobody had built. One
// `when` block did name it, the one that turns the relief layer off for ride
// and cycle, so the accurate statement is that cycle could switch a layer off
// and could not draw anything differently. Tourist took the slot rather than becoming a fourth row because the
// city walker has style variants to hold and cycle had none; cycle is
// unbuilt, not cancelled, and comes back when somebody writes its row.
// docs/map-style.md ("Tourist") and the parent repo's docs/tourist-mode.md.
enum class MapRideMode : uint8_t { Ride = 0, Hike = 1, Tourist = 2 };

inline constexpr uint8_t kMapRideModeCount = 3;

// Mode's starting rung on each ladder, indexed by MapRideMode. The buttons
// own the step after that, and the chosen step is stored per mode, so
// switching ride to hike and back returns each to what it was.
// docs/map-data-spec.md ("hike starts at step 0, ride at step 2") and
// docs/architecture-plan.md ("hike step 1, ride step 3").
//
// They live in this header rather than next to the ladders themselves
// because CrossPointSettings needs the same numbers for its defaults, and a
// second copy of them there would be a silent way for a fresh device and a
// mode switch to disagree about where a mode starts.
// Tourist starts where hike starts, not where cycle did (step 1): a walker in
// an old town is reading the alley they are standing in, and the rung that
// shows a block of it is the finest one. Cycle's step 1 was a cyclist's
// compromise between a hiker's block and a rider's valley; nothing of that
// survives the mode it replaced.
inline constexpr uint8_t kDefaultZoomStepForMode[kMapRideModeCount] = {2, 0, 0};
inline constexpr uint8_t kDefaultMarkerStepForMode[kMapRideModeCount] = {3, 1, 1};

// Rungs on each ladder. They were one number while both were five; they are
// two numbers since 2026-08-12, because the zoom ladder grew to seven and the
// marker ladder had no reason to follow it (the marker anchor is a screen
// position, and five positions already span mid-screen to bottom edge).
//
// Both are still short for the same reason: a step is a full-screen e-ink
// refresh, so a longer ladder means waiting through refreshes to get anywhere
// (docs/map-data-spec.md). MapViewport static_asserts its two tables against
// these, and CrossPointSettings clamps a hand-edited settings file to them --
// each against its own, so a zoom step of 6 survives a reload and a marker step
// of 6 does not.
inline constexpr uint8_t kMapZoomStepCount = 7;
inline constexpr uint8_t kMapMarkerStepCount = 5;

// Wire name, matching mapstyle.json's `modes` keys and the `mode` command's
// argument. Same string in both directions, so a round trip through `info`
// and back through `mode` is exact.
inline const char* mapRideModeName(MapRideMode mode) {
  switch (mode) {
    case MapRideMode::Hike:
      return "hike";
    case MapRideMode::Tourist:
      return "tourist";
    case MapRideMode::Ride:
    default:
      return "ride";
  }
}
