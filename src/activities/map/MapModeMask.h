#pragma once

#include <cstdint>
#include <string_view>

#include "MapClassEnum.h"
#include "MapRideMode.h"

// The render-time mode filter -- docs/map-data-spec.md, "Mode is a
// render-time filter".
//
// One tile set serves ride, hike and cycle. A mode is a 32-bit mask over the
// tile format's own class_id enum, and a way is drawn when
// `mask & (1u << class_id)`. Nothing is filtered at build time: doing that
// would mean three tile sets on the card and a re-sync every time the rider
// changes mode.
//
// Pure. No I/O, no Arduino, no JSON -- MapStyleModes is the device-side
// loader that fills these from mapstyle.json, and it is the only file that
// touches the card. That split is what lets the mask logic be tested
// natively.

// One class's bit. Written as a function rather than a macro so a bad
// argument is a compile error.
constexpr uint32_t mapClassBit(MapClassId id) { return 1u << static_cast<uint8_t>(id); }

// Built-in masks, used when the card carries no mapstyle.json or its `modes`
// block is missing or unreadable. They are deliberately a duplicate of the
// lists in mapbuilder/style.json rather than the only copy: a card with no
// style file must still draw a usable map, and "draws nothing" is the worst
// possible failure for a navigation device. mapstyle.json wins whenever it
// is present.
//
// Ride keeps `track`: this is a trail device, and a forest track is the point
// of it rather than an edge case. What a track is *closed* to is the
// `no_motor` flag's job, not the class list's -- see docs/map-data-spec.md,
// "The class enum".
inline constexpr uint32_t kDefaultRideMask =
    mapClassBit(MapClassId::Unknown) | mapClassBit(MapClassId::Motorway) | mapClassBit(MapClassId::Trunk) |
    mapClassBit(MapClassId::Primary) | mapClassBit(MapClassId::Secondary) | mapClassBit(MapClassId::Tertiary) |
    mapClassBit(MapClassId::Unclassified) | mapClassBit(MapClassId::Residential) |
    mapClassBit(MapClassId::LivingStreet) | mapClassBit(MapClassId::Service) | mapClassBit(MapClassId::Track) |
    mapClassBit(MapClassId::Ferry);

// Hike adds everything a walker can use and a motorcycle cannot -- paths,
// footways, steps, bridleways, pedestrian streets -- and keeps the big roads,
// which are landmarks and barriers on foot rather than routes.
inline constexpr uint32_t kDefaultHikeMask =
    kDefaultRideMask | mapClassBit(MapClassId::Pedestrian) | mapClassBit(MapClassId::Bridleway) |
    mapClassBit(MapClassId::Cycleway) | mapClassBit(MapClassId::Footway) | mapClassBit(MapClassId::Path) |
    mapClassBit(MapClassId::Steps) | mapClassBit(MapClassId::Railway) | mapClassBit(MapClassId::Aerialway);

inline constexpr uint32_t kDefaultCycleMask = kDefaultRideMask | mapClassBit(MapClassId::Cycleway) |
                                              mapClassBit(MapClassId::Path) | mapClassBit(MapClassId::Railway);

// One mask per mode, indexed by MapRideMode. Default-constructed to the
// built-ins above, so an instance is usable before anything has been loaded.
struct MapModeMasks {
  uint32_t mask[kMapRideModeCount] = {kDefaultRideMask, kDefaultHikeMask, kDefaultCycleMask};

  uint32_t forMode(MapRideMode mode) const {
    const uint8_t index = static_cast<uint8_t>(mode);
    return index < kMapRideModeCount ? mask[index] : kDefaultRideMask;
  }
  void setMode(MapRideMode mode, uint32_t value) {
    const uint8_t index = static_cast<uint8_t>(mode);
    if (index < kMapRideModeCount) mask[index] = value;
  }
};

// Resolves one of the tile format's class names ("motorway", "living_street")
// to its class_id. Reserved slots are nullptr in kMapClassNames and so can
// never be resolved by name -- an unknown name is a false return, never a
// silently wrong bit.
bool mapClassIdFromName(std::string_view name, uint8_t& outId);

// mapstyle.json's `modes` keys. Returns false for anything else.
bool mapRideModeFromName(std::string_view name, MapRideMode& outMode);
