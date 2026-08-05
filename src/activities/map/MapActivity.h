#pragma once

#include <cstdint>
#include <memory>

#include "HalFileSource.h"
#include "MapBleConsole.h"
#include "MapModeMask.h"
#include "MapProjection.h"
#include "MapSerialConsole.h"
#include "MapTileSource.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Draws real OSM map data from the SD card around the position received over
// BLE or typed into a command console -- P4 of docs/prototype-plan.md, merged
// with P3's console per that doc's "Merge" section, and P5's buttons, mode
// filter and BLE command channel on top.
//
// A received fix -- a BLE packet, a `pos`/`heading`/`redraw` command, or a
// ladder step from the buttons -- is a viewport reset: re-anchor on the
// marker, rebuild the MapProjection, work out which .tib tiles the rotated
// screen rect touches (docs/map-data-spec.md, "Which tiles to load"), and
// stream them through MapTileSource into MapRenderer. Nothing about the map
// is held between resets, and nothing scales with how much map is on screen.
//
// The tile source is ~5.5 KB of fixed buffers and is heap-allocated in
// onEnter(), never a local: a task stack here is 2-4 KB and CLAUDE.md caps
// stack locals at 256 bytes.
//
// A tile that is absent, truncated or crc32-mismatched draws as hatch, never
// as white. White is empty countryside; hatch is "no data here".
//
// ## The buttons
//
// | UP / DOWN      | zoom ladder, 5 rungs, 3..15 m/px                     |
// | LEFT / RIGHT   | marker-height ladder, 5 rungs, look-ahead 50..95 %   |
// | CONFIRM        | open the map menu: Refresh, Mode (ride/hike/cycle)   |
// | BACK           | leave (or close the menu, if it is open)             |
//
// Any of those that triggers a redraw first paints an hourglass badge above the
// button hints and refreshes only its rectangle (showBusy()). A ladder step
// waits out the settle timer and then spends the better part of two seconds on
// tiles and the refresh, which is long enough to read as a dead button. One
// badge per burst, cleared by the frame that replaces it.
//
// **There is no spare button** (docs/architecture-plan.md, "The map screen's
// button budget is exactly full"). CONFIRM stays the only entry point for
// anything new -- it opens a menu (OptionPopup) rather than acting directly,
// so Refresh and Mode share the one button the budget allows.
//
// Every one of them goes through MappedInputManager's logical buttons. The
// front four are user-remappable in settings and the mapping is
// orientation-aware; reading HalGPIO::BTN_* directly breaks both.
//
// ## Coalescing is required, not a nicety
//
// A ladder step is a full-screen redraw plus, when the step crosses an LOD,
// a fresh tile load off the card -- together the better part of two seconds.
// Three quick presses must cost one redraw, not three, so a press only moves
// the step in memory and arms a timer; the render happens once the presses
// settle.
//
// The settings write is armed on a longer timer still. CrossPointSettings
// persists to /.crosspoint/settings.json on the SD card, so every save is an
// SD write (CLAUDE.md rule 8), and it must never sit in front of the redraw.
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
  // Re-renders the last received fix at the current ladder steps and mode.
  // This is what a zoom or marker step produces: the reset re-anchors on the
  // marker, which is the point -- zooming out must show more of the road
  // ahead, not more of wherever the marker has drifted to.
  void renderCurrent();
  // Draws one line of the debug readout, trimmed to the screen width.
  // Mutates `text` in place.
  void drawDebugLine(int y, char* text);
  // Fixed top-right "N" indicator. The map is always drawn north-up today
  // (see renderViewport()'s kNoRouteDisplayHeading), so this is static
  // furniture, not derived from any heading.
  void drawCompass();
  // Immediate "working on it" feedback, above the button hints. A ladder step
  // or a Refresh does not reach the panel for the better part of two seconds
  // (settle, tile reads, then the refresh itself), which is long enough that a
  // rider cannot tell a slow redraw from a dead button. This paints an
  // hourglass into the current frame and refreshes ONLY its rectangle, so it
  // costs one fast windowed refresh and leaves the map on screen.
  //
  // One badge per burst: repeated presses coalesce into a single redraw
  // (armRedraw()), and the badge is already on screen, so they must not each
  // pay a refresh. busyShown_ is that latch, cleared by whatever repaints the
  // whole screen.
  void showBusy();
  void drawBusyBadge();
  // Badge rectangle in logical screen coordinates. displayBufferWindow()
  // handles the controller's multiple-of-8 alignment itself.
  void busyRect(int& x, int& y, int& w, int& h) const;

  // Ring plus a mode-specific center glyph -- dot (hike), small arrow
  // (cycle) or large arrow (ride). headingStep is the real incoming
  // heading, independent of the forced-north heading the map itself is
  // drawn with.
  void drawPositionMarker(int cx, int cy, uint8_t headingStep, MapRideMode mode);

  // Buttons, and the two timers they arm.
  void handleButtons();
  // delta moves along the ladder; out of range is a no-op, deliberately, so
  // pressing into the end of a ladder costs no refresh at all.
  void stepZoom(int delta);
  void stepMarker(int delta);
  void armRedraw();
  void armSave();
  // Pulls the console's ladder values back in after a command, switches the
  // mode's stored steps in when the mode changed, and pushes the resolved
  // values back so `info` reports what is on screen rather than what was
  // typed.
  void syncLaddersFromConsole();
  void publishLadders();
  // Writes the ladder steps to settings, but only if they actually differ
  // from what is already stored.
  void saveLaddersIfChanged();

  // CONFIRM's menu: Refresh and Mode, both in one flat list -- no second
  // popup. Picking Mode cycles ride->hike->cycle->ride and reopens the same
  // list with the row's label updated and the highlight still on Mode, so
  // repeated Select presses step through modes without leaving the menu.
  // initialIndex lets the Mode-cycle path reopen onto row 1 instead of
  // resetting to row 0. Draws the popup itself via
  // optionPopup_.processRender() right after show() -- MapActivity never
  // calls requestUpdate() (it always has drawn straight to the buffer, on
  // the main task, not through Activity's render(RenderLock&&)/render-task
  // path), so nothing else would ever paint the popup's first frame or its
  // label updating.
  void openMapMenu(int initialIndex = 0);
  // No-op if newMode is already current -- picking the mode already on
  // screen must cost nothing, same rule as stepZoom/stepMarker's ladder ends.
  void switchMode(MapRideMode newMode);

  // Allocated once in onEnter(), released in onExit(). MapTileSource holds
  // references to both, so neither may move or die while it is alive.
  std::unique_ptr<HalFileSource> file_;
  std::unique_ptr<MapTileSource> source_;
  // Reset per viewport reset, before the source is used again -- the source
  // reads it live, so it must not change part-way through a render.
  MapProjection proj_;

  bool hasReceivedAny_ = false;
  uint8_t lastDrawnSeq_ = 0;
  // The fix a ladder step re-renders around. Only meaningful once
  // hasReceivedAny_ is true.
  int32_t lastLatE7_ = 0;
  int32_t lastLonE7_ = 0;
  uint8_t lastHeading_ = 0;
  // True from onEnter() bootstrapping the last-saved fix off the card until
  // the first real fix of this session lands -- distinguishes "showing
  // where the rider was last seen" from "showing where they actually are
  // right now", so renderViewport() knows to keep the waiting banner up.
  bool showingPersistedFix_ = false;

  // Ladder state, per mode, held in memory and seeded from settings once in
  // onEnter(). **In memory, not read back out of settings on a mode
  // switch**: the settings write is debounced by seconds, so re-reading it
  // would silently lose any step landed on inside that window -- switch mode
  // and back within four seconds and the step you just chose would be gone.
  // Settings is where this is written to, never where it is read from after
  // onEnter().
  MapRideMode mode_ = MapRideMode::Ride;
  uint8_t zoomStep_[kMapRideModeCount] = {kDefaultZoomStepForMode[0], kDefaultZoomStepForMode[1],
                                          kDefaultZoomStepForMode[2]};
  uint8_t markerStep_[kMapRideModeCount] = {kDefaultMarkerStepForMode[0], kDefaultMarkerStepForMode[1],
                                            kDefaultMarkerStepForMode[2]};
  // The current mode's rungs. Everything on the render path reads these.
  uint8_t zoomStep() const { return zoomStep_[static_cast<uint8_t>(mode_)]; }
  uint8_t markerStep() const { return markerStep_[static_cast<uint8_t>(mode_)]; }
  // Class mask per mode, compiled in from style.json's `modes` block
  // (MapModeMaskDefaults.h, MapModeMask.h). No runtime source overrides it.
  MapModeMasks modeMasks_;

  // millis() deadlines; 0 means nothing armed. See the coalescing note above.
  uint32_t redrawDueMs_ = 0;
  uint32_t saveDueMs_ = 0;

  // True while the busy badge is on the panel. See showBusy().
  bool busyShown_ = false;

  // One state, two channels. A `zoom 3` over USB and a `zoom 3` over BLE
  // land on the same number because they share this object, not because two
  // copies are kept in step (MapCommandConsole.h).
  MapConsoleState consoleState_;
  MapSerialConsole serial_{consoleState_};
  MapBleConsole ble_{consoleState_};

  // CONFIRM's menu (Refresh / Mode). Mode's own onSelect re-shows this same
  // instance (openMapMenu(1)) to cycle in place, so there is only ever the
  // one popup, never a second one stacked on top.
  OptionPopup optionPopup_;
  // Set when a Back press closes optionPopup_ (loop()), cleared by the one
  // Back release it is meant to swallow (also loop()) -- see the comment
  // there for why the release needs swallowing at all.
  bool suppressBackRelease_ = false;
};
