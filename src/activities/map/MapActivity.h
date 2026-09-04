#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>

#include "CrossPointSettings.h"
#include "HalFileSource.h"
#include "MapBleConsole.h"
#include "MapCommandConsole.h"
#include "MapFollow.h"
#include "MapGnssHeading.h"
#include "MapMarkerMetrics.h"
#include "MapModeMask.h"
#include "MapPins.h"
#include "MapPointQuery.h"
#include "MapPointSource.h"
#include "MapProjection.h"
#include "MapRenderer.h"
#include "MapRouteSource.h"
#include "MapSerialConsole.h"
#include "MapTileSource.h"
#include "MapTransferReceiver.h"
#include "MapViewport.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

#ifdef ENABLE_GNSS_CMD
// A reference parameter needs no definition, and pulling GnssAccess.h in here
// would put the driver's whole header into every translation unit that draws a
// map screen. The definition arrives in MapActivity.cpp, which includes it.
struct GnssFix;
#endif

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
// | UP / DOWN      | zoom ladder, 7 rungs, 1..45 m/px                     |
// | LEFT / RIGHT   | marker-height ladder, 5 rungs, look-ahead 50..95 %   |
// | CONFIRM        | open the map menu: Refresh, Mode, zoom/rotation/     |
// |                | heading mode toggles (ride/hike/cycle)               |
// | BACK           | leave (or close the menu, if it is open)             |
//
// Observe mode (menu's "Observation mode" row, MapScreenMode::Observe): the
// same four direction buttons are re-read as a pan, not a ladder step -- each
// press moves the viewport 30 % of a screen the way it points (panBy()), and the
// hints/side-hints repaint to say so (renderViewport()'s button-hint switch).
// GPS fixes are still recorded (applyFix()) but never redraw the frame out
// from under a rider who is looking around; picking "Follow mode" from the
// same menu snaps straight back to wherever the last fix actually was.
//
// The zoom ladder is not lost in Observe, it moves onto a **hold**: Up or Down
// held past kObserveZoomHoldMs steps the same ladder it steps in Follow (in on
// Up, out on Down), one rung per hold, and the release that ends the hold does
// not also pan. A pan therefore fires on button *release* in this mode, not on
// press -- a press cannot be told from the start of a hold until it ends.
// The zoom re-anchors on the frame's own anchor point (renderCurrent() ->
// lastLatE7_, which panBy() has been repointing all along), so a rung change
// keeps whatever the rider panned to, rather than snapping back to the fix.
// A hold is invisible on a still panel, so the menu carries "Zoom in"/"Zoom
// out" rows in this mode as well (openMapMenu()) -- that is where a rider who
// never guesses the hold finds the ladder.
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
// (maybeAutoSyncTiles()), a transfer icon in the header says data is moving,
// and the map redraws once the answers are in. Everything it needs was already
// on this screen: the BLE peripheral, the transfer receiver, the command
// console the phone answers on, and the store the hatch loop already writes to.
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
class MapActivity final : public Activity, public IMapSkipObserver, public IMapStaleObserver, public IMapFakeSink {
 public:
  // `routePath` is an absolute card path to a .tir route, or nullptr for none.
  // RouteSelectActivity passes what the rider picked; every other caller --
  // `CMD:GOTO_MAP` over serial, the OOM fallbacks -- passes nothing and gets the
  // map exactly as it was before routes existed.
  MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* routePath = nullptr,
              bool resumedFromSleep = false);

  bool isMapActivity() const override { return true; }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Same mechanism CrossPointWebServerActivity/OtaUpdateActivity/etc. use --
  // don't let the device auto-sleep (and drop off USB) while the BLE
  // peripheral is running and might receive a position update any moment.
  bool preventAutoSleep() override;

  // But *do* let the CPU throttle. This screen is up for hours and spends
  // almost all of them waiting: run 2 measured 160 MHz for all but 0.02 % of a
  // 13 h day, with the panel busy 3.3 % of it (docs/power-plan.md).
  //
  // Returns true only while there is queued work a slow clock would make the
  // rider wait for. It carries no safety duty: HalPowerManager will not go
  // below BLE_SAFE_FREQ while the BT controller is enabled, so the radio is
  // protected by the floor rather than by what this returns. An earlier
  // attempt put that duty here, keyed it on a cached view of the link, and
  // hung the device when the view went stale.
  bool preventThrottle() override;

  // IMapSkipObserver -- the phone saying it cannot supply one tile. Marks the
  // entry refused so autosync stops asking for it, and settles the row against
  // the ask that is outstanding.
  void onTileSkipped(uint8_t z, uint32_t col, uint32_t row) override;

  // IMapStaleObserver -- the phone's verdict on the tiles this screen already
  // holds. onTileStale() only records; the fetch goes out from loop(), where
  // the rate cap and the link state live (docs/tile-freshness.md).
  void onTileStale(uint8_t z, uint32_t col, uint32_t row) override;
  void onCheckFinished(bool known, uint16_t staleCount) override;

  // IMapFakeSink -- seeds a grid for the tile sync screen to draw. This screen
  // is the one that has the projection and MISSING_TILES, which is why the sink
  // lives here rather than on the screen that shows the result.
  void seedFakeTiles(uint16_t missing, uint16_t held, uint16_t& seededMissing, uint16_t& seededHeld) override;

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
#ifdef ENABLE_GNSS_CMD
  // Reads the on-device receiver and feeds applyFix() when it has a sample this
  // activity has not seen. The third caller of applyFix(), next to the BLE
  // packet and the command console -- see the comment on the definition for why
  // that is the whole integration.
  void pollGnssFix();
