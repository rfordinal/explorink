#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>

#include "HalFileSource.h"
#include "MapBleConsole.h"
#include "MapModeMask.h"
#include "MapProjection.h"
#include "MapSerialConsole.h"
#include "MapTileSource.h"
#include "MapTransferReceiver.h"
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
// | CONFIRM        | map menu: Refresh, Mode, Fetch missing tiles          |
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
// ## Fetching missing tiles is a state of this screen, not its own activity
//
// The rider asks for the gaps to be filled from CONFIRM's menu, and a progress
// screen replaces the map until the phone is done. That progress screen lives
// **inside this activity**, which looks like the wrong shape next to
// FontDownloadActivity's dedicated screen -- but a pushed activity would run
// MapActivity::onExit(), and onExit() calls transfer_.detach() and
// BlePositionServer::end(). The transfer channel the fetch depends on is owned
// by this screen's lifetime (onEnter(): begin() then attach()), so a second
// activity would tear down the link it needs before drawing its first frame.
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

  // --- Fetching missing tiles (see the header comment above) -----------

  // Menu action. Orders the store by fetch priority, tells the phone how many
  // tiles it is being asked for (`NEED_TILES <n>` on the command channel) and
  // puts the progress screen up. Ends immediately in Finished, with a reason
  // on screen, when there is nothing to fetch or nobody is listening.
  void startFetch();
  // Leaves the progress screen and paints the map back. Sends `FETCH_CANCEL`
  // only when a fetch was still running -- the phone must stop pushing, but a
  // fetch that already finished has nothing to cancel.
  void endFetch();
  // Full-screen first frame of the progress screen.
  void renderFetchScreen();
  // Repaints only the progress panel, and only when a tile has landed or been
  // skipped since the last repaint. Deliberately not per chunk: the panel is a
  // windowed e-ink refresh, and a byte counter is not worth one.
  void updateFetchProgress();
  // Panel rectangle in logical screen coordinates -- the region
  // updateFetchProgress() repaints.
  void fetchPanelRect(int& x, int& y, int& w, int& h) const;
  // Draws the panel's contents into the current frame. No refresh of its own,
  // so both the full first frame and the windowed update can use it.
  void drawFetchPanel();
  // Clears MissingTilesStore entries for tiles that have landed over BLE.
  //
  // On the activity task on purpose: renderViewport()'s record() calls make
  // this task the store's only writer, and a second writer on the NimBLE host
  // task would corrupt the vector. The receiver publishes a coordinate
  // (MapTransferReceiver::Status::lastTile) and this reads it.
  void drainTransferredTiles();

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
  // differential and ghost, so this is a budget (MapFollow::kMaxPartialMoves).
  uint8_t partialMoves_ = 0;

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

  // Where the fetch progress screen is. Off means the map owns the panel.
  // Finished keeps the screen up with its verdict until the rider presses
  // Back -- a fetch that ends on its own must not silently vanish.
  enum class FetchPhase : uint8_t { Off, Running, Finished };
  FetchPhase fetchPhase_ = FetchPhase::Off;
  // Tiles this fetch asked for: the store's size at the moment it started.
  // Fixed for the run, so entries the arrivals remove do not shrink the
  // denominator under the progress bar.
  uint32_t fetchTotal_ = 0;
  // transfer_'s completed counter when this fetch started. The receiver counts
  // every file since the screen opened, including earlier pushes, so progress
  // is the difference rather than the raw count.
  uint32_t fetchBaseCompleted_ = 0;
  // What the panel currently shows, so updateFetchProgress() can tell a real
  // change from a poll. Bytes deliberately not among them -- see that function.
  uint32_t fetchDrawnDone_ = 0;
  uint32_t fetchDrawnFailed_ = 0;
  // Which end state Finished is showing: a completed run, an empty list, or
  // nobody on the other end. Only read while fetchPhase_ == Finished.
  StrId fetchVerdict_ = StrId::STR_MAP_FETCH_DONE;

  // Last MapTransferReceiver::Status::tileSeq this task has already cleared out
  // of MissingTilesStore. See drainTransferredTiles().
  uint32_t lastClearedTileSeq_ = 0;

  // Map files pushed over the same BLE connection the position packets use.
  // Attached while this screen is up and only while it is up: the receiver
  // writes to the card from a BLE callback, and the card is the map's own
  // (MapTransferReceiver.h).
  MapTransferReceiver transfer_;
};
