#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>

#include "MapBleConsole.h"
#include "MapTilePath.h"
#include "MapTransferReceiver.h"
#include "activities/Activity.h"

// Asks the phone for the map tiles the device is missing, and shows one row per
// tile with its own progress bar. Entered from the home menu.
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
// ## One bar per tile, not one bar for the batch
//
// A batch bar hides everything worth seeing: which tile is moving, which one is
// stuck, which ones the phone refused. A row per tile shows all of it, and it is
// the shape parallel transfers would need anyway -- more than one row simply
// shows movement at once, with no redesign.
//
// **Row state is derived, not accounted for.** Keeping a second ledger in step
// with the store is how the two drift apart, so instead each row asks:
//
// - the receiver's `activeTile` says this row is on the wire, and `received`
//   over `total` is its bar (`MapTransferReceiver::Status`)
// - `skipped_` says the phone gave up on it (`IMapSkipObserver`)
// - otherwise, gone from `MissingTilesStore` means it arrived -- `forget()` is
//   what removes it, and only an arrival calls that
// - still in the store means still waiting
//
// The one thing that must be remembered is the **order**, because the store
// shrinks as tiles land: `rows_` is a snapshot of the fetch-priority order taken
// when the sync starts, so a row never moves under the rider's eyes.
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
class TileSyncActivity final : public Activity, public IMapSkipObserver {
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
  enum class RowState : uint8_t { Waiting, Active, Done, Skipped };

  void renderScreen();
  // Repaints what changed: the whole list when a tile settles, or just the
  // active row while its bytes climb.
  void updateProgress();
  void drawList();
  void drawRow(int index, int y, int rowHeight);
  RowState stateOf(int index, uint32_t& received, uint32_t& total) const;
  // Rows that fit, and the first one shown -- the window follows the active row.
  int visibleRowCount() const;
  int firstVisibleRow() const;
  void listRect(int& x, int& y, int& w, int& h) const;
  void rowRect(int index, int& x, int& y, int& w, int& h) const;

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
    bool skipped = false;
  };
  std::unique_ptr<Row[]> rows_;
  uint32_t rowCount_ = 0;

  // Tiles the phone has given up on. Counted here as well as in the console's
  // tally because a `skip` for a tile that is not on this snapshot (a stale
  // phone-side list) still has to count toward finishing.
  uint32_t skipped_ = 0;

  // What the panel currently shows, so updateProgress() can tell a real change
  // from a poll.
  uint32_t drawnDone_ = 0;
  uint32_t drawnSkipped_ = 0;
  // millis() of the last active-row repaint. The bytes of a transfer in flight
  // change constantly and each repaint is a real waveform pass, so the moving
  // bar is rate-capped rather than drawn per chunk.
  uint32_t lastActiveDrawMs_ = 0;
  static constexpr uint32_t kActiveRowRefreshMs = 2000;

  // Last tileSeq already cleared out of the store. See drainTransferredTiles().
  uint32_t lastClearedTileSeq_ = 0;

  MapConsoleState consoleState_;
  MapBleConsole ble_{consoleState_};
  MapTransferReceiver transfer_;
};
