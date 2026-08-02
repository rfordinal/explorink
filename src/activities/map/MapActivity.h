#pragma once

#include <cstdint>
#include <memory>

#include "HalFileSource.h"
#include "MapProjection.h"
#include "MapSerialConsole.h"
#include "MapTileSource.h"
#include "activities/Activity.h"

// Draws real OSM map data from the SD card around the position received over
// BLE or typed into the serial command console -- P4 of
// docs/prototype-plan.md, merged with P3's console per that doc's "Merge"
// section.
//
// A received fix -- a BLE packet, or a `pos`/`heading`/`redraw` console
// command -- is a viewport reset: re-anchor on the marker, rebuild the
// MapProjection, work out which .tib tiles the rotated screen rect touches
// (docs/map-data-spec.md, "Which tiles to load"), and stream them through
// MapTileSource into MapRenderer. Nothing about the map is held between
// resets, and nothing scales with how much map is on screen.
//
// The tile source is ~5.5 KB of fixed buffers and is heap-allocated in
// onEnter(), never a local: a task stack here is 2-4 KB and CLAUDE.md caps
// stack locals at 256 bytes.
//
// A tile that is absent, truncated or crc32-mismatched draws as hatch, never
// as white. White is empty countryside; hatch is "no data here".
//
// No buttons yet -- the zoom and marker ladders and the mode filter are P5.
// Zoom is fixed at ladder step kZoomStep; the `zoom`, `marker` and `mode`
// console commands still answer ERR unimplemented.
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
  void renderWaiting();
  // headingStep is always 0-15 (MapHeading's domain) -- callers do any
  // channel-specific conversion before calling this, so the projection and
  // the debug readout only ever see one heading representation.
  void renderViewport(int32_t latE7, int32_t lonE7, uint8_t headingStep, uint8_t seq);
  // Draws one line of the debug readout, trimmed to the screen width.
  // Mutates `text` in place.
  void drawDebugLine(int y, char* text);

  // Allocated once in onEnter(), released in onExit(). MapTileSource holds
  // references to both, so neither may move or die while it is alive.
  std::unique_ptr<HalFileSource> file_;
  std::unique_ptr<MapTileSource> source_;
  // Reset per viewport reset, before the source is used again -- the source
  // reads it live, so it must not change part-way through a render.
  MapProjection proj_;

  bool hasReceivedAny_ = false;
  uint8_t lastDrawnSeq_ = 0;

  // P3 serial command console. Everything it does lives in its own files;
  // this member and one block in loop() are the whole footprint here.
  MapSerialConsole console_;
};
