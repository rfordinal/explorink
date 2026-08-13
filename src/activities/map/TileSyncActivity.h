#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>

#include "MapBleConsole.h"
#include "MapCommandConsole.h"
#include "MapTilePath.h"
#include "MapTransferReceiver.h"
#include "activities/Activity.h"

// Asks the phone for the map tiles the device is missing, and shows them as the
// squares they are on the map. Entered from the home menu.
//
// ## Why this is not part of the map screen
//
// It was, briefly. The map screen owns the BLE peripheral (`onEnter()`:
// `BlePositionServer::begin()`, `onExit()`: `end()`), so putting the fetch behind
// its CONFIRM menu was the only way to reach a live link -- and that made the
// progress panel a second render mode inside an activity that already has
// enough of them.
//
// It is also the wrong place for a rider. Filling coverage gaps is preparation:
// it happens at home, over a phone that has the tiles on it, before a ride.
// Nobody stops mid-trail to sync map data. So this screen starts its own BLE and
// owns the whole job, and the map screen went back to being a map.
//
// The list itself is still built while riding --
// `MapActivity::renderViewport()` records every tile it had to hatch
// (`MissingTilesStore`). Record on the trail, fetch at home; the two never need
// to be on screen together.
//
// ## The grid, not a list
//
// This screen used to be one row per tile: `z12 1105/711`, a status word and a
// little bar. That names nothing a rider knows. The coordinates are internal,
// the order is the fetch order, and reading the list tells nobody where on the
// map the holes are.
//
// So the tiles are drawn where they actually are. Squares in their real relative
// positions, north up, nested the way the tile pyramid nests: a z11 parent frame
// holds four z12 quadrant frames, each of those holds four z13 leaves. A square
// on screen means that tile is still missing. A tile that arrives is **rubbed
// out**, and its frames go with it once they are empty. The screen empties as
// the sync runs.
//
// It stays up when the run ends. On a run the supplier could not fill, the
// squares still standing are exactly which ground is still missing -- the one
// thing the verdict line cannot say.
//
// Squares are drawn as outlines, never filled. Riders at rungs 3-6 read z11, so
// a run collects whole missing z11 parents, and filling those paints most of the
// panel black (seen against real device data, 2026-08-13). Filling an area says
// nothing its outline does not.
//
// That is the whole design. There is no "downloading" state, no "done" state and
// no legend, because there is nothing to tell apart: present means missing. It
// is a toy to watch while a fetch that takes minutes runs, and the numbers that
// actually answer questions -- how many, how fast, how long left -- are in the
// summary line above it, which is where they always were.
//
// A tile the supplier answered `skip` for stays drawn. Nothing arrived, the
// device still does not have it, and rubbing it out would say otherwise.
//
// ## The viewport sits on the densest area, and that is all it does
//
// Missing tiles accumulate across rides (`MissingTilesStore`), so the set can be
// a dense cluster plus one square 40 km away. Drawn to true scale that would
// collapse the cluster to a dot.
//
// So the grid does not draw all of them. It is a window of at most
// `kMaxWindowCols` x `kMaxWindowRows` z11 parents, placed where the most missing
// tiles are, and anything outside it is simply not drawn. Inside the window the
// geometry is true -- real relative positions, real nesting, no compression.
//
// Nothing is lost by that, because **the progress bar is the indicator**. The
// grid answers "what is left, roughly where" and nothing else; the bar and the
// summary line answer how many, how fast and how long. A window that showed
// every last outlier would be a worse toy and no better instrument.
//
// The window is chosen once, in `armRun()`, and never moves -- a viewport that
// re-centred as tiles landed would make squares jump under the rider's eyes.
//
// **Tile state is derived, not accounted for.** Keeping a second ledger in step
// with the store is how the two drift apart, so instead each tile asks:
//
// - the receiver's `activeTile` says this tile is on the wire, and `received`
//   over `total` is what the summary line reports (`MapTransferReceiver::Status`)
// - `skipped_` says the phone gave up on it (`IMapSkipObserver`)
// - otherwise, gone from `MissingTilesStore` means it arrived -- `forget()` is
//   what removes it, and only an arrival calls that
// - still in the store means still waiting
//
// The grid only asks one question of that: did it land. Everything else draws
// the same square.
//
// The thing that must be remembered is the **set**, because the store shrinks as
// tiles land: `rows_` is a snapshot taken when the sync starts, so the grid
// keeps its shape and squares vanish out of a layout that does not reflow.
//
// ## What it owns while it is up
//
// - the BLE peripheral: `begin()` in `onEnter()`, `end()` in `onExit()`
// - the transfer receiver, attached after `begin()` and detached before `end()`
//   -- a hook still registered when this activity is deleted is a callback that
//   outlives its owner (`MapTransferReceiver.h`)
// - its own `MapConsoleState` plus a BLE console over it, because the phone
//   answers in the same ASCII the map console takes: `missing` to read the list,
//   `skip` to give up on a tile
//
// It deliberately does not share MapActivity's console state. Two screens are
// never up at once, and a shared state would put this screen's skip tally and
// the map's zoom in one object for no reason.
//
// ## The conversation
//
// `onEnter()` sorts the store into fetch priority, snapshots it, and sends
// `NEED_TILES <count> fmt <version>` as an unsolicited indication. The phone
// pages the list with `missing`, pushes tiles over the transfer channel, and
// sends `skip` for what it cannot supply. Done when arrivals plus skips reach
// the count. BACK sends `FETCH_CANCEL` and leaves.
class TileSyncActivity final : public Activity, public IMapSkipObserver, public IMapStaleObserver {
 public:
  TileSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Same reason as the map screen: the BLE peripheral is running and a transfer
  // can take minutes, so the device must not doze off in the middle of one.
  bool preventAutoSleep() override;

