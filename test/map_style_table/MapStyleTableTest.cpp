// The per-(mode, rung) style table and the class masks that go with it.
//
// docs/map-data-spec.md, "Style is per mode and per rung". data/mapstyle.json
// lets any block carry a `when` list; scripts/mapstyle_variants.py resolves it
// at build time and scripts/gen_mapstyle.py / gen_mode_masks.py emit the
// finished tables. Nothing here tests the resolver -- that is Python, and
// scripts/test_mapstyle_variants.py owns it. What is tested here is the shape
// of what arrived, and the invariants the firmware then depends on.

#include <gtest/gtest.h>

#include "MapModeMask.h"
#include "MapStyleTable.h"
#include "MapViewport.h"

namespace {

constexpr int kRungs = MapViewport::kZoomStepCount;

}  // namespace

TEST(MapStyleTable, EveryModeAndRungResolvesToAStyleThatDrawsRoads) {
  // The floor under everything else. A rung whose style draws no road at all is
  // a blank panel that looks exactly like a missing tile, so the generators
  // fail the build on it -- this is the assertion that says so in C++ as well,
  // because a generator can be edited and a build error read as noise.
  for (int modeIndex = 0; modeIndex < kMapRideModeCount; ++modeIndex) {
    const MapRideMode mode = static_cast<MapRideMode>(modeIndex);
    for (int step = 0; step < kRungs; ++step) {
      const MapStyle& style = mapStyleFor(mode, step);
      bool anyRoad = false;
      for (uint8_t classId = 0; classId < kClassEnumSlots; ++classId) {
        if (style.roadWidthPx[classId] > 0) anyRoad = true;
      }
      EXPECT_TRUE(anyRoad) << mapRideModeName(mode) << " rung " << step;
    }
  }
}

TEST(MapStyleTable, RoadsGetNarrowerAsTheRungGetsCoarser) {
  // The reason the table exists at all. Widths are device pixels and do not
  // scale with the ground under them: an 11 px motorway is 11 m of ground at
  // rung 0 and 220 m at rung 4, which is what made the coarse rungs read as a
  // chaotic city (docs/PROGRESS.md, 2026-08-08). So a main road must never get
  // wider as the rung coarsens, and must actually get narrower somewhere.
  //
  // Not a check on any particular number: those are a style decision to judge
  // on the panel, and this test must not have to be edited every time one is
  // tuned. It checks the direction, which is the thing that would be a bug.
  //
  // **Motorway is deliberately exempt from having to narrow.** Maintainer's
  // call 2026-08-25: it is 5 px of shaded inline inside a 2 px outline at every
  // rung, because it is the class you find at a glance and it should not fade
  // out at the widest view. It still may not get *wider* as the rung coarsens
  // -- that is a bug at any width, and this test caught exactly that once, when
  // a 6-then-7 ladder was proposed.
  const MapClassId mains[] = {MapClassId::Motorway, MapClassId::Trunk, MapClassId::Primary,
                              MapClassId::Secondary, MapClassId::Tertiary};
  for (const MapClassId classId : mains) {
    const uint8_t index = static_cast<uint8_t>(classId);
    uint8_t previous = mapStyleFor(MapRideMode::Ride, 0).roadWidthPx[index];
    const uint8_t atRungZero = previous;
    for (int step = 1; step < kRungs; ++step) {
      const uint8_t width = mapStyleFor(MapRideMode::Ride, step).roadWidthPx[index];
      EXPECT_LE(width, previous) << "class " << (int)index << " widens at rung " << step;
      previous = width;
    }
    if (classId == MapClassId::Motorway) continue;
    EXPECT_LT(previous, atRungZero) << "class " << (int)index << " never narrows across the ladder";
  }
  // And the ladder as a whole still has to thin, or the per-rung table is doing
  // nothing at all -- which is what the exemption above could otherwise hide.
  EXPECT_LT(mapStyleFor(MapRideMode::Ride, kRungs - 1).roadWidthPx[static_cast<uint8_t>(MapClassId::Tertiary)],
            mapStyleFor(MapRideMode::Ride, 0).roadWidthPx[static_cast<uint8_t>(MapClassId::Tertiary)]);
}

TEST(MapStyleTable, TheMarkerAnchorIsTheSameAtEveryModeAndRung) {
  // MapViewport turns the anchor into a constexpr and the whole tile
  // arithmetic is built on it, so a per-rung anchor would be a viewport that
  // disagrees with itself. `when` is banned under `device` in the resolver and
  // the generator checks it again; this is the third lock, on what shipped.
  for (int modeIndex = 0; modeIndex < kMapRideModeCount; ++modeIndex) {
    for (int step = 0; step < kRungs; ++step) {
      const MapStyle& style = mapStyleFor(static_cast<MapRideMode>(modeIndex), step);
      EXPECT_EQ(style.markerXPx, kDefaultMapStyle.markerXPx);
      EXPECT_EQ(style.markerYPx, kDefaultMapStyle.markerYPx);
    }
  }
}

