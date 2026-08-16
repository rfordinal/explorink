#include "TileSyncActivity.h"

#include <BlePositionServer.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "HeldTilesStore.h"
#include "MapByteFormat.h"
#include "MapMissingAnchor.h"
#include "MapPowerStatsProvider.h"
#include "MapTileReader.h"
#include "MappedInputManager.h"
#include "MissingTilesConsoleSource.h"
#include "MissingTilesStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "TILESYNC";

// Same card root the map's tile paths are built against (MapActivity.cpp).
constexpr const char* kTileRoot = "/trailink";

}  // namespace

// Stateless view onto MISSING_TILES for the `missing` command.
static MissingTilesConsoleSource g_missingTilesConsoleSource;

TileSyncActivity::TileSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("TileSync", renderer, mappedInput), transfer_(kTileRoot) {}

bool TileSyncActivity::armRun() {
  // The list the phone is about to read and the count it is about to be told have
  // to be the same list in the same order, and where the rider was last seen
  // decides what goes out first. The same anchor goes to the console source,
  // because `missing` re-sorts the store when the phone starts paging -- two
  // different orders would label the rows on this screen for tiles the phone was
  // never told about.
  const MissingTileAnchor anchor = missingTileAnchorFromLastFix();
  MISSING_TILES.sortByFetchPriority(anchor);
  g_missingTilesConsoleSource.setAnchor(anchor);
  const auto& hits = MISSING_TILES.hits();
  rowCount_ = static_cast<uint32_t>(hits.size());

  if (rowCount_ > 0) {
    rows_ = makeUniqueNoThrow<Row[]>(rowCount_);
    if (!rows_) {
      LOG_ERR(kLogTag, "OOM: %lu rows", static_cast<unsigned long>(rowCount_));
      // Without the snapshot there is no stable order to draw, and a fetch whose
      // screen cannot show what it is doing is worse than one that did not start.
      // The list is untouched, so the rider can try again.
      rowCount_ = 0;
      return false;
    }
    for (uint32_t i = 0; i < rowCount_; ++i) {
      rows_[i].tile = MapTileCoord{hits[i].z, hits[i].col, hits[i].row};
      rows_[i].unavailable = false;
    }
  }

  // Counters, not just the snapshot. The receiver counts "since the screen
  // opened", so a second run on the same visit would otherwise start with the
  // first one's arrivals already on the board.
  transfer_.resetCounters();
  consoleState_.clearSkips();
  skipped_ = 0;
  drawnDone_ = 0;
  drawnSkipped_ = 0;
  lastClearedTileSeq_ = transfer_.status().tileSeq;
  staleTiles_.clear();
  freshnessAsked_ = false;
  freshnessState_ = Freshness::Idle;
  freshnessAskedCount_ = 0;
  freshnessStale_ = 0;
  freshnessRound_ = 0;
  // A run that ended with work flagged must not have the next one act on it:
  // armRun() is also the re-entry path when a phone comes back after a
  // finished run (trackPhone).
  freshnessAskPending_ = false;
  freshnessRedrawPending_ = false;
  lastSettleMs_ = 0;
  // After staleTiles_.clear(), so the window is placed over what this run will
  // actually draw. Unconditional rather than inside the rowCount_ > 0 branch
  // above: a visit with nothing missing still has a grid, made of the tiles
  // queued for a freshness check, and sizing it on the missing list alone left
  // that case with no window at all.
  chooseWindow();
  return true;
}

void TileSyncActivity::onEnter() {
  Activity::onEnter();

  // NimBLEDevice::init() hangs at a low clock, and this screen can be entered
  // from an idle Home screen that is already throttled (verified 2026-08-04,
  // docs/power-management.md). HalPowerManager's BLE_SAFE_FREQ floor only
  // applies once the controller is enabled -- which is what the next line
  // does -- so the window before it needs closing here.
  powerManager.setPowerSaving(false);

  if (!freeink::BlePositionServer::getInstance().begin()) {
    // Plausible, not theoretical -- BLE init costs ~75 KB heap
    // (docs/map-memory.md). Without this the screen would sit in
    // Phase::Waiting forever, indistinguishable from a phone that just
    // has not connected yet. Same verdict mechanism armRun() failing uses
    // below, different string -- STR_TILE_SYNC_NO_ANSWER is a stalled
    // transfer, this is a stack that never came up.
    LOG_ERR(kLogTag, "BlePositionServer.begin() failed");
    enterPhase(Phase::Finished);
    verdict_ = StrId::STR_TILE_SYNC_BLE_FAILED;
    renderScreen();
    return;
  }
  // After begin(), so the characteristics exist before anything can be written
  // to them.
  transfer_.attach();

  if (!armRun()) {
    enterPhase(Phase::Finished);
    verdict_ = StrId::STR_MAP_FETCH_NOTHING;
    renderScreen();
    return;
  }

  consoleState_.setMissingTilesSource(&g_missingTilesConsoleSource);
  consoleState_.setSkipObserver(this);
  // What `have` answers from: every tile the map has drawn since boot that no
  // check has settled yet. This screen has no viewport of its own, and reading
  // a content_id anywhere else on the device would mean opening tiles for no
  // other reason (HeldTilesStore).
  consoleState_.setHeldTilesStore(&g_heldTiles);
  consoleState_.setStaleTiles(&staleTiles_);
  consoleState_.setStaleObserver(this);
  consoleState_.setLinkMtuProvider(
      +[]() -> uint16_t { return freeink::BlePositionServer::getInstance().negotiatedMtu(); });
  consoleState_.setLinkIntervalProvider(
      +[]() -> uint16_t { return freeink::BlePositionServer::getInstance().connIntervalMs(); });
  // Same power meter as the map screen. This screen is the worst case a power
  // review has -- radio saturated, SD writing, CPU pinned -- so it is the one
  // state most worth being able to poll while it runs.
  consoleState_.setPowerStatsProvider(&fillMapPowerStats);
  // Quoted in NEED_TILES below and reported by `info`. A tile built to another
  // version transfers fine, passes CRC and is then refused on open, so the
  // supplier needs the number before it sends anything (MapTileReader.h).
  consoleState_.setTileFormatVersion(MapTileReader::kFormatVersion);

  if (rowCount_ == 0) {
    // Worth a screen rather than a silent bounce back to the menu: the rider
    // picked this, and "nothing is missing" is good news. The freshness check
    // still runs: nothing missing does not mean nothing out of date, and this
    // is the case the whole feature exists for.
    verdict_ = StrId::STR_MAP_FETCH_NOTHING;
    LOG_INF(kLogTag, "nothing missing, nothing to ask for");
    drawnPhoneListening_ = phoneListening();
    hadPhone_ = drawnPhoneListening_;
    if (drawnPhoneListening_) {
      askAboutFreshness();
      enterPhase(Phase::Finished);
    } else {
      // A phone connecting synchronously with begin() above never happens on
      // real hardware (measured: ~1.8 s). trackPhone() takes it from here,
      // same as the missing-tiles path below -- see its comment.
      enterPhase(Phase::Waiting);
    }
    renderScreen();
    return;
  }

  // Advertising now; nothing to ask until a phone subscribes to the command
  // channel. trackPhone() does the asking when one does -- see phoneListening().
  enterPhase(Phase::Waiting);
  verdict_ = StrId::STR_MAP_FETCH_DONE;
  drawnPhoneListening_ = phoneListening();
  hadPhone_ = drawnPhoneListening_;
  LOG_INF(kLogTag, "%lu tiles to ask for, waiting for a phone", static_cast<unsigned long>(rowCount_));
  renderScreen();
  if (drawnPhoneListening_) askForTiles();
}

