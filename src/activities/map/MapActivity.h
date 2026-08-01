#pragma once

#include <cstdint>

#include "activities/Activity.h"

// First on-device checkpoint for the map/nav feature (see
// docs/firmware-implementation-plan.md in the parent xteink repo). Draws a
// hardcoded/mock MapViewState via MapRenderer -- real map/route loading
// lands in a later phase.
//
// Also runs the BLE peripheral (Phase 3) and shows a plain-text debug
// readout of the latest received position+heading, independent of the map
// rendering -- lets BLE be verified end-to-end before it's wired into the
// actual marker (see the "Wire received BLE position into actual map
// marker" follow-up task).
class MapActivity final : public Activity {
 public:
  MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Same mechanism CrossPointWebServerActivity/OtaUpdateActivity/etc. use --
  // don't let the device auto-sleep (and drop off USB) while the BLE
  // peripheral is running and might receive a position update any moment.
  bool preventAutoSleep() override;

 private:
  void renderDebugReadout(bool haveUpdate, int32_t lat, int32_t lon, uint8_t heading, uint8_t seq);

  bool hasReceivedAny_ = false;
  uint8_t lastDrawnSeq_ = 0;
};
