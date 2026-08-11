#include "TileSyncActivity.h"

#include <BlePositionServer.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "LastHeldTiles.h"
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

// Row geometry. The bar sits under the coordinate rather than beside it: at 480
// px wide a bar long enough to read anything off leaves no room for a label.
constexpr int kRowBarHeight = 6;
constexpr int kRowGap = 6;

}  // namespace

// Stateless view onto MISSING_TILES for the `missing` command.
static MissingTilesConsoleSource g_missingTilesConsoleSource;

TileSyncActivity::TileSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("TileSync", renderer, mappedInput), transfer_(kTileRoot) {}

void TileSyncActivity::onEnter() {
  Activity::onEnter();

  freeink::BlePositionServer::getInstance().begin();
  // After begin(), so the characteristics exist before anything can be written
  // to them.
  transfer_.attach();

  // The list the phone is about to read and the count it is about to be told
  // have to be the same list in the same order, and where the rider was last
  // seen decides what goes out first. The same anchor
  // goes to the console source below, because `missing` re-sorts the store when
  // the phone starts paging -- two different orders would label the rows on this
  // screen for tiles the phone was never told about.
  const MissingTileAnchor anchor = missingTileAnchorFromLastFix();
  MISSING_TILES.sortByFetchPriority(anchor);
  const auto& hits = MISSING_TILES.hits();
  rowCount_ = static_cast<uint32_t>(hits.size());

  if (rowCount_ > 0) {
    rows_ = makeUniqueNoThrow<Row[]>(rowCount_);
    if (!rows_) {
      LOG_ERR(kLogTag, "OOM: %lu rows", static_cast<unsigned long>(rowCount_));
      // Without the snapshot there is no stable order to draw, and a fetch whose
      // screen cannot show what it is doing is worse than one that did not
      // start. The list is untouched, so the rider can try again.
      rowCount_ = 0;
      phase_ = Phase::Finished;
      verdict_ = StrId::STR_MAP_FETCH_NOTHING;
      renderScreen();
      return;
    }
    for (uint32_t i = 0; i < rowCount_; ++i) {
      rows_[i].tile = MapTileCoord{hits[i].z, hits[i].col, hits[i].row};
      rows_[i].unavailable = false;
    }
  }

  g_missingTilesConsoleSource.setAnchor(anchor);
  consoleState_.setMissingTilesSource(&g_missingTilesConsoleSource);
  consoleState_.setSkipObserver(this);
  // What `have` answers from: the tiles the map screen last drew, with the
  // content_id each was opened at. This screen has no viewport of its own, and
  // reading a content_id anywhere else on the device would mean opening tiles
  // for no other reason (LastHeldTiles.h).
  consoleState_.setHeldTiles(g_lastHeldTiles);
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
  consoleState_.clearSkips();
  skipped_ = 0;
  drawnDone_ = 0;
  drawnSkipped_ = 0;
  lastClearedTileSeq_ = transfer_.status().tileSeq;

  staleTiles_.clear();
  freshnessAsked_ = false;

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
      phase_ = Phase::Finished;
    } else {
      // A phone connecting synchronously with begin() above never happens on
      // real hardware (measured: ~1.8 s). trackPhone() takes it from here,
      // same as the missing-tiles path below -- see its comment.
      phase_ = Phase::Waiting;
    }
    renderScreen();
    return;
  }

  // Advertising now; nothing to ask until a phone subscribes to the command
  // channel. trackPhone() does the asking when one does -- see phoneListening().
  phase_ = Phase::Waiting;
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
  if (!g_lastHeldTiles.valid || g_lastHeldTiles.count == 0) {
    LOG_INF(kLogTag, "freshness: no tiles drawn yet, nothing to check");
    return;
  }
  char line[32];
  snprintf(line, sizeof(line), "CHECK_TILES %lu", static_cast<unsigned long>(g_lastHeldTiles.count));
  if (!freeink::BlePositionServer::getInstance().sendCommandReply(line)) {
    LOG_ERR(kLogTag, "CHECK_TILES not delivered");
    return;
  }
  freshnessAsked_ = true;
  LOG_INF(kLogTag, "freshness: asked about %lu tile(s)", static_cast<unsigned long>(g_lastHeldTiles.count));
}