  // IMapSkipObserver -- the phone saying it cannot supply one tile.
  void onTileSkipped(uint8_t z, uint32_t col, uint32_t row) override;

  // IMapStaleObserver -- the phone's verdict on tiles the device already holds.
  // This screen does not fetch them itself: the phone found them by reading the
  // index, so it knows which tile and which content id, and pushes each one
  // unasked on the transfer channel (docs/tile-freshness.md).
  void onTileStale(uint8_t z, uint32_t col, uint32_t row) override;
  void onCheckFinished(bool known, uint16_t staleCount) override;

 private:
  // Waiting means the device is advertising and nothing has subscribed to the
  // command channel yet -- no phone, or a phone with the app closed. It is a
  // state worth showing, not an error: the rider opens this screen first and
  // the phone catches up a moment later, and a screen that said nothing would
  // look identical to one that had hung.
  //
  // Running until arrivals plus skips reach the total; Finished keeps the
  // verdict up until the rider presses Back, so a sync that ends on its own
  // does not silently vanish.
  enum class Phase : uint8_t { Waiting, Running, Finished };

  // What one row is doing. Derived per repaint -- see the header comment.
  //
  // `Missing`, not `Skipped`: the wire verb is `skip` and stays that way, but
  // from the rider's side nothing was skipped -- the tile simply is not
  // available from the supplier. "Skipped" reads as a choice somebody made.
  enum class RowState : uint8_t { Waiting, Active, Done, Missing };

  void renderScreen();
  // Bytes as a rider reads them: "6.1 kB", "440 kB", "1.2 MB". A raw byte count
  // is arithmetic homework on a screen glanced at in gloves.
  static void formatBytes(uint32_t bytes, char* out, size_t outSize);
  // "2m 20s" / "45s". Empty while there is nothing to base it on.
  static void formatDuration(uint32_t seconds, char* out, size_t outSize);
  // Writes the summary line: done/total, percent, transferred, rate, ETA.
  void formatSummary(char* out, size_t outSize) const;
  // Repaints what changed: the whole grid when a tile settles, or just the
  // summary line while the bytes of the tile in flight climb.
  void updateProgress();
  RowState stateOf(int index, uint32_t& received, uint32_t& total) const;
  void gridRect(int& x, int& y, int& w, int& h) const;
  void summaryRect(int& x, int& y, int& w, int& h) const;
  // The whole grid, cleared and redrawn. Every square that is still missing,
  // nothing for the ones that landed. `top` is the floor to start at, or 0 for
  // the running screen's own -- the finished screen writes more above it.
  void drawGrid(int top);
  // One z11 parent and everything of it that has not arrived. Returns false
  // when the parent is empty, so the caller can leave its frame off too.
  bool drawParent(int px, int py, int size, uint16_t pc, uint16_t pr);