#endif
#ifdef ENABLE_GNSS_CMD
  // What the header row's GNSS glyph says. Three states, one Lucide glyph each
  // (locate-off, locate, locate-fixed).
  enum class GnssHeaderState : uint8_t { Off, Seeking, Fixed };
  GnssHeaderState gnssHeaderState() const;
  // The 16-step heading to draw for a fix: the receiver's course while the
  // rider is moving, the last one while they are not. Not const -- it carries
  // the speed gate's hysteresis and the drawn step forward.
  uint8_t gnssHeadingStep(const GnssFix& fix);
#endif
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
  // Swaps the live per-mode marker for the small ring-and-dot of
  // MapMarkerMetrics.h's kSleepMarker* on the way into a quick-resume sleep, and
  // refreshes just that box. Called from onExit(), where the patch and the
  // renderer are both still alive and the frame about to be handed to
  // SleepActivity is the frame that will sit on the glass for the whole sleep.
  // No-op unless there is a marker on the panel and a valid patch to erase it
  // with -- the map under a marker exists nowhere else in single-buffer mode.
  void drawSleepMarker();
  // The sleep-style ring-and-dot, drawn at the *real* last fix
  // (observeReturnLatE7_/Lon_) projected into whatever the rider panned to,
  // while Observe is active. Not the anchor -- the anchor is a pan target,
  // not a fix, and drawPositionMarker()'s Observe guard is unchanged. Skips
  // silently when the fix projects off the current viewport, same as any
  // marker that would draw outside the panel.
  void drawObserveFixMarker();
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
  // The transfer icon / Bluetooth logo / signal bars alone -- everything inside
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
  //
  // Falls back to recheckHatchedTiles() when nothing was freshly hatched this
  // tick -- see that method for why a full re-render is not what this waits
  // for.
  void maybeAutoSyncTiles();
  // Re-derives autoSyncWantCount_ from the last reset's own tile range and
  // missing mask (lastTileRange_), instead of a fresh hatch.
  //
  // Why this exists: drawMapLayers() is the only place a tile's refusal is
  // re-checked against the clock (MapActivity.cpp, MISSING_TILES.isRefused()),
  // and it only runs on a full renderViewport() reset. Between resets,
  // MapFollow::Action::MoveMarker slides the marker with no re-render at all
  // (applyFix()) -- and with a route loaded, that can run for
  // MapFollow::kRouteFramePartialMoves fixes before a ReAnchor forces one
  // (MapActivity.h, partialMoveBudget()). At a zoomed-out rung and highway
  // speed that budget was measured taking 15+ minutes to exhaust (2026-08-14
  // report), which is much longer than a refused tile's own retry delay
  // (MissingTilesStore::kRefusalBaseMs..kRefusalMaxMs) -- so autosync could go
  // quiet for a tile the CDN had long since built, for as long as the rider
  // stayed on a straight leg.
  //
  // No card read and no redraw: just isRefused() over a mask already in RAM,
  // the same handful of integer compares maybeAutoSyncTiles() costs when idle.
  // Rate-limited the same way a fresh hatch is -- through autoSyncNextAskMs_
  // in maybeAutoSyncTiles(), not by its own timer -- so this cannot make
  // autosync ask any more often than it already does; it only changes what
  // feeds the want count on a tick that had no fresh one.
  uint32_t recheckHatchedTiles() const;
  // Sibling of maybeAutoSyncTiles(), and deliberately not folded into it: one
  // asks for tiles the device does not have, the other asks whether the ones it
  // does have are still current. Different setting, different cooldown,
  // different question.
  void maybeCheckTileFreshness();
  // `NEED_TILES <count> fmt <version> view` -- the `view` word is what tells
  // the phone to answer from `tiles` (this screen) rather than page `missing`
  // (the tile sync screen). docs/ble-map-transfer-protocol.md.
  void askForViewportTiles(uint32_t count);
  // `DIAG_M <metres>` -- the ground distance the current zoom rung's screen
  // diagonal represents, so the phone can size its GPS send-move threshold to
  // what the panel can actually show instead of one constant guessing at every
  // rung (docs/send-interval-analysis.md in the parent repo). Sent once per
  // rung change and once per reconnect; no-op otherwise. No reply expected,
  // same shape as FETCH_CANCEL, not a listing -- docs/ble-map-transfer-protocol.md.
  void sendViewportDiagonalIfChanged();
  // Clears MissingTilesStore entries for tiles that have landed, and settles
  // them against the outstanding ask.
  //
  // On the activity task, never in the BLE callback: this and
  // renderViewport()'s record() are the store's only writers, and a second
  // writer on the NimBLE host task would corrupt the vector. The receiver
  // publishes a coordinate plus a sequence number and this acts on the change
  // (MapTransferReceiver::Status::lastTile).
  void drainTransferredTiles();
  // Gives up on an ask nothing has answered, so the transfer icon cannot stay
  // lit forever after the phone walks away mid-transfer.
  void expireAutoSync();
  // Keeps the header status row honest between full frames: the transfer icon,
  // the link state and the signal bars. Refreshes only the header strip, and only
  // when something actually changed -- same windowed mechanism as the busy
  // badge.
  //
  // Without this the row is drawn only by a full frame, so a phone that
  // disconnects leaves its signal bars on the panel until something unrelated
  // forces a redraw. Seen in a real session, 2026-08-07.
  void updateHeaderStatus();
  // Is Observe's clock currently showing a withheld units digit? See
  // docs/map-header-status.md, "Proposed: a coarse clock in Observe".
  bool clockIsCoarse() const;
  // The clock value to compare and to print, quantised per clockIsCoarse().
  // -1 when the phone has never sent a time. Writes the raw local time out so a
  // caller that needs the hour does not have to ask twice.
  int16_t clockTick(uint32_t& localNowOut) const;
  // Maps a live rssi() reading to a bar count, holding the last real one
  // across a failed (0) read. See MapActivity.cpp for why.
  int resolveBleBars(int8_t rssi);
  // The transfer icon's own slot, and the strip drawHeaderStatus() backs and repaints.
  // One source for both, or the repaint clips what the draw put down.
  void headerStatusRect(int& x, int& y, int& w, int& h) const;
  // Height of the header bar on the panel right now: the base single-row bar
  // in Ride/Cycle, one kHeaderRowHeight taller in Hike to hold the elevation
  // line. Everything that used to read the file-scope kHeaderBarHeight
  // constant for "where does the map start" now reads this instead, so a mode
  // switch moves the map viewport, the separator and the marker-bounds check
  // together. Text *inside* row one (the place name, the battery/BLE icons)
  // still centres against the unchanged single-row constant -- only the
  // content boundary below the whole bar depends on mode_.
  int headerBarHeight() const;
  // The 1px black separator's y, same value as headerBarHeight() -- kept as
  // its own call so a read site says which one it means without doing the
  // arithmetic itself.
  int headerSeparatorY() const;
  // First row the map may draw into. GfxRendererCanvas's minY clips there, so
  // in Hike mode the map starts one kHeaderRowHeight lower than in Ride/Cycle.
  int mapContentTop() const;
  // Hike-only second header line: elevation (from whichever fix last carried
  // one -- BLE or the serial/BLE command console) and the fix's own lat/lon,
  // already known from lastLatE7_/lastLonE7_. Ride and Cycle draw nothing here
  // -- see docs/map-header-status.md, "Hike mode's second line".
  void drawHikeElevationLine();
  // Keeps the hike line honest between full frames, the same windowed-repaint
  // shape updateHeaderStatus() uses for the row above: polled, not checked
  // every tick, and repainted only when the text it would draw actually
  // changed.
  void updateHikeElevationLine();
  void showBusy();

  // Raise the CPU back to full speed before anything the rider waits on.
  // Called at the top of every render entry point.
  //
  // This is about latency, not safety. With preventThrottle() false the screen
  // sits at BLE_SAFE_FREQ (80 MHz), where a viewport reset -- close to two
  // seconds at 160 MHz, much of it software floating point -- would take
  // roughly twice as long. A fix can also arrive and force a redraw inside the
  // same loop() iteration whose preventThrottle() was already polled, so the
  // render paths cannot rely on that flag alone.
  //
  // The main loop drops the clock again once IDLE_POWER_SAVING_MS passes with
  // nothing asking for it (main.cpp).
  static void kickFullClock();
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
  // The map pixels the menu is about to cover, saved so closing the menu costs
  // one window refresh instead of a full re-render (tiles off the card, then a
  // whole-panel waveform -- seconds). Allocated on open, freed on close, so the
  // ~26 KB it needs at the biggest dialog size is held only while the menu is
  // up; on OOM the capture simply fails and every close falls back to
  // renderCurrent(), the behaviour before this existed.
  bool captureMenuBackdrop();
  // Heap that must survive taking the backdrop. Everything that runs while the
  // menu is up -- BLE tile transfers, the console, a settings write -- draws
  // from the same pool.
  static constexpr uint32_t kMenuBackdropHeapReserve = 24 * 1024;
  // Puts the saved pixels back, repaints the map's own button hints over the
  // popup's, and refreshes just that window. False when there is nothing saved
  // or the write did not fit -- caller then does a full redraw.
  bool restoreMenuBackdrop();
  void dropMenuBackdrop();
  // The map's own four button hints for the current screen mode. Shared by the
  // full render and the menu-close restore, so the two cannot disagree about
  // what the buttons say.
  void drawMapButtonHints();
  // No-op if newMode is already current -- picking the mode already on
  // screen must cost nothing, same rule as stepZoom/stepMarker's ladder ends.
  void switchMode(MapRideMode newMode);

  // Which pins popup the next loop() should open. A row callback may not open one
  // itself: OptionPopup invokes the callback from inside handleInput(), and a
  // show() from there reassigns the very std::function that is executing -- it
  // destroys the running callable under its own call. So a callback records what
  // it wants here and returns, and loop() opens it one iteration later.
  enum class PinPopup : uint8_t { None, List, AddList, Offscreen, ConfirmSet, ConfirmDelete, Show, Save };
  PinPopup pendingPinPopup_ = PinPopup::None;
  uint8_t pendingPinArg_ = 0;
  // The map menu's own dialog size, so every pins list opens at exactly that size
  // instead of shrinking to its own content. Two reasons: a differently sized box
  // in the middle of the previous one reads as a different kind of dialog rather
  // than the next step of the same one, and a same-or-smaller dialog keeps the
  // menu backdrop valid, which is what makes the close cheap.
  // Which row the off-screen list reopens on after a toggle, so flipping four pins
  // does not walk back down the list four times.
  uint8_t pinsOffscreenRow_ = 0;
  int menuDialogWidth_ = 0;
  int menuVisibleRows_ = 0;

  // ## Nearby (../../docs/nearby-menu.md)
  //
  // The POI browser over the point layer: what useful things are around the
  // rider, answered from the GPS fix over a 25 km radius
  // (../../../docs/safety-concept.md, "Nearby"). OptionPopup inside this
  // activity for the same reason Pins is: leaving the map screen would drop the
  // phone link this activity owns.
  //
  // Three screens, and the popups are opened through pendingNearbyPopup_ from
  // loop(), never from inside a popup callback -- show()ing from a callback
  // reassigns the std::function currently running (PinPopup says the same).
  enum class NearbyPopup : uint8_t { None, Menu, Category, Detail };
  NearbyPopup pendingNearbyPopup_ = NearbyPopup::None;
  uint8_t pendingNearbyArg_ = 0;
  void servicePendingNearbyPopup();
  void openNearbyMenu();
  void openNearbyCategoryList(uint8_t category);
  void openNearbyPointDetail(uint8_t hitIndex);
  // `View on map`: turns the category's layer on, remembers which point was
  // asked for so it can be marked apart from its neighbours, and re-anchors the
  // frame on it in Observe mode.
  void viewNearbyPointOnMap(uint8_t hitIndex);
  // The ring around the point the rider asked to see, drawn straight onto
  // GfxRenderer after the map -- same place and same reason as drawPins(): the
  // renderer knows nothing about which POI a menu was pointing at.
  void drawViewedNearbyPoint();
  // Whether a windowed refresh of this rect can be afforded right now.
  //
  // `GfxRenderer::displayBufferWindow()` returns bool, and every call site here
  // handles false by falling back to a full refresh -- but the driver under it
  // allocates (w/8)*h bytes through a **throwing** std::vector
  // (freeink-sdk/.../Ssd1677Driver.cpp), and on a -fno-exceptions build a
  // throwing allocation is abort(), not false. So the affordability has to be
  // decided before the call, against the largest block the heap can actually
  // give (ESP.getMaxAllocHeap(), the same number the MEM log line prints).
  //
  // Measured 2026-08-22: a coredump caught exactly this -- abort() in loopTask,
  // operator new -> __cxa_throw -> terminate, with the driver asking for 48,000
  // bytes for a full-panel window. docs/power-management.md is the other freeze;
  // this one is docs/map-follow.md, "An unbounded window aborts the device".
  bool windowRefreshAffordable(int w, int h) const;
  // Bytes to leave free after a windowed refresh's own buffer.
  //
  // 4 kB, measured 2026-08-22. The first cut was 12 kB and it was wrong in a way
  // only the panel could show: the menu-close window is 480x553 = 33,733 bytes
  // against a largest block of 43 to 45 kB, so 12 kB of margin refused a window
  // that fits and made every menu close pay a full refresh. The job of this
  // number is to refuse the 48,000-byte full-panel window that aborted the
  // device, and 4 kB still does that (48,000 + 4,096 > 45,044) while leaving the
  // cheap close cheap.
  //
  // Nothing allocates between this check and the refresh -- the backdrop is
  // already freed and the map render itself is measured at zero allocations
  // (docs/device-preview.md) -- so the margin is insurance against
  // fragmentation, not a reservation for a known cost.
  static constexpr size_t kWindowHeapMargin = 4 * 1024;
  // Runs the radius search for the menu's rows. False when there is no fix to
  // search from, which is the one case the menu refuses outright -- the query
  // starts at the rider, not at the viewport.
  bool runNearbyQuery();
  // Fills the category screen's rows for `category` and records which one it
  // was, so the detail screen can be reopened without a second search.
  bool loadNearbyCategory(uint8_t category);
  // `Show on map` for one category: flips its bit in nearbyCategoryMask_ and
  // redraws. Not a setting and not persisted -- a temporary layer, gone at the
  // next boot (safety-concept.md: "Not a persisted setting, no layer manager").
  void toggleNearbyCategoryOnMap(uint8_t category);
  // `Set destination`: writes the existing `dest` pin, replacing whatever was
  // there. No new catalogue row and no v2 log record -- the pin type already
  // exists (PinCatalog.h) and a `sourceType` field would cost a format version
  // that older builds skip whole (safety-concept.md, "Set destination").
  void setNearbyDestination(uint8_t hitIndex);
  // "0.7 km NE" for a list row, and the label a category row carries.
  void nearbyRowValue(const MapPointQuery::Hit& hit, char* buf, size_t bufLen) const;
  // SETTINGS.mapPointsEnabled: whether this device deals in the point layer at
  // all. Gates the shard source, the render walk and the `Nearby` row.
  bool pointsEnabled() const;
  static StrId nearbyCategoryLabel(uint8_t category);
  // The condition line on the detail screen: the first reliability flag the
  // point carries, worded for that category ("Water quality unverified" rather
  // than "Not verified" under Water). Empty when the point carries none.
  static StrId nearbyConditionLabel(uint8_t category, uint8_t flags);

  // Which categories draw their marks on the map right now. Zero -- the default
  // -- draws none, so the map is unchanged until the rider asks for a layer.
  // Deliberately not in CrossPointSettings: it is a view, not a preference.
  uint16_t nearbyCategoryMask_ = 0;
  // Nearest metres per category from the last search, MapPointQuery::kNoDistance
  // where the radius held none.
  uint32_t nearbyDistances_[kSafetyCategoryCount] = {};
  MapPointQuery::Hit nearbyHits_[MapPointQuery::kMaxHits];
  uint8_t nearbyHitCount_ = 0;
  uint8_t nearbyCategory_ = 0;
  // Which row the category list reopens on, so toggling `Show on map` does not
  // walk the rider back down the list.
  uint8_t nearbyRow_ = 0;
  // The point `View on map` was pointed at, kept so the frame can mark it. Not
  // a pin and never persisted: it is a thing the rider is looking at right now,
  // and it is dropped the moment they go back to following themselves.
  int32_t nearbyViewedLatE7_ = 0;
  int32_t nearbyViewedLonE7_ = 0;
  uint8_t nearbyViewedCategory_ = 0;
  uint8_t nearbyViewedFlags_ = 0;
  bool nearbyViewedValid_ = false;
  // The query's own file handle and its scratch, allocated when the rider first
  // opens Nearby and kept for the rest of the screen's life: the map's own
  // sources are streaming during a render and one seek cursor cannot serve two
  // readers.
  std::unique_ptr<HalFileSource> nearbyFile_;
  std::unique_ptr<MapPointQuery> nearbyQuery_;
  // The render-side source for the marks. Allocated in onEnter() next to the
  // tile source, with its own file handle for the same reason.
  std::unique_ptr<HalFileSource> pointFile_;
  std::unique_ptr<MapPointSource> points_;

  // ## The destination readout in the header (../../docs/nearby-menu.md)
  //
  // While a destination is set, its sector and distance replace the place name
  // in the header slot that already exists. Quantised hard and floored at 30 s,
  // because a value that changes is a waveform pass: pin distance prints in 10 m
  // steps, which at 30 km/h would repaint about once a second
  // (docs/map-header-status.md, "The repaint policy").
  static constexpr uint32_t kDestRepaintMs = 30 * 1000;
  // Fills `buf` with "NE 4.2 km" for the current destination, quantised. False
  // when no destination is set or there is no fix to measure from -- and then
  // the place name keeps the slot.
  bool destHeaderText(char* buf, size_t bufLen) const;
  // The quantised value, so a repaint can be skipped when nothing a rider would
  // read has changed. Distance in the printed unit's own steps, never metres.
  uint32_t destQuantisedDistance() const;
  uint8_t destSector() const;
  bool hasDestination() const;
  uint32_t drawnDestDistance_ = 0;
  uint8_t drawnDestSector_ = 0xFF;
  bool drawnDestPresent_ = false;
  uint32_t nextDestRepaintMs_ = 0;

  // ## Pins (../../../docs/pins-plan.md, phase 3)
  //
  // All of it is OptionPopup inside this activity, not a Pins activity: this
  // screen owns the BLE peripheral for exactly its own lifetime, so leaving it
  // would drop the phone link, stop the position and cost a full redraw to come
  // back.
  //
  // The Pins list: existing pins only, with the distance from the rider in the
  // popup's value column. LEFT deletes, SELECT shows, RIGHT replaces
  // (OptionPopup::RowActions).
  // Opens whatever a row callback asked for (pendingPinPopup_). Called from
  // loop(), never from a callback.
  void servicePendingPinPopup();
  void openPinsMenu();
  // Add / Replace: all eleven catalogue slots, so an empty one can be filled. An
  // empty slot saves straight away; an occupied one is confirmed.
  void openPinsAddList();
  // One row per saved pin plus `All`: which pins may put a marker on the edge when
  // they are outside the viewport (CrossPointSettings::mapPinsOffscreenMask). SELECT
  // toggles and reopens the list, so several can be flipped in one visit.
  void openPinsOffscreenList();
  bool pinEdgeMarkerEnabled(const PinEntry& entry) const;
  size_t pinEdgeMarkerCount() const;
  // The Pins list carries two rows above the pins (Add / Replace, Off-screen
  // markers), so a row index is not a pin index. One place converts.
  static constexpr int kPinListFirstPinRow = 2;
  static size_t pinRowToListIndex(int row) { return static_cast<size_t>(row - kPinListFirstPinRow); }
  // A Replace is always confirmed, from either list. One entry point for both:
  // a catalogue row and a store slot are the same index for every key this build
  // knows, and the slots above the catalogue hold foreign keys, which this handles
  // by reading the key off the entry.
  void confirmPinReplaceSlot(size_t slot);
  void confirmPinDelete(size_t slot);
  // Both go through MapPins, which writes the history before it moves the active
  // set -- the console and these popups must produce the same record.
  void savePin(const char* key, const char* label);
  void deletePin(size_t slot);
  // SELECT on a pin: render the viewport around it and switch to Observe, so the
  // next fix does not yank the frame back. Reuses observation mode wholesale
  // rather than adding a fourth kind of frame, which also means the return path
  // already exists -- the menu's "Follow mode" row (docs/map-observation-mode.md).
  void showPinOnMap(size_t slot);
  // Every stored pin that lands inside the viewport, drawn straight onto
  // GfxRenderer in the same composition pass as the compass and the marker -- not
  // through MapRenderer, which knows nothing about pins. Off-screen pins are
  // skipped here; their edge indicators are phase 6.
  void drawPins();
  // One pin: a rounded head with the type glyph inside it, on a tail whose point
  // is the coordinate. Same 2 px stroke as the position marker, so the two read as
  // one family, and big enough to find on a panel full of building outlines --
  // both asked for on hardware 2026-08-17, where a bare 16 px glyph could not be
  // spotted and sat under the marker when the pin was at the rider's own position.
  void drawPinBalloon(int tipX, int tipY, size_t catalogIndex, uint8_t step = 0);
  // Which baked rotation of the shape points closest to a screen-space direction.
  static uint8_t pinShapeStepFor(double dx, double dy);
  // Sized on the panel, not on a laptop: a 16 px glyph was legible and still hard
  // to pick out of a field of building outlines, so this went to a real pin shape
  // at 39x47 with the glyph inside its head (2026-08-17). Against the position
  // marker's 44 px, which keeps the rider's own mark the biggest thing on screen.
  // The dimensions live with the asset (pins_shape.h), not here.

  // One edge marker for a pin outside the viewport: where the bearing ray leaves
  // the screen, which way it points (unit vector times 64 -- the raw pixel
  // difference can be millions), how far the pin is, and how many pins merged
  // into this one arrow.
  struct PinEdgeMark {
    int16_t x = 0;
    int16_t y = 0;
    int16_t dirX = 0;
    int16_t dirY = 0;
    uint32_t metres = 0;
    uint8_t catalogIndex = 0;  // which pin, so the marker can carry its glyph
    uint8_t count = 0;         // 0 means merged into another mark
  };
  void drawPinEdgeMark(const PinEdgeMark& mark);
  // Where an edge marker and its label may land: the panel minus everything this
  // screen already draws over the map -- the button bar, the side-hint boxes and the
  // compass. Empty (zero width or height) when there is nothing left, which is a
  // refusal to draw rather than a marker hidden under furniture.
  Rect pinEdgeArea() const;
  // Markers one frame will draw. A cap, because the array is a stack local and the
  // Resource Protocol keeps a frame's locals under 256 bytes -- eight is 128 of
  // them. Anything past it is logged, never dropped silently.
  static constexpr int kPinEdgeMax = 8;
  // Keeps an arrow and its distance fully on the panel instead of half off it.
  static constexpr int kPinEdgeMargin = 14;
  // Two markers closer than this merge into one with a count: two arrows on top
  // of each other read as one broken arrow.
  static constexpr int kPinEdgeMergePx = 52;  // a whole pin wide, since a marker is one now
  // Which store slot the nth row of the open Pins list stands for. Recomputed
  // rather than captured: the popup is modal, so the store cannot change under
  // it, and a captured table would be one more thing to keep in step.
  size_t pinSlotForRow(size_t row) const;
  void pinDistanceText(const PinEntry& entry, char* buf, size_t bufLen) const;
  // Why a pin cannot be saved right now, or nullptr when it can. Only ever "no
  // fix at all" today -- a pin must never be written at 0,0.
  const char* pinSaveRefusal() const;
  // Fills `buf` with how old the fix is ("fix 4 min old", "fix is from the last
  // session") when that matters, and returns true. False means the fix is fresh
  // enough that saying anything would be noise.
  bool pinFixAgeWarning(char* buf, size_t bufLen) const;
  // Where the rider actually is, which is not what the frame is showing while
  // Observe mode is panned away from them (observeReturnLatE7_).
  int32_t riderLatE7() const;
  int32_t riderLonE7() const;
  // The heading that belongs with those two. Same split for the same reason:
  // lastHeading_ is the heading the *frame* was drawn with, which after a pan or
  // a `View on map` is the frame's, not the rider's.
  uint8_t riderHeading() const;
  // Short confirmation above the button hints -- "Camp saved", or the reason
  // nothing was saved. Saves the pixels underneath and refreshes only its own
  // rectangle, so it costs one small window refresh and leaves the map up; the
  // patch is what lets it disappear again without re-reading a tile.
  void showPinNotice(const char* text);
  void clearPinNotice();
  void pinNoticeRect(int& x, int& y, int& w, int& h) const;
  // A popup that opened over the menu can be bigger than the menu was. The
  // backdrop covers the menu's rect only, so a larger popup has to give it up --
  // restoring a rect smaller than what is on the panel leaves popup pixels
  // behind.
  void dropBackdropIfPopupOutgrew();
  // How long a fix may be before a save says so. Not measured, and not the
  // answer to "how old is too old" -- that stays open (docs/pins-plan.md, Open
  // items). Two minutes is simply longer than the phone's own send interval at
  // any speed (../../../docs/send-interval-analysis.md), so a fresh link never
  // trips it.
  static constexpr uint32_t kPinStaleFixMs = 2 * 60 * 1000;
  static constexpr uint32_t kPinNoticeMs = 2500;

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
  // One pan step (kPanStepPercent of the screen) in `direction`, reusing the projection the frame on
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
  // Entered by the wake-into-map routing rather than by a person choosing Map.
  // Only difference it makes: the "reading tiles" splash is skipped, see onEnter().
  // Passed in rather than inferred from APP_STATE.mapActivityLoadCount, which is
  // nonzero for a slightly different reason and stops being nonzero at a different
  // moment.
  bool resumedFromSleep_ = false;
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
  // Latched by a hold on Up/Down in Observe that has already fired its zoom
  // step, so the release ending that hold does not also pan (handleButtons()).
  // One step per hold: this stays set until the button comes back up.
  bool observeHoldZoomed_ = false;

  // Which source this map session takes its position from, decided once in
  // onEnter() and constant for the life of the screen. True means the phone
  // over BLE; false means the receiver on this board.
  //
  // **False stops the BLE radio coming up at all**, not merely the position
  // path: BlePositionServer::begin() is one service with four characteristics
  // (position, command, transfer, transfer status), so there is no way to have
  // tiles without also advertising and running the controller. The rider who
  // turned the receiver on asked for a map that needs no phone, so the map does
  // not run a radio for one -- maintainer's call, 2026-09-03. What that costs
  // while the map is open: no autosync of missing tiles, no freshness check, no
  // BLE command channel. The clock comes from the receiver's own UTC instead
  // (drawHeaderStatusStrip()), and the tile sync screen still uses BLE
  // normally.
  //
  // Read it rather than SETTINGS.mapGnssPosition at each call site: a setting
  // toggled from the host mid-screen would otherwise half-apply, with icons
  // saying one thing and a running radio another.
  //
  // **Not starting the server changed callers that never mentioned it.**
  // `BlePositionServer::isRunning()` was being used as a stand-in for "the map
  // is live", and preventAutoSleep() was one of them -- so the first GNSS walk
  // got a map that let the device deep-sleep, which cold-started the receiver on
  // every wake (2026-09-04, fixed in preventAutoSleep()). `PowerLog::bleState()`
  // was another: its 0 no longer means "not the map screen". Before adding a
  // caller here, grep isRunning() and ask which question it is really asking.
  bool bleInUse_ = true;

  // True while the header clock carries its " UTC" suffix: a GNSS session whose
  // clockUtcOffsetQ was never set, so the time really is UTC. Decided in
  // drawHeaderStatusStrip() each time the row is laid out, and read by the same
  // function a few lines later -- a member rather than a local because the
  // layout chain settles it before the string that uses it is built.
  bool clockShowsUtc_ = false;

  // Set from BlePositionServer::begin()'s return in onEnter(). Without this,
  // a BLE stack that failed to come up (plausible: init costs ~75 KB heap,
  // see docs/map-memory.md) looks identical to a phone that simply has not
  // connected yet -- both render the same "waiting" banner forever.
  // renderWaiting() reads this to draw a different line.
  bool bleStartFailed_ = false;
  bool hasReceivedAny_ = false;
  uint8_t lastDrawnSeq_ = 0;
  // The fix a ladder step re-renders around. Only meaningful once
  // hasReceivedAny_ is true.
  int32_t lastLatE7_ = 0;
  int32_t lastLonE7_ = 0;
  uint8_t lastHeading_ = 0;
  // The most recent fix's altitude, from whichever channel carried one --
  // BLE's PositionUpdate or the serial/BLE command console's `pos ... alt`.
  // Not reset when a fix without altitude arrives: a phone that stops sending
  // altitude mid-ride should not blank a number the rider was just reading,
  // and there is no "altitude just became unknown" event worth reacting to
  // the way there is for the destination readout appearing or going away.
  int16_t lastAltitudeM_ = 0;
  bool hasAltitudeReading_ = false;
  // True from onEnter() bootstrapping the last-saved fix off the card until
  // the first real fix of this session lands -- distinguishes "showing
  // where the rider was last seen" from "showing where they actually are
  // right now", so renderViewport() knows to keep the waiting banner up.
  bool showingPersistedFix_ = false;

