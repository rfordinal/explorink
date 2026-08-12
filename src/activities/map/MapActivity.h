#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>

#include "CrossPointSettings.h"
#include "HalFileSource.h"
#include "MapBleConsole.h"
#include "MapFollow.h"
#include "MapMarkerMetrics.h"
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
// Follow mode (the default):
//
// | UP / DOWN      | zoom ladder, 5 rungs, 1..20 m/px                     |
// | LEFT / RIGHT   | marker-height ladder, 5 rungs, look-ahead 50..95 %   |
// | CONFIRM        | open the map menu: Refresh, Mode, zoom/rotation/     |
// |                | heading mode toggles (ride/hike/cycle)               |
// | BACK           | leave (or close the menu, if it is open)             |
//
// Observe mode (menu's "Observation mode" row, MapScreenMode::Observe): the
// same four direction buttons are re-read as a pan, not a ladder step -- each
// press moves the viewport half a screen the way it points (panBy()), and the
// hints/side-hints repaint to say so (renderViewport()'s button-hint switch).
// GPS fixes are still recorded (applyFix()) but never redraw the frame out
// from under a rider who is looking around; picking "Follow mode" from the
// same menu snaps straight back to wherever the last fix actually was.
//
// Any of those that triggers a redraw first paints an hourglass badge above the
// button hints and refreshes only its rectangle (showBusy()). A ladder step
// waits out the settle timer and then spends the better part of two seconds on
// tiles and the refresh, which is long enough to read as a dead button. One
// badge per burst, cleared by the frame that replaces it. A pan step is not on
// that settle timer -- see panBy()'s own comment for why coalescing it would
// either do nothing or be wrong.
//
// **There is no spare button** (docs/architecture-plan.md, "The map screen's
// button budget is exactly full"). CONFIRM stays the only entry point for
// anything new -- it opens a menu (OptionPopup) rather than acting directly,
// so Refresh, Mode and Observation mode share the one button the budget
// allows.
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
//
// ## Autosync: asking the phone for a tile the moment one is hatched
//
// Off unless SETTINGS.mapAutoSyncTiles says otherwise. On, a frame that had to
// hatch anything asks the connected phone for exactly those tiles
// (maybeAutoSyncTiles()), a globe in the header says data is moving, and the
// map redraws once the answers are in. Everything it needs was already on this
// screen: the BLE peripheral, the transfer receiver, the command console the
// phone answers on, and the store the hatch loop already writes to.
//
// Three rules keep it from being a nuisance, and none of them is optional:
//
// - **Viewport only.** `NEED_TILES <n> fmt <v> view` tells the phone to read
//   the current screen with `tiles`, not to page the whole 200-entry
//   `missing` list. What is hatched in front of the rider is the urgent part;
//   the rest is what the tile sync screen is for.
// - **A refused tile backs off; it is not asked for again straight away.** The
//   phone answering `skip` schedules the entry
//   (MissingTilesStore::markRefused()), and a tile inside its delay does not
//   count toward the next ask. Without that a rider parked at the edge of
//   coverage re-hatches the same squares on every viewport reset and begs for
//   them forever, on the phone's mobile data. The delay doubles per refusal from
//   90 s to an hour and then stays there, because a refusal now means "the CDN
//   has not built it *yet*" -- the phone's own 404 is what queues the build
//   (../../../docs/tile-autobuild.md) -- rather than "nobody will ever have it".
// - **One ask per kAutoSyncIntervalMs.** A rate cap, not a settle timer: the
//   next ask is not pushed further out by more hatching.
class MapActivity final : public Activity, public IMapSkipObserver, public IMapStaleObserver {
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

  // IMapSkipObserver -- the phone saying it cannot supply one tile. Marks the
  // entry refused so autosync stops asking for it, and settles the row against
  // the ask that is outstanding.
  void onTileSkipped(uint8_t z, uint32_t col, uint32_t row) override;

