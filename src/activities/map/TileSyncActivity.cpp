#include "TileSyncActivity.h"

#include <BlePositionServer.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

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
  // have to be the same list in the same order.
  MISSING_TILES.sortByFetchPriority();
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
      rows_[i].skipped = false;
    }
  }

  consoleState_.setMissingTilesSource(&g_missingTilesConsoleSource);
  consoleState_.setSkipObserver(this);
  // Quoted in NEED_TILES below and reported by `info`. A tile built to another
  // version transfers fine, passes CRC and is then refused on open, so the
  // supplier needs the number before it sends anything (MapTileReader.h).
  consoleState_.setTileFormatVersion(MapTileReader::kFormatVersion);
  consoleState_.clearSkips();
  skipped_ = 0;
  drawnDone_ = 0;
  drawnSkipped_ = 0;
  lastClearedTileSeq_ = transfer_.status().tileSeq;

  if (rowCount_ == 0) {
    // Worth a screen rather than a silent bounce back to the menu: the rider
    // picked this, and "nothing is missing" is good news.
    phase_ = Phase::Finished;
    verdict_ = StrId::STR_MAP_FETCH_NOTHING;
    LOG_INF(kLogTag, "nothing missing, nothing to ask for");
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
    askForTiles();
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
      rows_[i].skipped = true;
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
  if (row.skipped) return RowState::Skipped;
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
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + lineHeight * 2;

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
    case RowState::Active:
      snprintf(bytes, sizeof(bytes), "%lu / %lu B", static_cast<unsigned long>(received),
               static_cast<unsigned long>(total));
      right = bytes;
      break;
    case RowState::Done:
      right = tr(STR_TILE_SYNC_ROW_DONE);
      break;
    case RowState::Skipped:
      right = tr(STR_TILE_SYNC_ROW_SKIPPED);
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
  GUI.drawProgressBar(renderer, Rect{lx, barY, lw, kRowBarHeight}, barValue, barMax);
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
  char status[80];
  switch (phase_) {
    case Phase::Waiting:
      // "never turned up" and "was here and left" are different problems --
      // one is setup, the other is range -- so they get different words.
      snprintf(status, sizeof(status), "%s",
               hadPhone_ ? tr(STR_TILE_SYNC_PHONE_LEFT) : tr(STR_TILE_SYNC_WAITING_PHONE));
      break;
    case Phase::Running:
      snprintf(status, sizeof(status), "%lu / %lu   fail %lu", static_cast<unsigned long>(transfer.completed),
               static_cast<unsigned long>(rowCount_), static_cast<unsigned long>(skipped_));
      break;
    case Phase::Finished:
      snprintf(status, sizeof(status), "%s   %lu / %lu", I18N.get(verdict_),
               static_cast<unsigned long>(transfer.completed), static_cast<unsigned long>(rowCount_));
      break;
  }
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, status, true);
  y += lineHeight;

  // While waiting, say what would make it start. A screen that only says
  // "waiting" leaves the rider with nothing to try.
  if (phase_ == Phase::Waiting) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, tr(STR_TILE_SYNC_WAITING_HINT), true);
  }

  drawList();

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
    if (phase_ == Phase::Running && done + skipped_ >= rowCount_) {
      phase_ = Phase::Finished;
      verdict_ = StrId::STR_MAP_FETCH_DONE;
      LOG_INF(kLogTag, "done, %lu landed, %lu skipped", static_cast<unsigned long>(done),
              static_cast<unsigned long>(skipped_));
    }
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