  // Clears MissingTilesStore entries for tiles that have landed.
  //
  // On this task, never in the BLE callback: this and
  // `MapActivity::renderViewport()`'s `record()` are the store's only writers and
  // both run on an activity task. A second writer on the NimBLE host task would
  // corrupt the vector, so the receiver publishes a coordinate and this acts on
  // the change (`MapTransferReceiver::Status::lastTile`).
  void drainTransferredTiles();
  // True when this tile is still on the store's list.
  bool stillMissing(const MapTileCoord& tile) const;
  // Sends `FETCH_CANCEL` if the sync was still running, then goes home.
  void leave();

  Phase phase_ = Phase::Waiting;
  // Sends NEED_TILES and moves to Running. Called when the phone subscribes,
  // and again if it leaves and comes back.
  void askForTiles();
  // Watches the command channel's subscription and moves between Waiting and
  // Running. A phone that walks out of range mid-sync takes the transfer with
  // it, so the screen goes back to waiting rather than pretending.
  void trackPhone();
  // True while a central is subscribed to the command channel.
  bool phoneListening() const;
  // Sends CHECK_TILES when the rider has asked for the check to run here
  // (SETTINGS.mapTileFreshnessMode). Preparation at home is exactly where
  // spending the phone's data belongs, which is why this mode exists separately
  // from the map screen's live one.
  void askAboutFreshness();
  // Writes the one line that says what the freshness check is doing, or
  // returns false when there is nothing to say (Freshness::Idle). Separate from
  // the fetch's own status line because the two answer different questions --
  // "did the tiles I lack arrive" and "are the tiles I have still right" -- and
  // a rider who cannot tell them apart reads a check as a download.
  bool formatFreshness(char* out, size_t size) const;

  // Snapshots the missing list and zeroes everything one run reports, so a second
  // run on the same visit starts where a fresh entry would. False means the
  // snapshot could not be allocated.
  bool armRun();
  // The z11 parent a tile sits in. z11/z12/z13 are the only LODs the map reads
  // (docs/zoom-rungs.md), so this is a shift down, never up.
  static void parentOf(const MapTileCoord& tile, uint16_t& pc, uint16_t& pr);
  // Places the viewport over the densest patch of the snapshot. Called once per
  // run, from armRun() -- see the header comment.
  void chooseWindow();

  // Stale tiles this visit, for the ping-pong guard and the log. Not persisted
  // and not this screen's row list -- see StaleTilesList.
  StaleTilesList staleTiles_;
  // True once CHECK_TILES has gone out this visit. One check per visit: this
  // screen draws no map, so nothing adds to the held-tile store while it is up.
  bool freshnessAsked_ = false;

  // What the screen says about the check, so a rider can tell a freshness pass
  // from a plain fetch. It could not before: the check ran, tiles moved over
  // BLE, and the only evidence was the serial log -- so the data spend read as
  // a download of missing tiles and nothing named it (docs/tile-freshness.md).
  enum class Freshness : uint8_t {
    Idle,     // not asked -- mode Off, nothing held, or the fetch is still running
    Asking,   // CHECK_TILES is out, waiting for `checked`
    Current,  // the phone compared them all and none had moved
    Stale,    // freshnessStale_ tiles were out of date; the phone is pushing them
    Unknown,  // `checked unknown` -- the phone could not read the index
  };
  Freshness freshnessState_ = Freshness::Idle;
  // Cumulative over the visit: how many tiles have been checked and how many of
  // them were out of date. A visit asks in rounds (HeldTilesStore::
  // kMaxPerListing), and the rider wants one number, not one per round.
  uint32_t freshnessAskedCount_ = 0;
  uint32_t freshnessStale_ = 0;
  // What the round currently on the wire covers, folded into the total when
  // `checked` closes it.
  uint32_t freshnessRound_ = 0;

