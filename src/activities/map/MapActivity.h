#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>

#include "HalFileSource.h"
#include "MapBleConsole.h"
#include "MapFollow.h"
#include "MapModeMask.h"
#include "MapProjection.h"
#include "MapRenderer.h"
#include "MapRouteSource.h"
#include "MapSerialConsole.h"
#include "MapTileSource.h"
#include "MapTransferReceiver.h"
#include "MapViewport.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Draws real OSM map data from the SD card around the position received over
// BLE or typed into a command console -- P4 of docs/prototype-plan.md, merged
// with P3's console per that doc's "Merge" section, and P5's buttons, mode
// filter and BLE command channel on top.
//
// A viewport reset -- a ladder step, a mode switch, a Refresh, or a fix the
// frame on screen cannot hold -- re-anchors on the marker, rebuilds the
// MapProjection, works out which .tib tiles the rotated screen rect touches
// (docs/map-data-spec.md, "Which tiles to load"), and streams them through
// MapTileSource into MapRenderer. Nothing about the map is held between resets,
// and nothing scales with how much map is on screen.
//
// ## Most fixes do not reset the viewport
//
// A fix that has moved the rider 20 metres does not invalidate the map on the
// panel -- only the 64x64 patch the marker sits in. So a fix is projected
// through the projection the frame was drawn with, and if the marker stays
// clear of the screen edges the marker alone moves: restore the pixels it
// covered, draw it at the new spot, refresh those rectangles and nothing else
// (moveMarker()). No SD read, no MapRenderer pass, no whole-panel waveform.
// MapFollow.h owns that decision and is unit-tested on the host; see
// docs/map-follow.md for the mechanism and its costs.
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
  // `routePath` is an absolute card path to a .tir route, or nullptr for none.
  // RouteSelectActivity passes what the rider picked; every other caller --
  // `CMD:GOTO_MAP` over serial, the OOM fallbacks -- passes nothing and gets the
  // map exactly as it was before routes existed.
  MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* routePath = nullptr);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Same mechanism CrossPointWebServerActivity/OtaUpdateActivity/etc. use --
  // don't let the device auto-sleep (and drop off USB) while the BLE
  // peripheral is running and might receive a position update any moment.
  bool preventAutoSleep() override;

 private:
  void renderWaiting();
  // A frame that says the tiles are being read, refreshed before the read
  // starts. Entering this screen with a stored fix goes straight into
  // renderViewport(), and nothing reaches the panel until its displayBuffer()
  // at the end -- 10.9 s at rung 0 and 15.3 s at rung 1, measured 2026-08-06,
  // during which e-ink holds whatever screen the rider came from. This costs
  // one 500 ms refresh and replaces that with an answer.
  // docs/optimization/09-progressive-render.md, step 0.
  void renderLoadingTiles();
  // Routes one incoming fix (BLE packet or console `pos`) through MapFollow's
  // decision: nothing, a marker move inside the current frame, or a full
  // viewport reset. Every fix channel goes through here -- the decision must not
  // be duplicated per channel.
  void applyFix(int32_t latE7, int32_t lonE7, uint8_t headingStep, uint8_t seq);
  // Erases the marker from the frame on the panel (writing back the pixels
  // saved when it was drawn), redraws it at sx/sy, and refreshes only the
  // rectangles involved. Leaves the map, the compass, the readout and the
  // button hints exactly as they were -- including where the marker overlapped
  // them, which is why the saved patch is taken after all of them are drawn.
  void moveMarker(int16_t sx, int16_t sy, uint8_t headingStep);
  // The marker's halo box, the unit of everything partial: what is saved, what
  // is restored, what is refreshed. Fixed size, so one buffer fits any position.
  void markerRect(int cx, int cy, int& x, int& y, int& w, int& h) const;
  // Saves the framebuffer under the marker's box at cx/cy into markerPatch_.
  // False when the read did not fit or the box is off-panel, which is what
  // forces the next fix to re-anchor instead of leaving a marker behind.
  bool saveMarkerPatch(int cx, int cy);
  // headingStep is always 0-15 (MapHeading's domain) -- callers do any
  // channel-specific conversion before calling this, so the projection and
  // the debug readout only ever see one heading representation.
  void renderViewport(int32_t latE7, int32_t lonE7, uint8_t headingStep, uint8_t seq);
  // The whole route on one screen: MapRouteFit picks the rung, the heading and
  // the anchor, and the anchor is the screen centre rather than the marker
  // ladder's rung -- an overview has no look-ahead to reserve because it has no
  // rider position in it (MapRouteFit.h).
  //
  // This is the first frame after a route is picked, and the map menu's "Whole
  // route" row draws it again at any time. It is deliberately not followable:
  // overviewShown_ holds until a button asks for the normal map back, so a fix
  // arriving two seconds later cannot yank the frame away while the rider is
  // still reading it.
  void renderRouteOverview();
  // Shared by the follow frame and the overview: points the tile source at
  // `range`, streams it through MapRenderer with the route drawn on top, then
  // hatches whatever was missing and records it. Returns the unavailable mask.
  //
  // One function rather than two, because "which tiles were missing" and "the
  // route is a layer over the tiles" are properties of the map, not of which
  // kind of frame is being drawn.
  // `timing`, when given, is filled with per-layer milliseconds for this frame
  // (MapRenderer.h). Instrumentation: it changes no pixel and costs one clock
  // call per layer. The route overview passes nullptr -- it is not on the path
  // whose cost is being tracked.
  uint32_t drawMapLayers(const MapViewport::TileRange& range, IMapCanvas& canvas, const MapViewState& view,
                         MapRenderTiming* timing = nullptr, uint64_t knownBadLayers = 0);
  // Re-renders the last received fix at the current ladder steps and mode.
  // This is what a zoom or marker step produces: the reset re-anchors on the
  // marker, which is the point -- zooming out must show more of the road
  // ahead, not more of wherever the marker has drifted to.
  void renderCurrent();
  // Draws one line of the debug readout, trimmed to the screen width.
  // Mutates `text` in place.
  void drawDebugLine(int y, char* text);
  // Top-right north indicator. The map is drawn track-up, so this rotates: the
  // whole glyph turns about its own centre by the frame's heading, which is
  // what makes it point at true north instead of up the screen. Not static
  // furniture -- it is only correct for the heading the frame was drawn with
  // (anchorHeading_), never for the newest fix.
  void drawCompass(uint8_t headingStep);
  // Which way is up on the frame about to be drawn: the route's direction while a
  // route holds the frame, otherwise the rider's own heading. Everything that has
  // to agree about "up" -- the projection, MapViewState::heading, the compass and
  // anchorHeading_ -- goes through this one answer.
  uint8_t frameHeadingFor(uint8_t fixHeadingStep) const;
  // True while the loaded route owns the frame's orientation, which is also what
  // stops a heading change from re-anchoring (MapFollow::Request::routeHoldsFrame).
  bool routeHoldsFrame() const { return route_ != nullptr && routeFrameHeadingValid_; }
  // Marker moves this frame may spend before a clean one is forced. One source
  // for the decision and for the log line that reports it -- they disagreed once,
  // and a log that reports the wrong budget is worse than none.
  uint16_t partialMoveBudget() const {
    return routeHoldsFrame() ? MapFollow::kRouteFramePartialMoves : MapFollow::kMaxPartialMoves;
  }
  // Battery and BLE status row, top-right, above the compass. Battery mirrors
  // the block GUI.drawHeader() draws on every other screen (BaseTheme.cpp:363)
  // so this reads the same; the BLE logo and signal bars are this screen's
  // own -- no other activity has a wireless link to show.
  void drawHeaderStatus();
  // Up/Down are physical side buttons -- GUI.drawButtonHints()'s four front-
  // button boxes never mention them. Calls the theme's own
  // drawSideButtonHints() for the matching side-hint boxes.
  void drawZoomSideHints();
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
  // (cycle) or large arrow (ride). headingStep here is **relative to the
  // frame's own orientation** (MapFollow::relativeHeadingStep), not the raw
  // incoming heading: the map is track-up, so a fix matching the frame's
  // heading points straight up, and a rider who has turned since the frame was
  // drawn gets an arrow that shows exactly that turn.
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
  // The loaded route, and its own file handle. **Its own**, not the tile
  // source's: both stream during a render and one seek cursor cannot serve two
  // readers (MapRouteSource.h). Null when the rider skipped the picker, which is
  // what makes the route layer cost nothing when there is no route.
  std::unique_ptr<HalFileSource> routeFile_;
  std::unique_ptr<MapRouteSource> route_;
  // What RouteSelectActivity picked, empty for none. Copied rather than held by
  // pointer: the activity that chose it is deleted the moment this one is
  // constructed (main.cpp's exitActivity).
  char routePath_[MapRouteSource::kMaxPathLen] = {};
  // True while the panel holds the route overview rather than a follow frame.
  // Fixes are still recorded in that state but do not redraw -- see
  // renderRouteOverview().
  bool overviewShown_ = false;
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

  // ## Follow state: what the frame currently on the panel is
  //
  // Every one of these describes the picture the panel is holding, not the
  // newest fix. A marker move reads them to work out what to erase and how to
  // orient the arrow; a viewport reset is the only thing that rewrites them.
  //
  // True once a real map frame is up. The waiting banner and the persisted-fix
  // frame are not followable: one has no map under it at all, the other carries
  // a banner that only a full redraw can clear.
  bool viewportDrawn_ = false;
  // The heading the frame was drawn track-up with. proj_ is rotated by it, so
  // it is also the frame's "up".
  uint8_t anchorHeading_ = 0;
  // With a route loaded, the frame's "up" is the route's own direction and stays
  // that way for every reset -- docs/route-navigation.md, "The decision". Taken
  // from MapRouteFit, which measures the route's point set per heading and breaks
  // ties on its direction of travel, so this is already "the way this route
  // runs". Without it a keep-in reset would re-orient to the rider's heading and
  // hand back the rotating map the frozen frame exists to stop.
  uint8_t routeFrameHeading_ = 0;
  bool routeFrameHeadingValid_ = false;
  // Where the marker is drawn, in screen pixels. Starts at the ladder anchor
  // after a reset and walks from there.
  int16_t markerDrawnX_ = 0;
  int16_t markerDrawnY_ = 0;
  // The framebuffer under the marker's halo box, as it was before the marker
  // was drawn over it. Restoring this is the only way to erase the marker
  // without re-reading tiles: the map pixels it covered exist nowhere else
  // (single-buffer mode, no shadow copy). One fixed-size box, allocated once in
  // onEnter() next to the tile source -- 720 bytes, against a full frame's
  // 48,000, which is the whole point.
  std::unique_ptr<uint8_t[]> markerPatch_;
  size_t markerPatchCapacity_ = 0;
  // False when nothing valid is saved, which forces the next fix to re-anchor
  // rather than leave a stale marker on the panel.
  bool markerPatchValid_ = false;
  // Marker moves since the last full frame. Windowed refreshes are
  // differential and ghost, so this is a budget (MapFollow::kMaxPartialMoves,
  // or kRouteFramePartialMoves once a route holds the frame -- a leg is worth
  // far more moves than a free-ride frame).
  uint16_t partialMoves_ = 0;

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
  // Own, much longer deadline: MissingTilesStore's list changes far less
  // often than a ladder step, and a rider crossing a coverage gap for
  // minutes must not turn into an SD write every kSaveSettleMs. Armed only
  // once, by the first new tile since the last flush -- see renderViewport().
  uint32_t missingTilesSaveDueMs_ = 0;

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

  // Map files pushed over the same BLE connection the position packets use.
  // Attached while this screen is up and only while it is up: the receiver
  // writes to the card from a BLE callback, and the card is the map's own
  // (MapTransferReceiver.h).
  MapTransferReceiver transfer_;
};
