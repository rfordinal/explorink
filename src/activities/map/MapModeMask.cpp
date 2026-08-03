#include "MapModeMask.h"

bool mapClassIdFromName(std::string_view name, uint8_t& outId) {
  if (name.empty()) return false;
  for (uint8_t id = 0; id < kClassEnumSlots; ++id) {
    const char* candidate = kMapClassNames[id];
    if (candidate == nullptr) continue;  // reserved slot, no name to match
    if (name == candidate) {
      outId = id;
      return true;
    }
  }
  return false;
}

bool mapRideModeFromName(std::string_view name, MapRideMode& outMode) {
  if (name == "ride") {
    outMode = MapRideMode::Ride;
    return true;
  }
  if (name == "hike") {
    outMode = MapRideMode::Hike;
    return true;
  }
  if (name == "cycle") {
    outMode = MapRideMode::Cycle;
    return true;
  }
  return false;
}
