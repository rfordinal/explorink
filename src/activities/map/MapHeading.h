#pragma once

#include <cstdint>

// 8-direction heading snap (45 degree steps) -- user's explicit call, no
// smooth/continuous rotation. Only 8 fixed map/marker orientations are ever
// drawn, so no per-frame trig is needed anywhere in the render path.
enum class MapHeading : uint8_t { N, NE, E, SE, S, SW, W, NW };