  // IMapStaleObserver -- the phone's verdict on the tiles this screen already
  // holds. onTileStale() only records; the fetch goes out from loop(), where
  // the rate cap and the link state live (docs/tile-freshness.md).
  void onTileStale(uint8_t z, uint32_t col, uint32_t row) override;
  void onCheckFinished(bool known, uint16_t staleCount) override;

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
  // The marker's dimensions at the rung on the panel now
  // (MapViewport::ZoomStep::markerScale8). Never full-size constants directly:
  // rungs 5 and 6 draw a smaller marker, because a fixed pixel object covers
  // more ground the further out the rung is.
  MarkerMetrics markerMetrics() const;
  // The marker's halo box, the unit of everything partial: what is saved, what
  // is restored, what is refreshed. Sized by the rung the marker on the panel
  // was drawn at (markerBoxDrawn_), so a save and its erase always agree; the
  // patch buffer itself is allocated for the largest rung, so one buffer fits
  // any position at any rung.
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
  // `nearestOut`, when given, is filled with the header's place-name lookup
  // (MapRenderer.h, MapNearestPlaces) from the same places walk the dots come
  // from -- no second SD read.
  uint32_t drawMapLayers(const MapViewport::TileRange& range, IMapCanvas& canvas, const MapViewState& view,
                         MapRenderTiming* timing = nullptr, MapLayerBits knownBadLayers = {},
                         MapNearestPlaces* nearestOut = nullptr);
  // Re-renders the last received fix at the current ladder steps and mode.
  // This is what a zoom or marker step produces: the reset re-anchors on the
  // marker, which is the point -- zooming out must show more of the road
  // ahead, not more of wherever the marker has drifted to.
  void renderCurrent();
  // Draws one line of the debug readout, trimmed to the screen width, with a
  // white backing sized to its own text -- same reason the compass halo and
  // the header status row get one (MapActivity.cpp:765): the readout sits
  // over live map lines, not blank margin, and text drawn straight onto a
  // hatch or a road is unreadable. Mutates `text` in place (the trim).
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
  // Keeps frozenManualHeading_ current: captures the fix heading once, the
  // first time SETTINGS.mapHeadingMode reads MAP_HEADING_MANUAL, and clears
  // the capture (so switching to Manual again re-captures fresh) whenever it
  // reads MAP_HEADING_AUTO. Called at the top of applyFix() and from onEnter(),
  // both places a new headingStep becomes available.
  void updateManualHeadingCapture(uint8_t fixHeadingStep);
  // True while the loaded route owns the frame's orientation, which is also what
  // stops a heading change from re-anchoring (MapFollow::Request::routeHoldsFrame).
  bool routeHoldsFrame() const { return route_ != nullptr && routeFrameHeadingValid_; }
  // True while nothing on screen should rotate off of frameHeadingFor()'s last
  // answer because of the rider's own heading -- a route holding the frame, a
  // North-up rotation mode, or a frozen Manual heading. Same effect on
  // MapFollow::decide() as routeHoldsFrame() alone used to be
  // (Request::routeHoldsFrame suppresses the heading-drift ReAnchor check), so
  // applyFix() ORs this into that field rather than adding a second one.
  bool frameOrientationLocked() const {
    return routeHoldsFrame() || SETTINGS.mapRotationMode == CrossPointSettings::MAP_ROTATION_NORTH_UP ||
           SETTINGS.mapHeadingMode == CrossPointSettings::MAP_HEADING_MANUAL;
  }
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
  // The globe / Bluetooth logo / signal bars alone -- everything inside
  // headerStatusRect() and nothing outside it. Split out so the windowed
  // repaint can redraw exactly what it refreshes.
  void drawHeaderStatusStrip();
  // Left side of the header: the nearest named place to the marker
  // (MapRenderer.h, MapNearestPlaces / nearestPlaces_), truncated to fit
  // before the icon cluster. Draws nothing when nothing is loaded nearby.
  void drawHeaderPlaceName();
  // Up/Down are physical side buttons -- GUI.drawButtonHints()'s four front-
  // button boxes never mention them. Calls the theme's own
  // drawSideButtonHints() for the matching side-hint boxes.
  void drawZoomSideHints();
  // Five-segment black/white bar, bottom-left, plus tick marks and mark
  // labels rounded to a nice ground distance (1/2/5 x 10^n) for the current
  // zoom step's mpp (MapViewport::kZoomLadder).
  void drawMapScale();
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
  // Sends the ask, if the setting is on, a phone is listening, nothing is
  // already in flight and the rate cap has expired. Called from loop(), never
  // from the render path: the hatch loop only records what it would be worth
  // asking for (autoSyncWantCount_).
  void maybeAutoSyncTiles();
  // Sibling of maybeAutoSyncTiles(), and deliberately not folded into it: one
  // asks for tiles the device does not have, the other asks whether the ones it
  // does have are still current. Different setting, different cooldown,
  // different question.
  void maybeCheckTileFreshness();
  // `NEED_TILES <count> fmt <version> view` -- the `view` word is what tells
  // the phone to answer from `tiles` (this screen) rather than page `missing`
  // (the tile sync screen). docs/ble-map-transfer-protocol.md.
  void askForViewportTiles(uint32_t count);
  // Clears MissingTilesStore entries for tiles that have landed, and settles
  // them against the outstanding ask.
  //
  // On the activity task, never in the BLE callback: this and
  // renderViewport()'s record() are the store's only writers, and a second
  // writer on the NimBLE host task would corrupt the vector. The receiver
  // publishes a coordinate plus a sequence number and this acts on the change
  // (MapTransferReceiver::Status::lastTile).
  void drainTransferredTiles();
  // Gives up on an ask nothing has answered, so the globe cannot stay lit
  // forever after the phone walks away mid-transfer.
  void expireAutoSync();
  // Keeps the header status row honest between full frames: the globe, the
  // link state and the signal bars. Refreshes only the header strip, and only
  // when something actually changed -- same windowed mechanism as the busy
  // badge.
  //
  // Without this the row is drawn only by a full frame, so a phone that
  // disconnects leaves its signal bars on the panel until something unrelated
  // forces a redraw. Seen in a real session, 2026-08-07.
  void updateHeaderStatus();
  // The globe's own slot, and the strip drawHeaderStatus() backs and repaints.
  // One source for both, or the repaint clips what the draw put down.
  void headerStatusRect(int& x, int& y, int& w, int& h) const;
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