void TileSyncActivity::askAboutFreshness() {
  if (freshnessAsked_) return;
  if (SETTINGS.mapTileFreshnessMode == CrossPointSettings::MAP_TILE_FRESHNESS_OFF) return;
  // Nothing drawn since boot means no content_id to offer, and `have` would
  // answer `have=none` anyway. Saying nothing is the honest version of that.
  //
  // Nothing *pending* is the other silent case, and it is the good one: every
  // tile the map has drawn has already been compared, either by an earlier
  // visit here or by the map screen's own Live check (HeldTilesStore drains).
  const uint32_t pending = static_cast<uint32_t>(g_heldTiles.pendingCount());
  if (!g_heldTiles.valid() || pending == 0) {
    LOG_INF(kLogTag, "freshness: nothing pending of %lu held, nothing to check",
            static_cast<unsigned long>(g_heldTiles.size()));
    return;
  }
  // One round's worth, not the whole store. A listing runs its blocks back to
  // back on the activity task and each waits on the peer's confirm, so an
  // unbounded one freezes the buttons -- see HeldTilesStore::kMaxPerListing.
  // onCheckFinished() asks again while anything is still pending, so a visit
  // still drains the store; it just does it in rounds.
  const uint32_t round =
      pending < HeldTilesStore::kMaxPerListing ? pending : static_cast<uint32_t>(HeldTilesStore::kMaxPerListing);
  // `fmt <version>` for the same reason the map screen sends it: a device with
  // nothing missing never sends NEED_TILES, so without this the phone falls
  // back to a stale default and compares against the wrong /v<N>/ index tree
  // (docs/tile-freshness.md).
  char line[48];
  snprintf(line, sizeof(line), "CHECK_TILES %lu fmt %u", static_cast<unsigned long>(round),
           static_cast<unsigned>(MapTileReader::kFormatVersion));
  if (!freeink::BlePositionServer::getInstance().sendCommandReply(line)) {
    LOG_ERR(kLogTag, "CHECK_TILES not delivered");
    return;
  }
  freshnessAsked_ = true;
  freshnessRound_ = round;
  freshnessState_ = Freshness::Asking;
  LOG_INF(kLogTag, "freshness: asked about %lu of %lu pending, %lu held", static_cast<unsigned long>(round),
          static_cast<unsigned long>(pending), static_cast<unsigned long>(g_heldTiles.size()));
  // The rider is watching a screen that has just stopped fetching, and the
  // panel would otherwise keep showing the fetch's own verdict. Flagged, not
  // painted: every other caller already repaints immediately after this
  // returns, so painting here made it two e-ink refreshes per ask.
  freshnessRedrawPending_ = true;
}

bool TileSyncActivity::formatFreshness(char* out, size_t size) const {
  switch (freshnessState_) {
    case Freshness::Idle:
      return false;
    case Freshness::Asking:
      // What this visit will have covered once the round on the wire lands, not
      // the round on its own -- a rider watching rounds tick by wants the total
      // going up, not a number that resets to 12 each time.
      snprintf(out, size, tr(STR_TILE_SYNC_CHECKING), static_cast<int>(freshnessAskedCount_ + freshnessRound_));
      return true;
    case Freshness::Current:
      snprintf(out, size, tr(STR_TILE_SYNC_ALL_CURRENT), static_cast<int>(freshnessAskedCount_));
      return true;
    case Freshness::Stale:
      // "downloading" is only true while something is still owed. Every stale
      // tile leaves staleTiles_ when its replacement lands
      // (drainTransferredTiles), so an empty list means the work is done --
      // without this the line sat on "downloading" after the last tile had
      // already arrived, seen on the panel 2026-08-13 next to a summary that
      // said 35 kB had moved.
      snprintf(out, size, I18N.get(staleTiles_.count() > 0 ? StrId::STR_TILE_SYNC_STALE : StrId::STR_TILE_SYNC_UPDATED),
               static_cast<int>(freshnessStale_), static_cast<int>(freshnessAskedCount_));
      return true;
    case Freshness::Unknown:
      // Deliberately not "everything is current". The phone could not read the
      // index, so it is claiming nothing, and the two must never read alike.
      snprintf(out, size, "%s", tr(STR_TILE_SYNC_CHECK_UNKNOWN));
      return true;
  }
  return false;
}

void TileSyncActivity::onTileStale(uint8_t z, uint32_t col, uint32_t row) {
  if (!staleTiles_.add(z, col, row)) return;
  LOG_INF(kLogTag, "freshness: z%u %lu/%lu is out of date", static_cast<unsigned>(z), static_cast<unsigned long>(col),
          static_cast<unsigned long>(row));
  // **The mark just changed and the panel has to be told.** This tile stops
  // being a dot and becomes a frame right here -- it has gone from waiting on
  // an answer to waiting on bytes -- and without this the screen held the last
  // frame until the round's closing `checked` arrived. Measured on hardware
  // 2026-08-13: a real 42-tile check repainted only between rounds, so the
  // transition this grid exists to show was never actually drawn while it
  // happened.
  //
  // A flag, not a repaint: this runs inside the console's dispatch of `stale`,
  // before its terminating OK, which is the one place work must not be started
  // (see freshnessRedrawPending_'s declaration).
  freshnessRedrawPending_ = true;
}

void TileSyncActivity::onCheckFinished(bool known, uint16_t staleCount) {
  if (!known) {
    // The phone could not read the index. Not the same as nothing being out of
    // date, and not reported as such -- on screen either.
    LOG_INF(kLogTag, "freshness: phone could not check (no index)");
    freshnessState_ = Freshness::Unknown;
    freshnessRedrawPending_ = true;
    return;
  }
  // Cumulative over the visit, not per round: the rider is told how much of
  // their card has been checked, which is one fact however many listings it
  // took to ask about it.
  freshnessAskedCount_ += freshnessRound_;
  freshnessStale_ += staleCount;
  freshnessState_ = freshnessStale_ > 0 ? Freshness::Stale : Freshness::Current;
  LOG_INF(kLogTag, "freshness: %u out of date this round, %lu of %lu checked so far", static_cast<unsigned>(staleCount),
          static_cast<unsigned long>(freshnessAskedCount_), static_cast<unsigned long>(g_heldTiles.size()));
  // Every `stale` line has landed by now -- `checked` is what closes the
  // listing -- so this is the first moment the grid window can cover them. It
  // was sized on the missing list alone in armRun(), which on a visit with
  // nothing missing meant no window at all. Pure arithmetic over lists already
  // in RAM, so it is safe to do here; the repaint it feeds is not.
  chooseWindow();
  freshnessRedrawPending_ = true;

  // Next round, if the store still holds anything unanswered. **Flagged, not
  // done here** -- see the flag's declaration: this runs inside the dispatch of
  // `checked`, before its terminating `OK`, and asking from here put a confirm
  // wait and a repaint in front of a reply the phone was still waiting for.
  //
  // Bounded and cannot spin: markAskedChecked() has already settled this
  // round's entries, so pendingCount() is strictly smaller each time and
  // reaches zero. Only on a `known` answer -- `checked unknown` settles
  // nothing, so re-asking on it would loop forever against a phone that cannot
  // read the index.
  if (g_heldTiles.pendingCount() > 0) freshnessAskPending_ = true;
}