#ifdef ENABLE_GNSS_CMD
  // True only when *this* activity powered the receiver's rail, so onExit()
  // cannot switch off a session somebody else started -- CMD:GNSS ON from the
  // host is the case that matters, since a bring-up run must survive a trip
  // through the map screen.
  bool gnssStartedHere_ = false;
  // millis() at which the driver last changed its fix, derived as
  // millis() - Gnss::fixAgeMs(). The driver has no "changed since you last
  // asked" flag for a second reader -- Gnss::poll() returns that, and main.cpp
  // is the one calling poll() -- so the change instant is what identifies a
  // sample here. haveGnssFixMs_ separates "never seen one" from "seen one at
  // millis() == 0".
  uint32_t lastGnssFixMs_ = 0;
  bool haveGnssFixMs_ = false;
  // applyFix() wants the phone's rolling packet counter and the receiver has no
  // such thing, so this stands in for it. It is only ever used for logging and
  // for the BLE path's already-drawn test, and the two counters can collide on
  // one value in 256 -- with both sources live that costs at most one skipped
  // BLE packet, which is the same 5 s the phone's next packet arrives in.
  uint8_t gnssSeq_ = 0;
  // What the panel was last told, for the same reason transferIconShown_ exists
  // next to it: the repaint decision compares against what is on the glass, not
  // against what is true. Starts at Off so the first draw of a running receiver
  // counts as a change.
  GnssHeaderState drawnGnssState_ = GnssHeaderState::Off;
  // The heading decision's state between fixes: the speed gate's hysteresis and
  // the step on the panel. The arithmetic lives in MapGnssHeading.
  MapGnssHeading::State gnssHeadingState_;