  // CONFIRM's menu: Refresh, Mode, Observation/Follow mode, and the
  // zoom/rotation/heading mode toggles, one flat list, no second popup. Every
  // row commits and closes on one Select -- picking Mode steps
  // ride->hike->cycle->ride and is done, same as any other row; a rider who
  // wants a different mode (or a different zoom/rotation/heading value)
  // again presses CONFIRM again. Draws the popup itself via
  // optionPopup_.processRender() right after show() -- MapActivity never
  // calls requestUpdate() (it always has drawn straight to the buffer, on
  // the main task, not through Activity's render(RenderLock&&)/render-task
  // path), so nothing else would ever paint the popup's first frame.
  void openMapMenu();
  // No-op if newMode is already current -- picking the mode already on
  // screen must cost nothing, same rule as stepZoom/stepMarker's ladder ends.
  void switchMode(MapRideMode newMode);

  // Follow is the normal ride/hike/cycle screen; Observe repurposes the four
  // direction buttons to pan the viewport instead of stepping the zoom/marker
  // ladders. Not MapRideMode -- that picks *what* the frame is for, this picks
  // *what the buttons do*, and the two are independent (a rider can look
  // around in any ride mode).
  enum class MapScreenMode : uint8_t { Follow, Observe };
  // Left/Right pan along the frame's own horizontal; Up/Down along its
  // vertical. Screen-space, not compass directions -- track-up means "right"
  // is whatever the current heading makes it (panBy()).
  enum class PanDirection : uint8_t { Left, Right, Up, Down };
  // Toggles screenMode_. Entering Observe stores the fix currently in effect
  // (observeReturnLatE7_ etc.) so leaving it can render exactly that fix
  // rather than wherever the rider last panned to -- same idea as
  // overviewShown_, just with its own return coordinate instead of reusing
  // lastLatE7_/lastLonE7_, which panBy() repoints at the pan target instead.
  // No-op entering with no fix yet (hasReceivedAny_ false): nothing to look
  // around in that case, same "cost nothing" rule as switchMode().
  void toggleObserveMode();
  // One half-screen step in `direction`, reusing the projection the frame on
  // screen was actually drawn with (proj_) -- whether that frame came from a
  // real fix or the previous pan step. See the .cpp for why this is not on
  // the ladder steps' settle timer.
  void panBy(PanDirection direction);
  // Up/Down side-button hints while Observe is active: same box as
  // drawZoomSideHints(), different glyphs, because the side buttons pan
  // instead of zooming.
  void drawPanSideHints();