bool TileSyncActivity::phoneListening() const {
  return freeink::BlePositionServer::getInstance().isCommandSubscribed();
}

void TileSyncActivity::askForTiles() {
  // Unsolicited indication on the command channel -- the one place the device
  // starts a conversation instead of answering one.
  //
  // Its return value is NOT evidence that a phone heard it: NimBLE accepts a
  // line into its queue whether or not anybody is subscribed. phoneListening()
  // is the real check and the caller has already made it; this only catches a
  // link that died between the two.
  char line[48];
  snprintf(line, sizeof(line), "NEED_TILES %lu fmt %u", static_cast<unsigned long>(rowCount_),
           static_cast<unsigned>(MapTileReader::kFormatVersion));
  if (!freeink::BlePositionServer::getInstance().sendCommandReply(line)) {
    LOG_ERR(kLogTag, "NEED_TILES not delivered");
    return;
  }
  LOG_INF(kLogTag, "asked for %lu tiles", static_cast<unsigned long>(rowCount_));
  enterPhase(Phase::Running);
  startedMs_ = millis();
  lastSettleMs_ = startedMs_;
  // Fresh run, fresh stall bookkeeping -- see updateProgress()'s
  // active-but-silent check.
  lastReceivedBytes_ = 0;
  lastProgressMs_ = 0;
  transferWasActive_ = false;
  renderScreen();
}

void TileSyncActivity::trackPhone() {
  const bool listening = phoneListening();
  if (listening == drawnPhoneListening_) return;
  drawnPhoneListening_ = listening;

  // A phone arriving after the run ended is a second chance, not noise. This used
  // to return early on Phase::Finished, so a rider who watched a run end with
  // nothing and *then* connected their phone got no ask, no message, and no way
  // to tell the screen had stopped listening -- found on hardware 2026-08-11
  // while testing the stall verdict, with a central that subscribed to a finished
  // screen and was ignored.
  //
  // Bounded by connect events, not a poll: this only runs on a false->true
  // transition, so a phone that stays connected cannot make it loop.
  if (phase_ == Phase::Finished && listening) {
    if (!armRun()) return;
    LOG_INF(kLogTag, "phone arrived after the run, asking again (%lu tiles)", static_cast<unsigned long>(rowCount_));
    hadPhone_ = true;
    if (rowCount_ > 0) {
      askForTiles();
    } else {
      askAboutFreshness();
      verdict_ = StrId::STR_MAP_FETCH_NOTHING;
      renderScreen();
    }
    return;
  }
  if (phase_ == Phase::Finished) return;

  if (listening) {
    hadPhone_ = true;
    LOG_INF(kLogTag, "phone subscribed, asking");
    // It has to be here rather than only in onEnter(). Measured on hardware
    // 2026-08-09: onEnter() tests phoneListening() at t=0, the moment BLE
    // starts, and the phone does not actually subscribe until ~+1.8 s. So an
    // onEnter()-only ask could never fire -- a bug in the timing, not the
    // logic, which unit tests could not catch.
    //
    // **One conversation at a time, missing tiles first.** Both asks used to go
    // out here, 15 ms apart, and that is a bug on the wire: the phone answers
    // each with a command, so two conversations run on one channel and each
    // one's terminating `OK` can end the other's listing. Measured on hardware
    // 2026-08-11 -- the device asked for 20 tiles, the phone read the list as
    // empty ("0 tiles of 20"), sent no tiles and no skips, and all 20 rows sat
    // at "waiting" forever with nothing to explain it. Freshness now goes out
    // when the fetch settles (updateProgress) or when there is nothing to
    // fetch at all.
    if (rowCount_ > 0) {
      askForTiles();
    } else {
      askAboutFreshness();
      // Nothing to fetch -- the freshness ask above was the only reason this
      // screen was still waiting. Same verdict onEnter() would have shown had
      // the phone been there from t=0.
      enterPhase(Phase::Finished);
      renderScreen();
    }
    return;
  }

  // The phone walked away. Whatever was in flight died with the link, and the
  // screen says so rather than sitting on a bar that will never move again.
  LOG_INF(kLogTag, "phone gone, back to waiting");
  enterPhase(Phase::Waiting);
  renderScreen();
}

void TileSyncActivity::onExit() {
  // Before end(): the hooks point at a member of this activity, and this
  // activity is about to be deleted (main.cpp's exitActivity). A transfer still
  // in flight loses its .part file here rather than surviving into a screen with
  // no BLE link.
  transfer_.detach();
  consoleState_.setSkipObserver(nullptr);
  consoleState_.setStaleObserver(nullptr);
  consoleState_.setStaleTiles(nullptr);
  freeink::BlePositionServer::getInstance().end();
  // Leaving is the checkpoint: whatever this sync cleared has to reach the card,
  // or the phone sends the same tiles again after a restart. A no-op when
  // nothing changed.
  MISSING_TILES.flushIfDirty();
  rows_.reset();
  rowCount_ = 0;
  Activity::onExit();
}

void TileSyncActivity::enterPhase(Phase phase) {
  phase_ = phase;
  phaseEnteredMs_ = millis();
}

// Defect: this used to be `return isRunning()`, true for the screen's entire
// visit -- Waiting with no phone, or Finished with the rider long gone, held
// 160 MHz + no-auto-sleep exactly as hard as an active transfer
// (docs/ble-review-2026-08.md, "Power": preventAutoSleep() pins 160 MHz for
// the whole screen). Fixed by gating on how long ago the screen last had
// something to show for itself.
//
// No separate "transfer is active" check: kStallVerdictMs (30 s) already
// bounds how long Running can go without a tile settling or a byte of
// progress before updateProgress() calls enterPhase(Finished) itself, and
// both of those events also re-stamp phaseEnteredMs_ below -- so a healthy
// transfer, however many tiles or minutes it takes, can never let this clock
// reach kIdleSleepTimeoutMs (10 min) on its own. A transfer that genuinely
// stalls converts to Finished well before 10 minutes and starts that
// countdown from there, same as any other Finished screen.
//
// isRunning() stays as a fast path: no BLE session at all (T3.5's
// STR_TILE_SYNC_BLE_FAILED Finished screen, begin() never having set
// begun_) has nothing to hold the clock for regardless of the timeout.
bool TileSyncActivity::preventAutoSleep() {
  if (!freeink::BlePositionServer::getInstance().isRunning()) return false;
  return millis() - phaseEnteredMs_ < kIdleSleepTimeoutMs;
}

