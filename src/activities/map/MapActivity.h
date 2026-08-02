#pragma once

#include <cstdint>

#include "MapSerialConsole.h"
#include "activities/Activity.h"

// First on-device checkpoint for the map/nav feature (see
// docs/firmware-implementation-plan.md in the parent xteink repo). Draws a
// hardcoded/mock road/village layout via MapRenderer -- real base-map
// loading lands in a later phase (see docs/roadmap.md item 7).
//
// Runs the BLE peripheral (Phase 3) and moves the marker using a
// placeholder lat/lon-to-screen projection (the first received fix becomes
// the screen center, later fixes offset from it by a fixed scale) --  not
// a real map projection, just enough to prove BLE data actually drives the
// marker until mapbuilder's real coordinate system replaces it. A plain
// text line still shows the raw values for debugging.
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

  // Placeholder projection origin: the lat/lon of the first update received
  // since onEnter(), reset every time the screen is (re)entered.
  bool hasOrigin_ = false;
  int32_t originLat_ = 0;
  int32_t originLon_ = 0;

  // P3 serial command console. Everything it does lives in its own files;
  // this member and one block in loop() are the whole footprint here.
  MapSerialConsole console_;
};