#endif

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
  // Set in onEnter(), consumed by whichever render*() draws the first real
  // frame for this activation (renderViewport(), renderRouteOverview() or
  // renderWaiting()). Forces that one frame through HALF_REFRESH instead of
  // FAST_REFRESH -- the fast LUT leaves ghosting (main.cpp's CMD:SHOWIMAGE
  // comment), and it never gets a real clear otherwise: the map screen has no
  // other non-differential refresh anywhere. Coming back from the menu after
  // the screen has ghosted over a long session is exactly the point this
  // screen reappears, so that is the one frame worth a clean waveform.
  //
  // HALF, not FULL: HALF is the single-pass absolute clean (0xD7), the only
  // clean primitive stock X4 firmware uses in normal operation. FULL selects
  // the multi-flash OTP waveform (0xF7) -- it clears no better and the map
  // screen visibly blinked through several inversions on every entry
  // (Ssd1677Driver::displayImpl, SleepActivity's #2471 note).
  bool pendingEntryCleanRefresh_ = false;
  // True whenever the frame on the panel carries a header row (the BLE link
  // icon, its bars, the transfer icon) -- drawHeaderStatus() drew one.
  // Separate from viewportDrawn_ on purpose: whether the phone is connected has
  // nothing to do with whether the frame is followable. The persisted-fix frame
  // has a header row and is not followable; the waiting banner has neither.
  // updateHeaderStatus() gates its own windowed repaint on this flag, not on
  // viewportDrawn_ -- see docs/map-header-status.md, "The row freezes while the
  // map shows a persisted fix".
  bool headerRowDrawn_ = false;
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
  // The last reset's own tile range and missing mask, kept for
  // recheckHatchedTiles() to re-scan between resets. Written only by
  // renderViewport() (never by renderRouteOverview() -- an overview has no
  // autosync), so it is stale, on purpose, while the overview or observation
  // screen is up; recheckHatchedTiles() gates on screenMode_/overviewShown_
  // rather than trusting valid alone for that reason. A field-for-field copy
  // of what renderViewport() already builds for consoleState_.setTileRange()
  // (MapCommandConsole.h, MapTileRangeSnapshot), not a second convention.
  MapTileRangeSnapshot lastTileRange_;
  // Tiles asked for and not yet settled by an arrival or a `skip`. Non-zero is
  // exactly the condition the transfer icon shows.
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
  // Last zoom step a DIAG_M line was actually sent for, and whether the phone
  // was subscribed at that time -- see sendViewportDiagonalIfChanged(). 0xFF
  // is "never sent", so the first render on a fresh connection always sends
  // regardless of which step it lands on.
  uint8_t lastSentDiagZoomStep_ = 0xFF;
  bool lastDiagSubscribed_ = false;
  // Last tileSeq already cleared out of the store (drainTransferredTiles()).
  uint32_t lastClearedTileSeq_ = 0;
  // Deadline for the redraw a tile arrival owes, pushed out by each further
  // arrival so a burst costs one frame. 0 means none owed. loop() also
  // re-arms this, with no bound, for as long as transfer_.status().active is
  // true when it expires -- a render mid-transfer has been reproduced to
  // kill the fetch, so there is deliberately no escape hatch there. See
  // MapActivity.cpp's kArrivalRedrawSettleMs comment and docs/missing-tiles.md.
  //
  // expireAutoSync() also clears this directly once it independently proves
  // the transfer has gone silent (kAutoSyncQuietMs of unmoved byte counters)
  // -- but only while an autosync ask is outstanding. An unsolicited push
  // with no ask never reaches expireAutoSync() and can still leave this
  // armed forever if it stalls the same way.
  uint32_t arrivalRedrawDueMs_ = 0;
  // Bytes the receiver had moved when the ask's quiet timer was last rearmed.
  // The ask expires on silence, not on elapsed time (expireAutoSync()).
  uint32_t lastTransferProgress_ = 0;

  // ## Tile freshness state (SETTINGS.mapTileFreshnessMode, docs/tile-freshness.md)
  //
  // In memory and never persisted -- see StaleTilesList for why this is not
  // MissingTilesStore.
  StaleTilesList staleTiles_;
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
  // a poll. All of them are written by drawHeaderStatusStrip(), on every path
  // that draws the row, including full frames.
  bool transferIconShown_ = false;
  bool drawnLinkConnected_ = false;
  // -1 means "never drawn", which is not the same as 0 bars.
  int drawnBleBars_ = -1;
  // Minute-of-day last painted into the clock slot, 0..1439. -1 means no time
  // is known (no packet has carried a non-zero utc yet) and the slot is blank
  // -- so the transition into or out of "no clock" moves this value and
  // repaints, same as a minute rolling over does.
  int16_t drawnClockMinute_ = -1;
  // Until when Observe's clock shows the exact minute. Set by any button press:
  // a rider who pressed something is looking at the screen, and the saving only
  // exists during the hours nobody is. 0 = never set, i.e. coarse.
  uint32_t clockFineUntilMs_ = 0;
  // Last bar count a real rssi() reading produced this connection. rssi()
  // answers 0 on failure, not a real signal strength, and resolveBleBars()
  // holds this instead of remapping 0 -- see its definition. 0 also means
  // "no reading has succeeded yet", which draws the same as 0 bars. Reset to
  // 0 on disconnect (drawHeaderStatusStrip()) and in onEnter(), so a new
  // connection never inherits the last one's bars.
  int lastKnownBleBars_ = 0;
  // millis() deadlines; 0 means due now. The poll interval bounds how often
  // rssi() is asked; the bars interval bounds how often a bar count that keeps
  // crossing a threshold may spend a waveform pass.
  uint32_t nextHeaderPollMs_ = 0;
  uint32_t nextBarsRepaintMs_ = 0;

  // ## Hike mode's second header line (elevation + lat/lon)
  //
  // The text currently on the panel, compared byte-for-byte against the next
  // candidate string rather than against the numbers that fed it -- one
  // comparison instead of separately quantising altitude and two coordinates,
  // and it can never disagree with what drawHikeElevationLine() actually drew.
  // Empty means nothing has been drawn there yet (mode entry, or before the
  // first fix).
  char drawnHikeLineText_[40] = "";
  // millis() deadlines, same shape as nextHeaderPollMs_/nextBarsRepaintMs_
  // above: the poll interval bounds how often the candidate text is even
  // formatted, and the repaint interval is the floor between two panel
  // refreshes caused only by the fix drifting (lat/lon change on every fix at
  // hiking pace otherwise repaints every poll).
  uint32_t nextHikeLinePollMs_ = 0;
  uint32_t nextHikeLineRepaintMs_ = 0;

  // One state, two channels. A `zoom 3` over USB and a `zoom 3` over BLE
  // land on the same number because they share this object, not because two
  // copies are kept in step (MapCommandConsole.h).
  MapConsoleState consoleState_;
  MapSerialConsole serial_{consoleState_};
  MapBleConsole ble_{consoleState_};

  // The rider's pins: the active set replayed off the card in onEnter(), and the
  // append-only history behind it (MapPins.h, ../../../docs/pins-plan.md).
  //
  // Lives on this screen rather than in a global for the reason a Pins *activity*
  // was rejected: this screen owns the BLE peripheral for exactly its own
  // lifetime, and the pins are only ever placed at the last fix it received.
  MapPins pins_;

  // CONFIRM's menu (Refresh / Mode / Observation mode).
  OptionPopup optionPopup_;
  // Set while an open popup has drawn the two side-hint boxes (a Pins list, which
  // takes the front pair for its row actions). They land outside the dialog, so
  // the close has to refresh a taller window than the backdrop rect
  // (restoreMenuBackdrop()).
  bool popupDrewSideHints_ = false;
  // The pin notice on the panel: the pixels it covered, and when it expires.
  // Cleared by the timer in loop() or by any button, whichever comes first.
  std::unique_ptr<uint8_t[]> pinNoticePatch_;
  size_t pinNoticePatchSize_ = 0;
  Rect pinNoticePatchRect_{0, 0, 0, 0};
  uint32_t pinNoticeUntilMs_ = 0;
  // millis() of the last *real* fix. Zero until one lands: the persisted fix
  // restored in onEnter() is not one, which is why a save on it warns
  // (pinFixAgeWarning()).
  uint32_t lastFixMs_ = 0;
  // The map under the open menu (captureMenuBackdrop()). Null whenever the menu
  // is closed or the capture failed.
  std::unique_ptr<uint8_t[]> menuBackdrop_;
  size_t menuBackdropSize_ = 0;
  Rect menuBackdropRect_{0, 0, 0, 0};
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
  // Set when openMapMenu()'s Mode row cycles mode_ -- that row keeps the menu
  // open (OptionPopup::setKeepOpenRows()), so the map itself is not
  // redrawn there. Checked and cleared where the menu actually closes
  // (loop()): true means menuBackdrop_ is the old mode's frame and a real
  // redraw is owed instead of a cheap restore.
  bool mapMenuModeChanged_ = false;

  // Map files pushed over the same BLE connection the position packets use.
  // Attached while this screen is up and only while it is up: the receiver
  // writes to the card from a BLE callback, and the card is the map's own
  // (MapTransferReceiver.h).
  MapTransferReceiver transfer_;
};