void TileSyncActivity::loop() {
  Activity::loop();

  // The phone's side of the conversation: `missing` to read the list, `skip` for
  // a tile it cannot supply. poll() returning true would mean a command changed
  // something on a map screen that is not up -- nothing to redraw here.
  ble_.poll();

  // The freshness check's follow-up work, out here rather than in the observer
  // that decided on it. onCheckFinished() runs inside the console's dispatch of
  // `checked`, before the terminating `OK`; starting a 3 s confirm wait or a
  // 500-1700 ms e-ink repaint there delays a reply the phone is waiting for.
  // Same shape as the console's own redraw flag: signal in, act after poll().
  //
  // Ask before repaint, so one round costs one e-ink refresh: the ask sets the
  // redraw flag itself, and the repaint below then shows the round it just
  // started rather than the one that finished.
  if (freshnessAskPending_) {
    freshnessAskPending_ = false;
    freshnessAsked_ = false;
    askAboutFreshness();
  }
  if (freshnessRedrawPending_) {
    freshnessRedrawPending_ = false;
    renderScreen();
  }

  // Advertising state and connection parameter requests, once per tick. Same
  // reason MapActivity::loop() does it: a restart that failed inside the
  // NimBLE disconnect callback cannot be retried on that task
  // (BlePositionServer.h, "Advertising state"), and this screen is the other
  // one that runs the BLE server -- a link dropped here must come back
  // without leaving the screen. transfer_.status().active is this screen's
  // own transfer receiver, same as MapActivity's.
  freeink::BlePositionServer::getInstance().serviceAdvertising(transfer_.status().active);

  // The other half of the same deal: a transfer status line parked because the
  // command channel held the connection's single indication slot. This screen
  // is exactly where that collides -- `missing`/`have` replies and file pushes
  // on one link (BlePositionServer.h, sendTransferStatus).
  freeink::BlePositionServer::getInstance().flushTransferStatus();

  trackPhone();
  drainTransferredTiles();
  updateProgress();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) leave();
}

void TileSyncActivity::leave() {
  if (phase_ == Phase::Running && phoneListening()) {
    // The phone is mid-push and has to be told: the device cannot stop it from
    // this end. The transfer channel's abort opcode (0x03) is a frame the
    // *central* writes, so a peripheral's only cancel is a word on the command
    // channel.
    //
    // Skip it if the last reply already went unconfirmed: a peer that stopped
    // confirming indications will not hear this one either, and the 3 s wait
    // for a confirm that was never coming is exactly the freeze this exists to
    // avoid (docs/ble-review-2026-08.md, "Console flush can freeze the
    // activity task").
    if (freeink::BlePositionServer::getInstance().lastConfirmTimedOut()) {
      LOG_ERR(kLogTag, "FETCH_CANCEL skipped: last confirm already timed out");
    } else if (!freeink::BlePositionServer::getInstance().sendCommandReply("FETCH_CANCEL")) {
      LOG_ERR(kLogTag, "FETCH_CANCEL not delivered");
    }
    LOG_INF(kLogTag, "cancelled by rider");
  }
  onGoHome(HomeMenuItem::TILE_SYNC);
}

void TileSyncActivity::onTileSkipped(uint8_t z, uint32_t col, uint32_t row) {
  ++skipped_;
  for (uint32_t i = 0; i < rowCount_; ++i) {
    if (rows_[i].tile.z == z && rows_[i].tile.col == col && rows_[i].tile.row == row) {
      rows_[i].unavailable = true;
      return;
    }
  }
  // Not on this snapshot: the phone is working from a list read before the
  // rider restarted the sync. Counted anyway, so the run can still finish.
  LOG_DBG(kLogTag, "skip for a tile not on this run: z%u %lu/%lu", static_cast<unsigned>(z),
          static_cast<unsigned long>(col), static_cast<unsigned long>(row));
}

void TileSyncActivity::drainTransferredTiles() {
  const MapTransferReceiver::Status transfer = transfer_.status();
  if (!transfer.lastTileValid || transfer.tileSeq == lastClearedTileSeq_) return;
  lastClearedTileSeq_ = transfer.tileSeq;

  // A stale tile's replacement lands here too, and it is not on the missing
  // list -- it never was. Clearing it here rubs its dot off the grid and arms
  // the ping-pong guard, so a cache that keeps serving the old copy is given up
  // on rather than fetched forever (StaleTilesList::add). The map screen has
  // always done this (MapActivity::drainTransferredTiles); this screen did not,
  // which left a replaced tile marked out of date for the rest of the visit.
  if (staleTiles_.contains(transfer.lastTile.z, transfer.lastTile.col, transfer.lastTile.row)) {
    staleTiles_.onArrived(transfer.lastTile.z, transfer.lastTile.col, transfer.lastTile.row);
    LOG_INF(kLogTag, "freshness: z%u %lu/%lu replaced", static_cast<unsigned>(transfer.lastTile.z),
            static_cast<unsigned long>(transfer.lastTile.col), static_cast<unsigned long>(transfer.lastTile.row));
    renderScreen();
  }

  if (!MISSING_TILES.forget(transfer.lastTile.z, transfer.lastTile.col, transfer.lastTile.row)) {
    // A tile the device never hatched -- a corridor update pushed ahead of a
    // ride, say. Nothing to clear, and not an error.
    return;
  }
  LOG_INF(kLogTag, "z%u %lu/%lu arrived, dropped from the list", static_cast<unsigned>(transfer.lastTile.z),
          static_cast<unsigned long>(transfer.lastTile.col), static_cast<unsigned long>(transfer.lastTile.row));
}

bool TileSyncActivity::stillMissing(const MapTileCoord& tile) const {
  for (const MissingTileHit& hit : MISSING_TILES.hits()) {
    if (hit.z == tile.z && hit.col == tile.col && hit.row == tile.row) return true;
  }
  return false;
}

TileSyncActivity::RowState TileSyncActivity::stateOf(int index, uint32_t& received, uint32_t& total) const {
  received = 0;
  total = 0;
  const Row& row = rows_[index];

  const MapTransferReceiver::Status transfer = transfer_.status();
  if (transfer.active && transfer.activeTileValid && transfer.activeTile.z == row.tile.z &&
      transfer.activeTile.col == row.tile.col && transfer.activeTile.row == row.tile.row) {
    received = transfer.received;
    total = transfer.total;
    return RowState::Active;
  }
  if (row.unavailable) return RowState::Missing;
  // Gone from the store means it landed: forget() is the only thing that
  // removes an entry, and only an arrival calls it.
  return stillMissing(row.tile) ? RowState::Waiting : RowState::Done;
}