void TileSyncActivity::onTileStale(uint8_t z, uint32_t col, uint32_t row) {
  if (staleTiles_.add(z, col, row)) {
    LOG_INF(kLogTag, "freshness: z%u %lu/%lu is out of date", static_cast<unsigned>(z), static_cast<unsigned long>(col),
            static_cast<unsigned long>(row));
  }
}

void TileSyncActivity::onCheckFinished(bool known, uint16_t staleCount) {
  if (!known) {
    // The phone could not read the index. Not the same as nothing being out of
    // date, and not reported as such.
    LOG_INF(kLogTag, "freshness: phone could not check (no index)");
    return;
  }
  LOG_INF(kLogTag, "freshness: %u tile(s) out of date, phone is pushing them", static_cast<unsigned>(staleCount));
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
  phase_ = Phase::Running;
  startedMs_ = millis();
  lastSettleMs_ = startedMs_;
  renderScreen();
}

void TileSyncActivity::trackPhone() {
  if (phase_ == Phase::Finished) return;
  const bool listening = phoneListening();
  if (listening == drawnPhoneListening_) return;
  drawnPhoneListening_ = listening;

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
      phase_ = Phase::Finished;
      renderScreen();
    }
    return;
  }

  // The phone walked away. Whatever was in flight died with the link, and the
  // screen says so rather than sitting on a bar that will never move again.
  LOG_INF(kLogTag, "phone gone, back to waiting");
  phase_ = Phase::Waiting;
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

bool TileSyncActivity::preventAutoSleep() { return freeink::BlePositionServer::getInstance().isRunning(); }

