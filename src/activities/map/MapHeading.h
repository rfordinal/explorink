#pragma once

#include <cstdint>

// 16-direction heading snap (22.5 degree steps), clockwise from north --
// docs/map-render-spec.md supersedes the earlier 8-direction decision. No
// smooth/continuous rotation: heading is derived from the route's direction
// of travel, which changes smoothly on its own, so 16 fixed orientations
// need no per-frame trig anywhere in the render path (see kHeadingDir in
// MapRenderer.cpp).
enum class MapHeading : uint8_t { N, NNE, NE, ENE, E, ESE, SE, SSE, S, SSW, SW, WSW, W, WNW, NW, NNW };