void TileSyncActivity::parentOf(const MapTileCoord& tile, uint16_t& pc, uint16_t& pr) {
  // z11 is the coarsest LOD the map reads and z13 the finest (docs/zoom-rungs.md,
  // "The ladder"), so the shift is 0..2 and never negative. Clamped rather than
  // asserted: a tile recorded by an older firmware with a coarser LOD would
  // otherwise shift by a negative amount, which is undefined behaviour, and
  // landing in the wrong cell of a toy is not worth a crash.
  const int down = tile.z > 11 ? tile.z - 11 : 0;
  pc = static_cast<uint16_t>(tile.col >> down);
  pr = static_cast<uint16_t>(tile.row >> down);
}

size_t TileSyncActivity::interestCount() const { return rowCount_ + g_heldTiles.pendingCount() + staleTiles_.count(); }

size_t TileSyncActivity::downloadCount() const { return rowCount_ + staleTiles_.count(); }

MapTileCoord TileSyncActivity::interestAt(size_t index) const {
  // [0, rowCount_) missing, then the stale ones -- together the download queue,
  // drawn as frames -- then the tiles still waiting on a check, drawn as dots.
  // Contiguous in that order so drawParent() can walk one range per mark.
  if (index < rowCount_) return rows_[index].tile;
  size_t rest = index - rowCount_;

  if (rest < staleTiles_.count()) {
    const StaleTilesList::Entry& e = staleTiles_.at(rest);
    return MapTileCoord{e.z, e.col, e.row};
  }
  rest -= staleTiles_.count();

  // The nth entry that is still unsettled. Walked rather than indexed: the
  // store keeps pending and settled entries in one array so that re-recording
  // a tile finds it wherever it sits.
  for (size_t i = 0; i < g_heldTiles.size(); ++i) {
    const HeldTileEntry& e = g_heldTiles.at(i);
    if (e.checked()) continue;
    if (rest == 0) return MapTileCoord{e.z, e.col, e.row};
    --rest;
  }
  return MapTileCoord{};
}

void TileSyncActivity::chooseWindow() {
  const size_t interest = interestCount();
  if (interest == 0) return;

  uint16_t minCol = 0xFFFF, maxCol = 0, minRow = 0xFFFF, maxRow = 0;
  for (size_t i = 0; i < interest; ++i) {
    uint16_t pc = 0, pr = 0;
    parentOf(interestAt(i), pc, pr);
    if (pc < minCol) minCol = pc;
    if (pc > maxCol) maxCol = pc;
    if (pr < minRow) minRow = pr;
    if (pr > maxRow) maxRow = pr;
  }

  const uint32_t spanCols = static_cast<uint32_t>(maxCol - minCol) + 1;
  const uint32_t spanRows = static_cast<uint32_t>(maxRow - minRow) + 1;

  // Everything fits: the whole spread is the viewport, and a run whose tiles sit
  // in one parent gets that parent filling the screen.
  if (spanCols <= kMaxWindowCols && spanRows <= kMaxWindowRows) {
    windowCol_ = minCol;
    windowRow_ = minRow;
    windowCols_ = static_cast<uint8_t>(spanCols);
    windowRows_ = static_cast<uint8_t>(spanRows);
    offWindow_ = 0;
    return;
  }

  // Too spread out to draw honestly. Put the window where the most tiles are and
  // let the rest fall outside it -- the bar is what says how much is left.
  //
  // Only occupied parents are tried as the top-left corner: a window whose corner
  // holds nothing can always be slid onto one that does without losing a tile, so
  // the best corner is among them. That makes this O(interestCount()^2) -- 50,000
  // compares at the 200-entry missing cap (MissingTilesStore::kMaxEntries) plus
  // the 24-entry stale one (StaleTilesList), once per run.
  uint32_t best = 0;
  windowCol_ = minCol;
  windowRow_ = minRow;
  for (size_t i = 0; i < interest; ++i) {
    uint16_t oc = 0, orr = 0;
    parentOf(interestAt(i), oc, orr);
    uint32_t inside = 0;
    for (size_t j = 0; j < interest; ++j) {
      uint16_t pc = 0, pr = 0;
      parentOf(interestAt(j), pc, pr);
      if (pc >= oc && pc < oc + kMaxWindowCols && pr >= orr && pr < orr + kMaxWindowRows) ++inside;
    }
    if (inside > best) {
      best = inside;
      windowCol_ = oc;
      windowRow_ = orr;
    }
  }
  // Shrink onto what the window actually caught. Without this a set with one
  // distant outlier keeps the full 6 x 8 cap, and three occupied parents get
  // drawn tiny in the corner of a mostly empty grid.
  uint16_t inCol = 0xFFFF, inMaxCol = 0, inRow = 0xFFFF, inMaxRow = 0;
  for (size_t i = 0; i < interest; ++i) {
    uint16_t pc = 0, pr = 0;
    parentOf(interestAt(i), pc, pr);
    if (pc < windowCol_ || pc >= windowCol_ + kMaxWindowCols) continue;
    if (pr < windowRow_ || pr >= windowRow_ + kMaxWindowRows) continue;
    if (pc < inCol) inCol = pc;
    if (pc > inMaxCol) inMaxCol = pc;
    if (pr < inRow) inRow = pr;
    if (pr > inMaxRow) inMaxRow = pr;
  }
  windowCol_ = inCol;
  windowRow_ = inRow;
  windowCols_ = static_cast<uint8_t>(inMaxCol - inCol + 1);
  windowRows_ = static_cast<uint8_t>(inMaxRow - inRow + 1);
  offWindow_ = static_cast<uint32_t>(interest - best);
  LOG_INF(kLogTag, "grid window at z11 %u/%u, %lu of %lu tiles outside it", static_cast<unsigned>(windowCol_),
          static_cast<unsigned>(windowRow_), static_cast<unsigned long>(offWindow_),
          static_cast<unsigned long>(interest));
}

void TileSyncActivity::summaryRect(int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  x = metrics.contentSidePadding;
  w = renderer.getScreenWidth() - metrics.contentSidePadding * 2;
  y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing / 2;
  h = renderer.getLineHeight(UI_10_FONT_ID);
}

void TileSyncActivity::gridRect(int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  // Below the header, the summary line, the overall bar and the percentage
  // GUI.drawProgressBar centres 15 px under it. Reserved whether or not the bar
  // is drawn, so the grid does not jump as the phase changes.
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + lineHeight +
                  metrics.progressBarHeight + 15 + lineHeight;

  x = metrics.contentSidePadding;
  w = pageWidth - metrics.contentSidePadding * 2;
  y = top;
  h = pageHeight - top - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
}