  // Allocated once in onEnter(), released in onExit(). MapTileSource holds
  // references to both, so neither may move or die while it is alive.
  std::unique_ptr<HalFileSource> file_;
  std::unique_ptr<MapTileSource> source_;
  // The loaded route, and its own file handle. **Its own**, not the tile
  // source's: both stream during a render and one seek cursor cannot serve two
  // readers (MapRouteSource.h). Null when the rider skipped the picker, which is
  // what makes the route layer cost nothing when there is no route.
  // Place-name layout scratch, allocated in onEnter() only when the compiled
  // style draws labels at all. Null means place dots and no names (MapLabels.h).
  std::unique_ptr<MapLabelScratch> labels_;
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

  // Which way the four direction buttons are wired up right now. Independent
  // of overviewShown_/mode_ -- see the enum's own comment.
  MapScreenMode screenMode_ = MapScreenMode::Follow;
  // The fix Observe mode returns to when the rider picks "Follow mode" again.
  // Set once on entry and kept current by applyFix() while Observe is active
  // (both in toggleObserveMode()) -- never written by panBy(), which repoints
  // lastLatE7_/lastLonE7_/lastHeading_ at the pan target instead. Two separate
  // coordinates for two separate questions: "what is on screen" (lastLatE7_)
  // vs. "where is the rider actually" (these).
  int32_t observeReturnLatE7_ = 0;
  int32_t observeReturnLonE7_ = 0;
  uint8_t observeReturnHeading_ = 0;
  uint8_t observeReturnSeq_ = 0;

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
  // Same idea as routeFrameHeading_, for SETTINGS.mapHeadingMode ==
  // MAP_HEADING_MANUAL instead of a loaded route: the heading frameHeadingFor()
  // returns while the rider has frozen the frame by hand. Captured once, by
  // updateManualHeadingCapture(), from the fix heading in effect at the moment
  // Manual was switched on -- not editable from a button yet (no spare one,
  // MapActivity.h's button table), so this only stops the frame from following
  // the rider; it does not yet let them re-aim it.
  uint8_t frozenManualHeading_ = 0;
  bool manualHeadingCaptured_ = false;
  // Where the marker is drawn, in screen pixels. Starts at the ladder anchor
  // after a reset and walks from there.
  int16_t markerDrawnX_ = 0;
  int16_t markerDrawnY_ = 0;
  // The box size the marker on the panel was drawn with. 0 until the first
  // marker goes down. Held rather than recomputed so that an erase can never be
  // sized by a rung the picture was not drawn at -- see markerRect().
  int16_t markerBoxDrawn_ = 0;
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

  // ## Autosync state (see the class comment)
  //
  // Tiles the last frame hatched that are worth asking for -- hatched, and not
  // already refused by the supplier. 0 means there is nothing to ask for.
  // Written by drawMapLayers(), consumed and cleared by maybeAutoSyncTiles().
  uint32_t autoSyncWantCount_ = 0;
  // Tiles asked for and not yet settled by an arrival or a `skip`. Non-zero is
  // exactly the condition the globe shows.
  uint32_t autoSyncPending_ = 0;
  // True once at least one tile of this ask has actually landed, so the map is
  // redrawn when the ask settles -- and is not redrawn when every answer was
  // "not available", which would spend two seconds of waveform on the same
  // picture.
  bool autoSyncArrived_ = false;
  // millis() deadlines; 0 means nothing armed. The interval is a rate cap on
  // asks, the deadline is the give-up on one ask.
  uint32_t autoSyncNextAskMs_ = 0;
  uint32_t autoSyncDeadlineMs_ = 0;
  // Last tileSeq already cleared out of the store (drainTransferredTiles()).
  uint32_t lastClearedTileSeq_ = 0;
  // Deadline for the redraw a tile arrival owes, pushed out by each further
  // arrival so a burst costs one frame. 0 means none owed.
  uint32_t arrivalRedrawDueMs_ = 0;
  // Bytes the receiver had moved when the ask's quiet timer was last rearmed.
  // The ask expires on silence, not on elapsed time (expireAutoSync()).
  uint32_t lastTransferProgress_ = 0;