void TileSyncActivity::loop() {
  Activity::loop();

  // The phone's side of the conversation: `missing` to read the list, `skip` for
  // a tile it cannot supply. poll() returning true would mean a command changed
  // something on a map screen that is not up -- nothing to redraw here.
  ble_.poll();

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
    if (!freeink::BlePositionServer::getInstance().sendCommandReply("FETCH_CANCEL")) {
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

void TileSyncActivity::listRect(int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  // Below the header and the two text lines renderScreen() puts there (status,
  // and the hint while waiting). Reserved whether or not the hint is drawn, so
  // the list does not jump up and down as the phase changes.
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  // Below the header, the summary line, the overall bar and the percentage
  // GUI.drawProgressBar centres 15 px under it. Reserved whether or not the bar
  // is drawn, so the list does not jump as the phase changes.
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + lineHeight +
                  metrics.progressBarHeight + 15 + lineHeight;

  x = metrics.contentSidePadding;
  w = pageWidth - metrics.contentSidePadding * 2;
  y = top;
  h = pageHeight - top - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
}

int TileSyncActivity::visibleRowCount() const {
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + kRowBarHeight + kRowGap;
  const int fits = rowHeight > 0 ? lh / rowHeight : 0;
  return fits < 1 ? 1 : fits;
}

int TileSyncActivity::firstVisibleRow() const {
  const int fits = visibleRowCount();
  if (static_cast<int>(rowCount_) <= fits) return 0;

  // Follow the first row that has not settled -- the one the phone is on, or the
  // one it is about to reach. Keeps the moving bar on screen without scrolling
  // on every arrival.
  int firstUnsettled = static_cast<int>(rowCount_);
  for (uint32_t i = 0; i < rowCount_; ++i) {
    uint32_t received = 0;
    uint32_t total = 0;
    const RowState state = stateOf(static_cast<int>(i), received, total);
    if (state == RowState::Waiting || state == RowState::Active) {
      firstUnsettled = static_cast<int>(i);
      break;
    }
  }
  // Keep it a couple of rows down from the top, so what just finished stays
  // visible above it.
  int first = firstUnsettled - 2;
  const int maxFirst = static_cast<int>(rowCount_) - fits;
  if (first > maxFirst) first = maxFirst;
  return first < 0 ? 0 : first;
}

void TileSyncActivity::rowRect(int index, int& x, int& y, int& w, int& h) const {
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + kRowBarHeight + kRowGap;
  x = lx;
  w = lw;
  h = rowHeight;
  y = ly + (index - firstVisibleRow()) * rowHeight;
}

void TileSyncActivity::drawRow(int index, int y, int rowHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);

  // Opaque: a row repaints over its own last contents.
  renderer.fillRect(lx, y, lw, rowHeight, false);

  uint32_t received = 0;
  uint32_t total = 0;
  const RowState state = stateOf(index, received, total);
  const MapTileCoord& tile = rows_[index].tile;

  char label[48];
  snprintf(label, sizeof(label), "z%u %lu/%lu", static_cast<unsigned>(tile.z), static_cast<unsigned long>(tile.col),
           static_cast<unsigned long>(tile.row));
  renderer.drawText(UI_10_FONT_ID, lx, y, label, true);

  // Right-aligned status word, so the eye can scan one column for trouble.
  const char* right = nullptr;
  char bytes[32];
  switch (state) {
    case RowState::Active: {
      char got[16], want[16];
      formatBytes(received, got, sizeof(got));
      formatBytes(total, want, sizeof(want));
      snprintf(bytes, sizeof(bytes), "%s / %s", got, want);
      right = bytes;
      break;
    }
    case RowState::Done:
      right = tr(STR_TILE_SYNC_ROW_DONE);
      break;
    case RowState::Missing:
      right = tr(STR_TILE_SYNC_ROW_MISSING);
      break;
    case RowState::Waiting:
      right = tr(STR_TILE_SYNC_ROW_WAITING);
      break;
  }
  if (right != nullptr) {
    const int rightWidth = renderer.getTextWidth(UI_10_FONT_ID, right);
    renderer.drawText(UI_10_FONT_ID, lx + lw - rightWidth, y, right, true);
  }

  // The row's own bar. Full for a landed tile, the real fraction for the one on
  // the wire, empty otherwise -- a skipped row keeps an empty bar rather than a
  // full one, because nothing was transferred.
  const int barY = y + renderer.getLineHeight(UI_10_FONT_ID);
  size_t barValue = 0;
  size_t barMax = 1;
  if (state == RowState::Done) {
    barValue = 1;
  } else if (state == RowState::Active && total > 0) {
    barValue = received;
    barMax = total;
  }
  // Drawn here rather than through GUI.drawProgressBar, which always writes a
  // centred percentage 15 px below its bar. That is right for the one big bar it
  // was built for (FontDownloadActivity) and wrong for a list: ten 6-pixel row
  // bars produced ten labels, each landing on the next row's text, each erased
  // by the next row's fill -- leaving one stray number under the list that read
  // as overall progress and was actually the last row's state. A 6-pixel bar has
  // no room for a label anyway.
  renderer.drawRect(lx, barY, lw, kRowBarHeight);
  if (barValue > 0) {
    const int fill = static_cast<int>((lw - 4) * barValue / barMax);
    if (fill > 0) renderer.fillRect(lx + 2, barY + 2, fill, kRowBarHeight - 4);
  }
  (void)metrics;
}

void TileSyncActivity::drawList() {
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  renderer.fillRect(lx, ly, lw, lh, false);

  if (rowCount_ == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, ly + lh / 2, I18N.get(verdict_));
    return;
  }

  const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + kRowBarHeight + kRowGap;
  const int first = firstVisibleRow();
  const int fits = visibleRowCount();
  for (int i = first; i < static_cast<int>(rowCount_) && i < first + fits; ++i) {
    drawRow(i, ly + (i - first) * rowHeight, rowHeight);
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
             static_cast<unsigned long>(rowCount_), unavailable, moved);
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
             static_cast<unsigned long>(rowCount_), unavailable, moved, static_cast<unsigned long>(rateBps / 1000),
             static_cast<unsigned long>((rateBps % 1000) / 100), eta, tr(STR_TILE_SYNC_LEFT));
  } else {
    snprintf(out, outSize, "%lu / %lu%s   %s", static_cast<unsigned long>(transfer.completed),
             static_cast<unsigned long>(rowCount_), unavailable, moved);
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
      snprintf(status, sizeof(status), "%lu / %lu%s   %s", static_cast<unsigned long>(transfer.completed),
               static_cast<unsigned long>(rowCount_), unavailable, moved);
      break;
    }
  }
  // A finished run is a result, not a live view. The row list is what the rider
  // watches while it works and is irrelevant the moment it stops -- what they
  // came for is one answer, so on this screen the answer is the screen: verdict
  // big, the numbers under it, and for squares that did not arrive the reason
  // stated plainly rather than as a footnote. No list, no bar.
  if (phase_ == Phase::Finished) {
    const int bigLine = renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, I18N.get(verdict_), true);
    y += bigLine;
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, status, true);
    y += lineHeight + bigLine / 2;
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
    }
  } else {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, status, true);
    y += lineHeight;

    // While waiting, say what would make it start. A screen that only says
    // "waiting" leaves the rider with nothing to try.
    if (phase_ == Phase::Waiting) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, tr(STR_TILE_SYNC_WAITING_HINT), true);
    } else {
      // The one bar that is about the whole run, and the one place a percentage
      // belongs. GUI.drawProgressBar writes that percentage itself, centred
      // below the bar, which is exactly what is wanted here and exactly what
      // made it wrong for the rows.
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

    drawList();
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

  // A tile settling -- landed or skipped -- changes the summary line, one row's
  // state and possibly the window, so that is a whole frame.
  if (done != drawnDone_ || skipped_ != drawnSkipped_) {
    lastSettleMs_ = millis();
    if (phase_ == Phase::Running && done + skipped_ >= rowCount_) {
      phase_ = Phase::Finished;
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
  // leaves a rider watching rows marked "waiting" with no way to tell a slow
  // fetch from a finished one. Measured on hardware 2026-08-11: 20 rows sat
  // there indefinitely after the phone had already given up.
  if (phase_ == Phase::Running && !transfer.active && lastSettleMs_ != 0 &&
      millis() - lastSettleMs_ > kStallVerdictMs) {
    phase_ = Phase::Finished;
    verdict_ = StrId::STR_TILE_SYNC_NO_ANSWER;
    LOG_INF(kLogTag, "no answer for %lu ms, %lu landed, %lu skipped", static_cast<unsigned long>(kStallVerdictMs),
            static_cast<unsigned long>(done), static_cast<unsigned long>(skipped_));
    askAboutFreshness();
    renderScreen();
    return;
  }

  if (phase_ != Phase::Running || !transfer.active || !transfer.activeTileValid) return;

  // The bytes of the transfer in flight climb continuously and every repaint is
  // a real waveform pass, so the moving bar is rate-capped and repaints only its
  // own row, not the list.
  const uint32_t now = millis();
  if (now - lastActiveDrawMs_ < kActiveRowRefreshMs) return;
  lastActiveDrawMs_ = now;

  const int first = firstVisibleRow();
  const int fits = visibleRowCount();
  for (uint32_t i = 0; i < rowCount_; ++i) {
    if (rows_[i].tile.z != transfer.activeTile.z || rows_[i].tile.col != transfer.activeTile.col ||
        rows_[i].tile.row != transfer.activeTile.row) {
      continue;
    }
    const int index = static_cast<int>(i);
    if (index < first || index >= first + fits) return;  // scrolled out of sight
    int rx, ry, rw, rh;
    rowRect(index, rx, ry, rw, rh);
    drawRow(index, ry, rh);
    if (!renderer.displayBufferWindow(rx, ry, rw, rh)) {
      LOG_ERR(kLogTag, "row window rejected: %d,%d %dx%d", rx, ry, rw, rh);
    }
    return;
  }
}