bool TileSyncActivity::drawParent(int px, int py, int size, uint16_t pc, uint16_t pr) {
  const int leaf = size / kLeavesPerParent;
  bool any = false;

  // **The download queue: missing tiles and stale ones alike.** The mark says
  // what the device is waiting on, not why it is waiting. A tile that came back
  // stale has stopped waiting on an answer and started waiting on bytes, which
  // is exactly what a missing tile is doing, so it stops being a dot and
  // becomes a frame. Watching dots turn into frames and frames then vanish is
  // the whole progress story of this screen in two marks.
  for (size_t i = 0; i < downloadCount(); ++i) {
    if (i < rowCount_) {
      uint32_t received = 0;
      uint32_t total = 0;
      // The one question the grid asks. Waiting, on the wire and refused all
      // draw the same square: it is not on the card yet. Stale entries have no
      // row state -- they leave the list when the replacement lands.
      if (stateOf(static_cast<int>(i), received, total) == RowState::Done) continue;
    }

    const MapTileCoord tile = interestAt(i);
    uint16_t tpc = 0, tpr = 0;
    parentOf(tile, tpc, tpr);
    if (tpc != pc || tpr != pr) continue;

    if (!any) {
      // The parent's own frame, drawn once something inside it survives. An empty
      // parent gets nothing at all, which is how a finished corner of the grid
      // clears itself.
      renderer.drawRect(px, py, size, size);
      any = true;
    }

    // Position inside the parent, in z13 leaves. z11 spans all four, z12 two,
    // z13 one -- so one expression covers every LOD the map reads.
    const int down = tile.z < 13 ? 13 - tile.z : 0;
    const int span = 1 << down;
    const int lx = static_cast<int>((tile.col << down) & (kLeavesPerParent - 1));
    const int ly = static_cast<int>((tile.row << down) & (kLeavesPerParent - 1));

    // The z12 quadrant this tile lives in, when the tile is smaller than one.
    // Structure the eye can read: without it a lone z13 square floats in an empty
    // parent with nothing to say how deep it is.
    if (span < kLeavesPerParent) {
      const int qx = px + (lx & ~1) * leaf;
      const int qy = py + (ly & ~1) * leaf;
      renderer.drawRect(qx, qy, leaf * 2, leaf * 2);
    }

    // The tile itself. Inset so neighbouring squares stay separate squares
    // instead of merging into one blot.
    const int fx = px + lx * leaf + kTileInset;
    const int fy = py + ly * leaf + kTileInset;
    const int fw = span * leaf - kTileInset * 2;
    if (fw <= 0) continue;

    // An outline, never a fill. Tried filled first and checked it against real
    // device data (30 tiles hatched off an unbuilt area, 2026-08-13): riders at
    // rungs 3-6 read z11, so a run collects whole missing z11 parents, and
    // filling those painted eight solid 128 px squares -- most of the panel
    // black. A lot of ink for a screen that redraws per tile, and the nesting
    // buried underneath. Filling an area says nothing its outline does not.
    //
    // Thicker than the scaffolding frames around it, and scaled to the cell, so
    // the tile that is actually missing stays the strongest line on screen.
    const int thickness = leaf / 8 < 2 ? 2 : leaf / 8;
    renderer.drawRect(fx, fy, fw, fw, thickness, true);
  }

  // The check queue: tiles on the card that no answer covers yet. A dot, not an
  // outline -- these are not missing, and drawing them the way a missing one is
  // drawn would say the opposite of what is true.
  //
  // A dot leaves in one of two ways, and they read differently on purpose: the
  // phone says the tile is current and it simply goes, or the phone says it is
  // stale and it turns into a frame, because it has stopped waiting on an
  // answer and started waiting on bytes.
  for (size_t i = downloadCount(); i < interestCount(); ++i) {
    const MapTileCoord tile = interestAt(i);
    uint16_t tpc = 0, tpr = 0;
    parentOf(tile, tpc, tpr);
    if (tpc != pc || tpr != pr) continue;

    // **No parent frame here.** The frames belong to the missing tiles: they
    // are the scaffolding that says how deep a hatched square sits. A dot
    // already carries its own depth in its size, so framing a parent that
    // holds nothing but dots spends ink on nothing and buries the one thing
    // worth watching -- the dots going out one by one. A parent that holds
    // both gets its frame from the loop above, which is correct: something in
    // it really is missing.
    const int down = tile.z < 13 ? 13 - tile.z : 0;
    const int span = 1 << down;
    const int lx = static_cast<int>((tile.col << down) & (kLeavesPerParent - 1));
    const int ly = static_cast<int>((tile.row << down) & (kLeavesPerParent - 1));

    // Centred in the tile's own cell, sized from that cell so the LOD still
    // reads, capped so it stays a mark rather than a filled square.
    const int cellPx = span * leaf;
    int dot = cellPx / kDotDivisor;
    if (dot > kMaxDotPx) dot = kMaxDotPx;
    if (dot < kMinDotPx) dot = kMinDotPx;
    const int cx = px + lx * leaf + cellPx / 2;
    const int cy = py + ly * leaf + cellPx / 2;
    // A rounded rect whose corner radius is half its side is a disc, so this
    // needs no new renderer primitive -- fillRoundedRect() already clamps the
    // radius to half the smaller side (GfxRenderer.cpp).
    renderer.fillRoundedRect(cx - dot / 2, cy - dot / 2, dot, dot, dot / 2, Color::Black);
  }

  return any;
}

void TileSyncActivity::drawGrid(int top) {
  int gx, gy, gw, gh;
  gridRect(gx, gy, gw, gh);
  // The finished screen writes more above the grid than the running one does, so
  // it passes its own floor rather than letting the squares land under the text.
  if (top > gy) {
    gh -= top - gy;
    gy = top;
  }
  if (gh <= 0) return;
  renderer.fillRect(gx, gy, gw, gh, false);

  // Nothing missing *and* nothing stale. A freshness-only visit still draws a
  // grid -- the dots are the whole point of it -- so this is the empty case for
  // both, not just for the fetch.
  if (interestCount() == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, gy + gh / 2, I18N.get(verdict_));
    return;
  }

  // Square cells, whichever axis runs out first, centred in what is left. Rounded
  // down to a multiple of the leaf count so a z13 leaf lands on whole pixels --
  // an off-by-one there shows up as frames that do not meet.
  int cell = gw / windowCols_;
  const int byHeight = gh / windowRows_;
  if (byHeight < cell) cell = byHeight;
  if (cell > kMaxCellPx) cell = kMaxCellPx;
  cell -= cell % kLeavesPerParent;
  if (cell < kLeavesPerParent) return;  // nothing legible to draw

  const int originX = gx + (gw - cell * windowCols_) / 2;
  const int originY = gy + (gh - cell * windowRows_) / 2;

  for (int r = 0; r < windowRows_; ++r) {
    for (int c = 0; c < windowCols_; ++c) {
      // North up, east right: the slippy-tile row grows south and the column
      // grows east, which is already the screen's own axes. No transform.
      drawParent(originX + c * cell, originY + r * cell, cell, static_cast<uint16_t>(windowCol_ + c),
                 static_cast<uint16_t>(windowRow_ + r));
    }
  }
}

void TileSyncActivity::formatBytes(uint32_t bytes, char* out, size_t outSize) {
  // Definition moved to MapByteFormat.h so the map screen's debug readout states
  // a transfer the same way this screen does -- it used to print raw bytes.
  mapfmt::formatBytes(bytes, out, outSize);
}