TEST(MapStyleTable, OutOfRangeArgumentsClampInsteadOfIndexingPastTheTable) {
  // A rung arrives as a persisted settings byte and a mode off a BLE or serial
  // command, and both channels are unauthenticated (../../CLAUDE.md,
  // "Security"). Neither may be able to read past the array.
  EXPECT_EQ(&mapStyleFor(MapRideMode::Ride, -1), &mapStyleFor(MapRideMode::Ride, 0));
  EXPECT_EQ(&mapStyleFor(MapRideMode::Ride, kRungs + 99), &mapStyleFor(MapRideMode::Ride, kRungs - 1));
  EXPECT_EQ(&mapStyleFor(static_cast<MapRideMode>(200), 2), &mapStyleFor(MapRideMode::Ride, 2));

  const MapModeMasks masks;
  EXPECT_EQ(masks.forMode(MapRideMode::Hike, -5), masks.forMode(MapRideMode::Hike, 0));
  EXPECT_EQ(masks.forMode(MapRideMode::Hike, 999), masks.forMode(MapRideMode::Hike, kRungs - 1));
  EXPECT_EQ(masks.forMode(static_cast<MapRideMode>(200), 1), masks.forMode(MapRideMode::Ride, 1));
}

TEST(MapStyleTable, TheFilterNeverLetsThroughAClassTheRungThenHides) {
  // The whole point of intersecting the mode's class list with the rung's
  // drawn classes. A way that passes the filter and is then drawn at width 0
  // costs a card read, a bbox test and a projection, and puts nothing on the
  // panel. The two used to be able to disagree; now they cannot.
  const MapModeMasks masks;
  for (int modeIndex = 0; modeIndex < kMapRideModeCount; ++modeIndex) {
    const MapRideMode mode = static_cast<MapRideMode>(modeIndex);
    for (int step = 0; step < kRungs; ++step) {
      const uint32_t mask = masks.forMode(mode, step);
      const MapStyle& style = mapStyleFor(mode, step);
      for (uint8_t classId = 0; classId < kClassEnumSlots; ++classId) {
        if ((mask & (1u << classId)) == 0) continue;
        EXPECT_GT(style.roadWidthPx[classId], 0)
            << mapRideModeName(mode) << " rung " << step << " reads class " << (int)classId
            << " off the card and draws nothing with it";
      }
    }
  }
}

TEST(MapStyleTable, ARungsFilterIsNeverWiderThanItsModesVocabulary) {
  // The mode's `classes` list is the ceiling: a rung may hide more, never
  // reveal something the mode does not do. A hiker's footpath must not appear
  // for a rider because a road rule was edited.
  const MapModeMasks masks;
  const uint32_t vocabulary[kMapRideModeCount] = {kDefaultRideMask, kDefaultHikeMask, kDefaultCycleMask};
  for (int modeIndex = 0; modeIndex < kMapRideModeCount; ++modeIndex) {
    for (int step = 0; step < kRungs; ++step) {
      const uint32_t mask = masks.forMode(static_cast<MapRideMode>(modeIndex), step);
      EXPECT_EQ(mask & vocabulary[modeIndex], mask) << "mode " << modeIndex << " rung " << step;
    }
  }
}

TEST(MapStyleTable, ThePlaceNameCapRisesWithTheGroundOnThePanel) {
  // Rung 0 shows 480 x 800 m, one settlement: a dozen names there is a dozen
  // names for one village. Rung 6 shows 24 x 40 km, where a dozen names is a
  // map of the region and the whole reason to be at that rung. Maintainer's
  // call 2026-08-12. It lived in MapViewport::ZoomStep::maxLabels until
  // 2026-08-25 and is layers.places' `when` now -- same numbers, one file.
  uint8_t previous = 0;
  for (int step = 0; step < kRungs; ++step) {
    const uint8_t cap = mapStyleFor(MapRideMode::Ride, step).placeMaxLabels;
    EXPECT_GE(cap, previous) << "rung " << step << " allows fewer names than the rung below it";
    previous = cap;
  }
  EXPECT_GT(mapStyleFor(MapRideMode::Ride, kRungs - 1).placeMaxLabels,
            mapStyleFor(MapRideMode::Ride, 0).placeMaxLabels);
}

TEST(MapStyleTable, AToneOnARoadAlwaysHasAnInteriorToPutItIn) {
  // `fill: tone` shades the middle of a cased road (MapStyle::roadFillTone).
  // Two things have to hold or it draws nothing while the style file says it
  // draws something: there must be a casing at all, because with none the road
  // is solid black and has no middle; and the middle must be at least 2 px,
  // because the lightest tone has a period of 2 and a 1 px interior cannot
  // carry it.
  //
  // gen_mapstyle.py refuses both outright. This is the same check on what
  // actually shipped, at every mode and rung -- a generator can be edited.
  for (int modeIndex = 0; modeIndex < kMapRideModeCount; ++modeIndex) {
    const MapRideMode mode = static_cast<MapRideMode>(modeIndex);
    for (int step = 0; step < kRungs; ++step) {
      const MapStyle& style = mapStyleFor(mode, step);
      for (uint8_t classId = 0; classId < kClassEnumSlots; ++classId) {
        if (style.roadFillTone[classId] == MapAreaTone::None) continue;
        const int casing = style.roadCasingPx[classId];
        EXPECT_GT(casing, 0) << mapRideModeName(mode) << " rung " << step << " class " << (int)classId;
        EXPECT_GE(style.roadWidthPx[classId] - 2 * casing, 2)
            << mapRideModeName(mode) << " rung " << step << " class " << (int)classId
            << " has a tone but no room for it";
      }
    }
  }
}
