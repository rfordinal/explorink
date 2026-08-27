#pragma once

#include <cstdint>

// Class bytes for the area layers. One small enum per layer, deliberately not
// slots in the 32-entry road enum (MapClassEnum.h): a record's layer id already
// says which vocabulary its class byte belongs to, and the road enum's reserved
// slots are a one-way door -- a 33rd entry breaks every card
// (docs/map-data-spec.md, "Every area layer needs its own class byte").
//
// These mirror mapbuilder's `landuse_class.py` and `water_class.py`. They are
// hand-written rather than generated for the same reason gen_mapstyle.py carries
// its own copy of the road class ids: nothing outside this repo may be read to
// build this repo. **If those files change, change these with them** -- a
// mismatch here draws a forest as built-up rather than failing.
//
// Verified against mapbuilder at parent-repo commit ab6d43a, 2026-08-05.

// Landuse layer (MapTileReader::Layer::Landuse). No `unknown`: the builder only
// writes a record when a tag maps to one of these, so a 0 here means a corrupt
// byte, and both passes skip it.
enum class MapLanduseClass : uint8_t {
  Forest = 1,   // natural=wood or landuse=forest
  BuiltUp = 2,  // landuse=residential/commercial/industrial/retail, merged
};

// Water layer (MapTileReader::Layer::Water). `Unknown` is a real value here,
// unlike landuse: a ditch or a weir still gets written, just undifferentiated,
// the same way an unrecognised `highway` lands on the road enum's `unknown`.
//
// Stream covers canal and drain too, matching how mapstyle.json already grouped
// them. Lake is the only one that arrives as a closed ring.
enum class MapWaterClass : uint8_t {
  Unknown = 0,
  River = 1,
  Stream = 2,
  Lake = 3,
};

// Slots to size a per-class style table with. Small on purpose -- these are not
// the road enum's 32, and a table indexed by one of these costs a handful of
// bytes of flash.
// Contour layer (MapTileReader::Layer::Contours). Mirrors mapbuilder's
// contour_class.py. No `unknown`, like landuse: the builder writes one of
// these two or nothing, so a 0 is a corrupt byte and both passes skip it.
//
// An **index contour** is every Nth line, drawn heavier so the eye can count.
// Which N is a build-time decision per LOD, not something the device knows.
enum class MapReliefClass : uint8_t {
  ContourMinor = 1,
  ContourIndex = 2,
  // A cliff is the one line on a mountain map that means "you cannot go this
  // way". Built 2026-08-27 from `natural=cliff`; measured at 0.4 kB per z13 tile
  // in the High Tatras and 1.2 kB in Mala Fatra against a 35 kB contour layer.
  //
  // Two things a reader of this layer must handle differently for a cliff:
  // `flags` is **0**, not an elevation -- a cliff runs across contours and has
  // no single height -- so nothing may print it as a number; and the record's
  // point order is OSM's, which puts the lower ground on the right of the
  // direction of travel. Nothing draws a correct-side tick yet, and the order
  // is what makes adding one a render change rather than a refetch.
  Cliff = 3,
  // Reserved, not built. `Cliff = 3` is the last class that fits a per-class
  // style table: kReliefClassSlots is 4, so the valid indices are 0-3, and
  // `drawContourClass` returns before it reads anything when
  // `index >= kReliefClassSlots` (MapRenderer.cpp:255). A Ridge left at 4
  // against a table of 4 therefore draws nothing at all, with no build error
  // and no log line.
  //
  // Building it means widening the table first, and that is cheap:
  // `contourWidthPx` is one byte per slot (MapStyle.h:277) and
  // data/mapstyle.json compiles to 14 MapStyle variants plus the base
  // (MapStyleDefaults.h:12 and :377), so a fifth slot costs 15 bytes of flash
  // before padding. `_CONTOUR_SLOTS` (scripts/gen_mapstyle.py:88) has to move
  // with it. Read off the code 2026-08-27, not measured.
  Ridge = 4,
};

// The names the contour code was written with, before the layer was named for its
// vocabulary rather than for its first class.
using MapContourClass = MapReliefClass;

inline constexpr uint8_t kLanduseClassSlots = 4;
inline constexpr uint8_t kWaterClassSlots = 4;
inline constexpr uint8_t kReliefClassSlots = 4;
inline constexpr uint8_t kContourClassSlots = kReliefClassSlots;