void TileSyncActivity::formatDuration(uint32_t seconds, char* out, size_t outSize) {
  mapfmt::formatDuration(seconds, out, outSize);
}

void TileSyncActivity::formatSummary(char* out, size_t outSize) const {
  const MapTransferReceiver::Status transfer = transfer_.status();
  // Still what decides the ETA -- a tile the supplier lacks does shorten the
  // run, even though it must not fill the bar.
  const uint32_t settled = transfer.completed + skipped_;

  char moved[16];
  formatBytes(transfer.completedBytes + (transfer.active ? transfer.received : 0), moved, sizeof(moved));

  // Stated apart from the arrivals, and only when there are any: it is a
  // different outcome, not a slower one, and the rider can do nothing about it.
  char unavailable[32] = {};
  if (skipped_ > 0) {
    snprintf(unavailable, sizeof(unavailable), "   %lu %s", static_cast<unsigned long>(skipped_),
             tr(STR_TILE_SYNC_ROW_MISSING));
  }

  // The rate and the remainder both need a tile to have actually landed. Before
  // that there is no honest number, so the line simply stops there rather than
  // showing a zero or a guess that will be wrong by an order of magnitude.
  const uint32_t elapsedMs = startedMs_ != 0 ? millis() - startedMs_ : 0;
  if (transfer.completed == 0 || elapsedMs < 1000) {
    snprintf(out, outSize, "%lu / %lu%s   %s", static_cast<unsigned long>(transfer.completed),
             static_cast<unsigned long>(transferTotal()), unavailable, moved);
    return;
  }

  const uint32_t elapsedS = elapsedMs / 1000;
  const uint32_t rateBps = transfer.completedBytes / (elapsedS > 0 ? elapsedS : 1);

  char eta[16] = {};
  const uint32_t remaining = rowCount_ > settled ? rowCount_ - settled : 0;
  if (remaining > 0) {
    // Time per settled tile, times what is left. Tiles vary a lot in size
    // (6 KB to 75 KB in one real fetch), so this is an estimate that firms up
    // as the run goes -- which is what an ETA is, and better than no answer at
    // all while a rider decides whether to wait.
    formatDuration(elapsedS * remaining / settled, eta, sizeof(eta));
  }

  if (eta[0] != '\0') {
    snprintf(out, outSize, "%lu / %lu%s   %s  %lu.%lu kB/s  %s %s", static_cast<unsigned long>(transfer.completed),
             static_cast<unsigned long>(transferTotal()), unavailable, moved,
             static_cast<unsigned long>(rateBps / 1000), static_cast<unsigned long>((rateBps % 1000) / 100), eta,
             tr(STR_TILE_SYNC_LEFT));
  } else {
    snprintf(out, outSize, "%lu / %lu%s   %s", static_cast<unsigned long>(transfer.completed),
             static_cast<unsigned long>(transferTotal()), unavailable, moved);
  }
}

void TileSyncActivity::renderScreen() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  // The channel is in the header, not left to be guessed. A rider who opens
  // this has no way of knowing whether the device wants Bluetooth, WiFi or a
  // cable, and the answer changes what they go and do about it.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TILE_SYNC),
                 tr(STR_TILE_SYNC_OVER_BLE));

  const MapTransferReceiver::Status transfer = transfer_.status();
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing / 2;

  // One line that says what is going on, because "0 / 29" alone cannot tell a
  // sync that is waiting for a phone from one that has hung.
  char status[112];
  switch (phase_) {
    case Phase::Waiting:
      // "never turned up" and "was here and left" are different problems --
      // one is setup, the other is range -- so they get different words.
      snprintf(status, sizeof(status), "%s",
               hadPhone_ ? tr(STR_TILE_SYNC_PHONE_LEFT) : tr(STR_TILE_SYNC_WAITING_PHONE));
      break;
    case Phase::Running:
      formatSummary(status, sizeof(status));
      break;
    case Phase::Finished: {
      // The verdict a run ends on has to carry the same two numbers the running
      // line does, or a rider reads "finished" and cannot tell whether anything
      // arrived. What landed, what was not available, and how much moved.
      char moved[16];
      formatBytes(transfer.completedBytes, moved, sizeof(moved));
      char unavailable[32] = {};
      if (skipped_ > 0) {
        snprintf(unavailable, sizeof(unavailable), "   %lu %s", static_cast<unsigned long>(skipped_),
                 tr(STR_TILE_SYNC_ROW_MISSING));
      }
      // No verdict word here: the finished screen draws it on its own line, at
      // UI_12, right above this one. Printing it in both put "Fetch finished"
      // on the panel twice -- seen on the panel, not readable from the code.
      //
      // With nothing missing there is no ratio to state, and printing one
      // anyway reads as an error: a stale tile's replacement lands in
      // transfer.completed too, so a freshness-only visit showed "1 / 0"
      // (seen on the panel 2026-08-13). Just the bytes in that case -- the
      // freshness line below says what they were.
      if (rowCount_ == 0) {
        snprintf(status, sizeof(status), "%s", moved);
      } else {
        snprintf(status, sizeof(status), "%lu / %lu%s   %s", static_cast<unsigned long>(transfer.completed),
                 static_cast<unsigned long>(transferTotal()), unavailable, moved);
      }
      break;
    }
  }
  // A finished run leads with the answer: verdict big, the numbers under it, and
  // for squares that did not arrive the reason stated plainly rather than as a
  // footnote. No bar -- a bar that will not move again is furniture.
  //
  // **The grid stays.** It used to be dropped here on the reasoning that a
  // finished run is a result and not a live view, and that was wrong: on a run
  // the supplier could not fill, the squares still standing are exactly which
  // ground is still missing, which is the one thing the verdict line cannot say.
  // Seen on hardware 2026-08-13 -- "Fetch finished 19 / 25, 6 queued" over an
  // empty half-screen, with nothing to say which six.
  if (phase_ == Phase::Finished) {
    const int bigLine = renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, I18N.get(verdict_), true);
    y += bigLine;
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, status, true);
    y += lineHeight;
    // The freshness check's own line, under the fetch's numbers and before the
    // not-built explanation. This is where a rider finds out that the data the
    // screen just spent was a check rather than a download -- the state it
    // reports had no representation on the panel at all until now.
    char freshness[64];
    if (formatFreshness(freshness, sizeof(freshness))) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, freshness, true);
      y += lineHeight;
    }
    y += bigLine / 2;
    if (skipped_ > 0) {
      // Not "failed". The server does not have this square yet, the device has
      // it written down, and it will ask again -- two short lines because one
      // ran off the right edge at this size (measured on the panel).
      //
      // What it does not claim: that anybody was told to build it. Nothing
      // reports these gaps upstream today (../../docs/tile-index-spec.md -- the
      // map server is static files, no API).
      renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_TILE_SYNC_NOT_BUILT), true);
      y += bigLine;
      renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_TILE_SYNC_NOT_BUILT_2), true);
      y += bigLine;
    }
    // Below whatever the verdict needed, not at the running screen's fixed top.
    // Skipped when there is nothing to draw in the first place -- drawGrid()
    // would centre the verdict a second time. Stale tiles count: a visit with
    // nothing missing and something out of date is a grid of dots and no
    // squares, which is the case this screen could not show at all before.
    if (interestCount() > 0) drawGrid(y + metrics.verticalSpacing);
  } else {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, status, true);
    y += lineHeight;

    // Same line on a screen that is still running: a check can be in flight
    // while the fetch's own bar is up, and it must not look like part of it.
    char freshness[64];
    if (formatFreshness(freshness, sizeof(freshness))) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, freshness, true);
      y += lineHeight;
    }

    // While waiting, say what would make it start. A screen that only says
    // "waiting" leaves the rider with nothing to try.
    if (phase_ == Phase::Waiting) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, tr(STR_TILE_SYNC_WAITING_HINT), true);
    } else {
      // The one bar that is about the whole run, and **the indicator this screen
      // is read for**. The grid below is a picture of what is left, drawn over a
      // window that can leave outliers out (see the header comment); the bar is
      // the number, and it counts every tile of the run.
      //
      // It counts **arrivals**, not settled tiles. A tile the supplier does not
      // have settles the run too, but filling the bar with it tells the rider
      // the map is complete when nothing was transferred -- a run where the CDN
      // holds none of the area would end on a full bar. Measured on the panel:
      // 0 landed, 5 unavailable, bar at 100%. The unavailable count goes in the
      // line above instead, where it cannot be read as progress.
      GUI.drawProgressBar(
          renderer,
          Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
          transfer.completed, rowCount_ > 0 ? rowCount_ : 1);
    }

    drawGrid(0);
  }

  const auto labels = mappedInput.mapLabels(phase_ == Phase::Running ? tr(STR_CANCEL) : tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  drawnDone_ = transfer.completed;
  drawnSkipped_ = skipped_;
  lastActiveDrawMs_ = millis();
}

