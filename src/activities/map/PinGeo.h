#pragma once

#include <cstddef>
#include <cstdint>

// Straight-line distance between two 1e7 coordinates, and how it is written for
// a rider. Never routing: a pin is a place to keep a spatial relation to, and
// the device has no router (../../docs/pins-plan.md, "Distance").
//
// Pure: no Arduino, no HAL, host-tested (test/pins/).
//
// Integer only, no libm, no float. The ESP32-C3 is RV32IMC -- every float is
// soft-float (read off the target spec, not measured) -- and a distance shown to
// 10 m does not need one. Equirectangular approximation with a cos(latitude)
// scale: exact enough far past any distance that matters here (the error grows
// with separation, and a pin the rider cares about is tens of kilometres away at
// most), and it costs two multiplies, a table lookup and one integer square root.

namespace PinGeo {

// Metres between the two points, rounded to the nearest metre. Order does not
// matter. Handles the antimeridian: a longitude difference is taken the short way
// round.
uint32_t distanceM(int32_t lat1E7, int32_t lon1E7, int32_t lat2E7, int32_t lon2E7);

// Writes the distance the way the lists show it, nul-terminated:
//
//   below 1 km    "820 m"    rounded to 10 m -- a metre of precision on a
//                            straight-line distance is noise, and the last
//                            digit would flicker on every fix
//   below 10 km   "4.2 km"
//   above         "37 km"
//
// `bufLen` should be at least 12. Writes an empty string if it cannot fit.
void formatDistance(uint32_t metres, char* buf, size_t bufLen);

}  // namespace PinGeo