  // ## Tile freshness state (SETTINGS.mapTileFreshnessMode, docs/tile-freshness.md)
  //
  // In memory and never persisted -- see StaleTilesList for why this is not
  // MissingTilesStore.
  StaleTilesList staleTiles_;
  // The viewport's tiles and the content_id each was opened at, refreshed by
  // every reset. What `have` answers from, and what CHECK_TILES counts.
  MapHeldTiles heldTiles_;
  // The header's place-name lookup, refreshed by drawMapLayers() every reset
  // (MapRenderer.h, MapNearestPlaces) and read back by drawHeaderStatus() --
  // set here rather than passed as a return value because drawMapLayers()
  // already returns the missing-tile mask.
  MapNearestPlaces nearestPlaces_;
  // True between `CHECK_TILES` going out and `checked` coming back. One check
  // at a time: a second ask would list a viewport the first one is still
  // answering for.
  bool freshnessPending_ = false;
  // millis() rate cap on how often the device may start a check, and the
  // give-up on one that is never answered. 0 means nothing armed.
  uint32_t freshnessNextAskMs_ = 0;
  uint32_t freshnessDeadlineMs_ = 0;
  // Throttles the gate-reason log in maybeCheckTileFreshness() -- that check
  // runs every tick, and without this a blocked gate (no phone subscribed, no
  // tiles held) would print on every loop() instead of at a readable rate.
  uint32_t freshnessLastGateLogMs_ = 0;
  // ## What the header status row currently has on it
  //
  // Not the state itself -- autoSyncPending_ and the BLE server are. These are
  // what was last *drawn*, so updateHeaderStatus() can tell a real change from
  // a poll. All three are written by drawHeaderStatusStrip(), on every path
  // that draws the row, including full frames.
  bool transferIconShown_ = false;
  bool drawnLinkConnected_ = false;
  // -1 means "never drawn", which is not the same as 0 bars.
  int drawnBleBars_ = -1;
  // millis() deadlines; 0 means due now. The poll interval bounds how often
  // rssi() is asked; the bars interval bounds how often a bar count that keeps
  // crossing a threshold may spend a waveform pass.
  uint32_t nextHeaderPollMs_ = 0;
  uint32_t nextBarsRepaintMs_ = 0;

  // One state, two channels. A `zoom 3` over USB and a `zoom 3` over BLE
  // land on the same number because they share this object, not because two
  // copies are kept in step (MapCommandConsole.h).
  MapConsoleState consoleState_;
  MapSerialConsole serial_{consoleState_};
  MapBleConsole ble_{consoleState_};

  // CONFIRM's menu (Refresh / Mode / Observation mode).
  OptionPopup optionPopup_;
  // Set when a Back press closes optionPopup_ (loop()), cleared by the one
  // Back release it is meant to swallow (also loop()) -- see the comment
  // there for why the release needs swallowing at all.
  bool suppressBackRelease_ = false;
  // Same problem, CONFIRM's side of it: a Select is a Confirm *press*
  // (OptionPopup.h), but handleButtons() opens the menu on a Confirm
  // *release* -- two edges of the same physical click. Left alone, the press
  // that picks a row closes the menu and the release that follows it, one
  // frame or a slow render later, reopens the very menu the row just picked
  // an action from. Set in loop() alongside suppressBackRelease_, cleared in
  // handleButtons() the one time it is meant to swallow.
  bool suppressConfirmRelease_ = false;

  // Map files pushed over the same BLE connection the position packets use.
  // Attached while this screen is up and only while it is up: the receiver
  // writes to the card from a BLE callback, and the card is the map's own
  // (MapTransferReceiver.h).
  MapTransferReceiver transfer_;
};