void TileSyncActivity::updateProgress() {
  const MapTransferReceiver::Status transfer = transfer_.status();
  const uint32_t done = transfer.completed;

  // A tile settling -- landed or skipped -- rubs a square out of the grid and
  // moves the bar, so that is a whole frame.
  if (done != drawnDone_ || skipped_ != drawnSkipped_) {
    lastSettleMs_ = millis();
    // A multi-tile sync with gaps between arrivals must not sleep mid-run:
    // this is the "still Running, more tiles to go" case, so enterPhase()
    // below is not reached and preventAutoSleep()'s clock has to be
    // re-stamped here instead.
    phaseEnteredMs_ = lastSettleMs_;
    if (phase_ == Phase::Running && done + skipped_ >= rowCount_) {
      enterPhase(Phase::Finished);
      verdict_ = StrId::STR_MAP_FETCH_DONE;
      LOG_INF(kLogTag, "done, %lu landed, %lu skipped", static_cast<unsigned long>(done),
              static_cast<unsigned long>(skipped_));
      // The queue's second half. Held until now on purpose: two conversations on
      // one command channel cross each other's replies (see trackPhone).
      askAboutFreshness();
    }
    renderScreen();
    return;
  }

  // Nothing has settled for a while and nothing is in flight: say so.
  //
  // The protocol has no "I am done" from the phone, and it cannot have a useful
  // one -- a phone that walked out of range would not send it either. So silence
  // is the only signal, and a screen that treats silence as "still working"
  // leaves a rider watching a grid that will never lose another square with no
  // way to tell a slow fetch from a finished one. Measured on hardware
  // 2026-08-11: 20 tiles sat there indefinitely after the phone had given up.
  if (phase_ == Phase::Running && !transfer.active && lastSettleMs_ != 0 &&
      millis() - lastSettleMs_ > kStallVerdictMs) {
    enterPhase(Phase::Finished);
    verdict_ = StrId::STR_TILE_SYNC_NO_ANSWER;
    LOG_INF(kLogTag, "no answer for %lu ms, %lu landed, %lu skipped", static_cast<unsigned long>(kStallVerdictMs),
            static_cast<unsigned long>(done), static_cast<unsigned long>(skipped_));
    askAboutFreshness();
    renderScreen();
    return;
  }

  // An active-but-silent transfer: the phone ANR'd mid-file with the GATT link
  // still held. `active` stays true forever in that case -- the receiver
  // reclaims a stalled transfer only on the *next* begin
  // (MapTransferReceiver.cpp:174-183) -- so the silence check above, gated on
  // `!transfer.active`, is suppressed and the bar would otherwise freeze with
  // no way out but Back. Same verdict, same kStallVerdictMs budget as that
  // check, and the same bytes-stopped-moving pattern MapActivity uses for
  // auto-sync (MapActivity::expireAutoSync(), MapActivity.cpp:870-894): the
  // timestamp resets on every byte of real movement, not on every repaint, so
  // a healthy slow transfer never trips it.
  if (phase_ == Phase::Running && transfer.active) {
    if (!transferWasActive_ || transfer.received != lastReceivedBytes_) {
      lastReceivedBytes_ = transfer.received;
      lastProgressMs_ = millis();
      // Same reasoning as the settle event above: bytes are moving, Running
      // stays Running, so this is the only place that tells preventAutoSleep()
      // the screen is still doing something.
      phaseEnteredMs_ = lastProgressMs_;
    } else if (millis() - lastProgressMs_ > kStallVerdictMs) {
      enterPhase(Phase::Finished);
      verdict_ = StrId::STR_TILE_SYNC_NO_ANSWER;
      LOG_INF(kLogTag, "stalled mid-transfer for %lu ms, %lu landed, %lu skipped",
              static_cast<unsigned long>(kStallVerdictMs), static_cast<unsigned long>(done),
              static_cast<unsigned long>(skipped_));
      askAboutFreshness();
      renderScreen();
      return;
    }
    transferWasActive_ = true;
  } else {
    transferWasActive_ = false;
  }

  if (phase_ != Phase::Running || !transfer.active || !transfer.activeTileValid) return;

  // Between arrivals the grid has nothing to say -- a square is there or it is
  // gone -- so the live part of this screen is the summary line: bytes moved,
  // rate, ETA. Rate-capped, because every repaint is a real waveform pass, and
  // windowed to that one line so the grid is not redrawn for a number.
  const uint32_t now = millis();
  if (now - lastActiveDrawMs_ < kSummaryRefreshMs) return;
  lastActiveDrawMs_ = now;

  char status[112];
  formatSummary(status, sizeof(status));

  int sx, sy, sw, sh;
  summaryRect(sx, sy, sw, sh);
  renderer.fillRect(sx, sy, sw, sh, false);
  renderer.drawText(UI_10_FONT_ID, sx, sy, status, true);
  if (!renderer.displayBufferWindow(sx, sy, sw, sh)) {
    LOG_ERR(kLogTag, "summary window rejected: %d,%d %dx%d", sx, sy, sw, sh);
  }
}