  // When something last landed or was skipped, for the stall verdict below.
  // Armed by askForTiles(), so a screen that never asked cannot time out.
  uint32_t lastSettleMs_ = 0;
  // Whether the panel currently shows a phone or not, so trackPhone() only
  // repaints on a real change.
  bool drawnPhoneListening_ = false;
  // True once a phone has subscribed at least once this session. Distinguishes
  // "no phone has ever shown up" from "the phone was here and left", which want
  // different words: the first is a setup problem, the second is a range one.
  bool hadPhone_ = false;
  // Which end state Finished is showing. Only read once phase_ is Finished.
  StrId verdict_ = StrId::STR_MAP_FETCH_DONE;

  // The fetch-priority order, snapshotted when the sync starts so a row cannot
  // move as the store shrinks under it. Heap, not a member array: 200 entries
  // (MissingTilesStore::kMaxEntries) is ~2.4 KB, which is too much for an
  // activity that would carry it whether or not a sync ever runs -- and unlike
  // the map screen this one allocates no MapTileSource, so it is still the
  // cheaper of the two. Freed in onExit().
  struct Row {
    MapTileCoord tile;
    // The supplier answered `skip` for this one: it does not have it.
    bool unavailable = false;
  };
  std::unique_ptr<Row[]> rows_;
  uint32_t rowCount_ = 0;

  // The drawn viewport, in z11 parent coordinates: top-left corner and extent.
  // Set by chooseWindow(), read only by the grid. Four scalars rather than a
  // second snapshot -- the tiles themselves are already in rows_.
  uint16_t windowCol_ = 0;
  uint16_t windowRow_ = 0;
  uint8_t windowCols_ = 1;
  uint8_t windowRows_ = 1;
  // How many of rowCount_ fall outside the window. Logged, not drawn: the count
  // the rider watches is the summary line's, which counts all of them.
  uint32_t offWindow_ = 0;

  // The viewport cap, in z11 parents. Sized against the drawable area: at
  // 440 x 552 px (Lyra metrics, 480x800 panel) a 6 x 8 window gives a 69 px
  // parent and a 17 px z13 leaf, which is the smallest square still worth
  // drawing a frame around. The ratio also matches the area, so neither axis
  // wastes room.
  static constexpr uint8_t kMaxWindowCols = 6;
  static constexpr uint8_t kMaxWindowRows = 8;
  // A z11 parent is 4 z13 tiles across -- the leaf grid inside one cell.
  static constexpr int kLeavesPerParent = 4;
  // How big a cell is allowed to get. Without it a run whose tiles sit in one
  // parent scales that parent to the full 440 px and its z13 leaves to 110 px
  // each, which paints most of the panel solid black -- a lot of ink and a blot
  // rather than a picture. 128 px caps a leaf at 32 px, close to the parents in
  // the design sketch. The grid is centred in whatever is left over.
  static constexpr int kMaxCellPx = 128;
  // Gap between a square and its neighbour, so blocks read as separate squares
  // rather than one blot. Drawn as an inset on the fill.
  static constexpr int kTileInset = 2;

  // A stale tile is a dot, not an outlined square, and the two are drawn on the
  // same grid on purpose: they answer different questions about the same ground
  // -- "this square is not on the card" against "this square is on the card and
  // out of date" -- and a rider needs to tell them apart at a glance. An
  // outline for one and a solid dot for the other does that with no legend.
  //
  // The dot scales with the tile's LOD so depth still reads (a z11 dot is
  // bigger than a z13 one), but it is capped well under the cell: a disc that
  // fills its square stops looking like a mark on a map and starts looking like
  // a filled tile, which is the thing the outline decision above already
  // rejected for being all ink and no information.
  static constexpr int kDotDivisor = 4;
  static constexpr int kMaxDotPx = 20;
  static constexpr int kMinDotPx = 3;

