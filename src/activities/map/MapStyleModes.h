#pragma once

#include "MapModeMask.h"

// Device-side loader for mapstyle.json's `modes` block. The only file in the
// map code that reads the style, so everything else -- including the mask
// logic itself (MapModeMask.h) -- stays testable natively.
//
// **Where the file lives.** On the SD card, in the same root as the tiles:
// /trailink/mapstyle.json. docs/map-render-spec.md says the device reads it
// out of the SPIFFS image built from firmware/trailink/data/, and that is
// still where the webapp writes the authoritative copy -- but nothing in this
// firmware mounts SPIFFS today, and mounting it costs flash on a build
// already at ~88 %. mapbuilder's build_tiles.py therefore copies the same
// file onto the card next to the tiles it just wrote, which also makes a card
// self-contained. See docs/map-data-spec.md.
//
// Absent, unreadable or malformed is not an error state: the masks keep their
// built-in defaults (MapModeMask.h) and the map still draws. A navigation
// device that draws nothing because a style file is missing is worse than one
// drawing a slightly wrong class set.
constexpr const char* kMapStyleDefaultPath = "/trailink/mapstyle.json";

// Fills `out` from `path`. Returns true only when at least one mode's class
// list was applied; on false, `out` is left exactly as it came in.
bool loadMapModeMasks(const char* path, MapModeMasks& out);
