#pragma once

#include <cstdint>

#include "MapRideMode.h"
#include "MapStyle.h"
#include "MapStyleDefaults.h"

// Picking the style for one travel mode at one zoom rung.
//
// docs/map-data-spec.md, "Style is per mode and per rung". data/mapstyle.json
// lets any block carry a `when` list -- "primary is 9 px normally, 6 px at
// rungs 3-4, 4 px above that, and hidden for a hiker at rung 6". None of that
// reaches the device: scripts/mapstyle_variants.py evaluates every rule at
// build time and scripts/gen_mapstyle.py emits the finished styles plus an
// index. This header is the whole runtime cost of the feature -- two array
// lookups, no branches, no parsing.
//
// Why a table and not a multiplier: a width that scales with the rung sounds
// cheaper and is wrong at both ends. Widths are device pixels, so an 11 px
// motorway covers 11 m of ground at rung 0 and 220 m at rung 4 -- which is
// what made the coarse rungs read as a chaotic city (docs/PROGRESS.md,
// 2026-08-08). But the fix is not one curve: a motorway must stay visible as
// it thins, a residential street must vanish rather than thin, and a railway's
// tick rhythm is not a width at all. Those are three different decisions per
// class, which is a table.

// The style to draw with. Out-of-range arguments clamp rather than fault: a
// rung comes from settings and a mode from a BLE or serial command, and a
// stolen device's operator does not get to index past the array
// (../../../CLAUDE.md, "Security").
inline const MapStyle& mapStyleFor(MapRideMode mode, int zoomStep) {
  const uint8_t modeIndex = static_cast<uint8_t>(mode) < kMapRideModeCount ? static_cast<uint8_t>(mode) : 0;
  const int step = zoomStep < 0 ? 0 : (zoomStep >= kMapZoomStepCount ? kMapZoomStepCount - 1 : zoomStep);
  return kMapStyleVariants[kMapStyleIndex[modeIndex][step]];
}

// Does ANY (mode, rung) draw place names / POI marks at all?
//
// These answer an allocation question, not a drawing one: the scratch buffers
// are taken once in MapActivity::onEnter() and must cover every rung the rider
// can reach afterwards, so asking the base style would under-allocate the
// moment a `when` switched a layer on at one rung only. The per-frame decision
// stays with the resolved style, as everywhere else.
inline bool mapStyleAnyDrawsPlaceLabels() {
  for (const MapStyle& style : kMapStyleVariants) {
    if (style.placeMaxLabels > 0 && (style.placeLabelPx > 0 || style.placeLabelMinorPx > 0)) return true;
  }
  return false;
}

inline bool mapStyleAnyDrawsPointMarks() {
  for (const MapStyle& style : kMapStyleVariants) {
    if (style.pointSquarePx > 0) return true;
  }
  return false;
}

// True when the style file draws the same picture at every mode and rung.
// Only diagnostics should care -- the renderer always goes through
// mapStyleFor() so that the answer cannot change how anything is drawn.
inline constexpr bool mapStyleHasVariants() {
  return sizeof(kMapStyleVariants) / sizeof(kMapStyleVariants[0]) > 1;
}