  // Every tile this screen still owes the rider an answer about, in three
  // groups, drawn as one grid:
  //
  //   [0, rowCount_)                 missing, being fetched      outlined square
  //   then every unsettled held tile queued for a freshness check    dot
  //   then every stale tile awaiting its replacement                 dot
  //
  // A dot means "not settled yet", whichever half of the work it is waiting on.
  // It goes out when the phone answers that the tile is current, or -- for one
  // that came back stale -- when the replacement has actually landed. So the
  // grid empties as the check works through it, which is the thing worth
  // watching on this screen.
  //
  // The three cannot overlap: a missing tile has no content_id so it is never
  // in the held store (HeldTilesStore::record ignores content_id 0), and a
  // stale tile has already been settled in the store by the `checked` that
  // reported it, so it is no longer pending.
  //
  // interestAt() walks the held store to find the nth pending entry, so it is
  // O(store) per call. chooseWindow()'s O(n^2) placement pass is the only
  // caller that could feel that, and only on the rare path where the tiles are
  // spread wider than the window; the common clustered case exits early.
  size_t interestCount() const;
  MapTileCoord interestAt(size_t index) const;

  // Tiles the phone has given up on. Counted here as well as in the console's
  // tally because a `skip` for a tile that is not on this snapshot (a stale
  // phone-side list) still has to count toward finishing.
  uint32_t skipped_ = 0;

  // What the panel currently shows, so updateProgress() can tell a real change
  // from a poll.
  uint32_t drawnDone_ = 0;
  uint32_t drawnSkipped_ = 0;
  // millis() when the ask went out. The clock the rate and the ETA are built
  // on -- both are meaningless before the phone actually starts sending.
  uint32_t startedMs_ = 0;
  // millis() of the last summary-line repaint. The bytes of a transfer in flight
  // change constantly and each repaint is a real waveform pass, so the live
  // numbers are rate-capped rather than drawn per chunk.
  //
  // It is the summary line and not the grid because the grid has nothing to say
  // between arrivals: a square is there or it is gone. Without this the screen
  // would hold completely still for the whole of a slow tile.
  uint32_t lastActiveDrawMs_ = 0;
  static constexpr uint32_t kSummaryRefreshMs = 2000;

  // How long silence is allowed to look like work. The transfer channel reclaims
  // a stalled transfer after 30 s (docs/ble-map-transfer-protocol.md), so a
  // phone that is genuinely still pushing cannot be quiet for longer than that
  // without the link itself being dead.
  static constexpr uint32_t kStallVerdictMs = 30000;

  // Bytes of the in-flight file last observed, and when that count last moved.
  // Covers the case the silence check above cannot: a phone that ANRs mid-file
  // with the GATT link still held keeps `transfer.active` true forever --
  // reclaim only happens on the *next* begin (MapTransferReceiver.cpp:174-183)
  // -- so the `!transfer.active` gate never fires and the bar freezes with no
  // verdict. Same bytes-stopped-moving pattern as
  // MapActivity::expireAutoSync() (MapActivity.cpp:870-894), against the same
  // kStallVerdictMs budget so both paths agree on how long silence gets to
  // look like work.
  uint32_t lastReceivedBytes_ = 0;
  uint32_t lastProgressMs_ = 0;
  // Edge-detects a transfer starting. `received_` resets to 0 on every begin
  // (MapTransferReceiver.cpp:216), which can equal the previous file's
  // last-seen count, so comparing `received` alone cannot tell "still
  // stalled" from "a fresh file just started". This makes the active
  // false->true transition always count as progress.
  bool transferWasActive_ = false;

  // Last tileSeq already cleared out of the store. See drainTransferredTiles().
  uint32_t lastClearedTileSeq_ = 0;

  MapConsoleState consoleState_;
  MapBleConsole ble_{consoleState_};
  MapTransferReceiver transfer_;
};
