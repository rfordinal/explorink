#include "WalletViewActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "WALLETVIEW";

}  // namespace

WalletViewActivity::WalletViewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int itemIndex,
                                       const char* title, const uint16_t pageCount)
    : Activity("WalletView", renderer, mappedInput), itemIndex_(itemIndex), pageCount_(pageCount) {
  if (title != nullptr) {
    std::strncpy(title_, title, sizeof(title_) - 1);
    title_[sizeof(title_) - 1] = '\0';
  }
}

void WalletViewActivity::onEnter() {
  Activity::onEnter();
  pageIndex_ = 0;
  level_ = wallet::Level::Fit;
  col_ = 0;
  row_ = 0;
  // HALF on entry to an item: the list is on the panel and a differential
  // refresh cannot clear a frame it never saw (../../../docs/refresh-modes.md).
  showCurrent(HalDisplay::HALF_REFRESH);
}

void WalletViewActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    cycleLevel();
    return;
  }

  // One press, one stable screen. No held-button repeat and no smooth pan: a
  // FAST refresh is 500 ms of waveform (../../../docs/refresh-modes.md), so a
  // repeat would queue frames the rider never sees.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (stepTile(-1, 0)) showCurrent(HalDisplay::FAST_REFRESH);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (stepTile(+1, 0)) showCurrent(HalDisplay::FAST_REFRESH);
    return;
  }

  // The overloaded pair. A level with a single tile row -- always the case at
  // FIT -- has no row to step, so there the side buttons turn the page instead.
  const bool singleRow = currentGrid().rows <= 1;
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (singleRow) {
      if (stepPage(-1)) showCurrent(HalDisplay::HALF_REFRESH);
    } else if (stepTile(0, -1)) {
      showCurrent(HalDisplay::FAST_REFRESH);
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (singleRow) {
      if (stepPage(+1)) showCurrent(HalDisplay::HALF_REFRESH);
    } else if (stepTile(0, +1)) {
      showCurrent(HalDisplay::FAST_REFRESH);
    }
    return;
  }
}

void WalletViewActivity::cycleLevel() {
  level_ = wallet::nextLevel(level_);
  // Back to the top-left tile of the new level. Carrying a tile coordinate
  // across a level change would land the rider somewhere they did not point at:
  // the grids have different sizes, so there is no honest mapping.
  col_ = 0;
  row_ = 0;
  // HALF on a level change: the whole image changes, and the previous level is
  // a different picture, not a shifted one.
  showCurrent(HalDisplay::HALF_REFRESH);
}

bool WalletViewActivity::stepTile(const int dCol, const int dRow) {
  const wallet::LevelGrid& grid = currentGrid();
  const int cols = grid.cols > 0 ? grid.cols : 1;
  const int rows = grid.rows > 0 ? grid.rows : 1;

  int nextCol = static_cast<int>(col_) + dCol;
  int nextRow = static_cast<int>(row_) + dRow;
  // Clamp, never wrap: at the right edge of a document the next press does
  // nothing, which is what a piece of paper does.
  if (nextCol < 0) nextCol = 0;
  if (nextCol >= cols) nextCol = cols - 1;
  if (nextRow < 0) nextRow = 0;
  if (nextRow >= rows) nextRow = rows - 1;

  if (nextCol == static_cast<int>(col_) && nextRow == static_cast<int>(row_)) return false;
  col_ = static_cast<uint8_t>(nextCol);
  row_ = static_cast<uint8_t>(nextRow);
  return true;
}

bool WalletViewActivity::stepPage(const int delta) {
  const int pages = pageCount_ > 0 ? pageCount_ : 1;
  int next = pageIndex_ + delta;
  if (next < 0) next = 0;
  if (next >= pages) next = pages - 1;
  if (next == pageIndex_) return false;
  pageIndex_ = next;
  // A new page is a new document surface: start at its FIT view, not at the tile
  // coordinate that happened to be on screen.
  level_ = wallet::Level::Fit;
  col_ = 0;
  row_ = 0;
  return true;
}

void WalletViewActivity::showCurrent(const HalDisplay::RefreshMode mode) {
  wallet::PageLookup page;
  if (!wallet::Store::lookupPage(itemIndex_, pageIndex_, level_, col_, row_, page, error_)) {
    // Zero the grid so the arrows have nowhere to step: a failed lookup means we
    // do not know the shape of this level, and stepping against a stale grid
    // would ask for tiles that may not exist either.
    for (uint8_t i = 0; i < wallet::kLevelCount; ++i) grid_[i] = wallet::LevelGrid{};
    drawFailure(HalDisplay::HALF_REFRESH);
    return;
  }
  // The grid is refreshed on every screen, so the arrows always clamp against
  // the page actually on the panel.
  for (uint8_t i = 0; i < wallet::kLevelCount; ++i) grid_[i] = page.grid[i];
  if (page.pageCount > 0) pageCount_ = page.pageCount;
  if (page.title[0] != '\0') std::memcpy(title_, page.title, sizeof(title_));

  wallet::AssetHeader header;
  if (!wallet::Store::loadAssetIntoFrameBuffer(page.assetId, renderer, header, error_)) {
    drawFailure(HalDisplay::HALF_REFRESH);
    return;
  }

  error_ = wallet::Error::None;
  LOG_INF(kLogTag, "item %d page %d %s %u,%u v%lu", itemIndex_, pageIndex_, wallet::levelName(level_),
          static_cast<unsigned>(col_), static_cast<unsigned>(row_), static_cast<unsigned long>(header.version));
  // header.presentation says which way up the generator laid the document out.
  // Nothing acts on it: the device rotates nothing, the rider turns the device.
  renderer.displayBuffer(mode);
}

void WalletViewActivity::drawFailure(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  LOG_ERR(kLogTag, "item %d page %d %s %u,%u failed: error %u", itemIndex_, pageIndex_, wallet::levelName(level_),
          static_cast<unsigned>(col_), static_cast<unsigned>(row_), static_cast<unsigned>(error_));

  // A bad asset must not take the screen down. The rider gets a legible reason
  // and every button still works, so BACK and CONFIRM navigate away from it.
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 title_[0] != '\0' ? title_ : tr(STR_WALLET), nullptr);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, wallet::errorText(error_), true,
                    EpdFontFamily::BOLD);
  y += lineHeight * 2;

  char where[80];
  snprintf(where, sizeof(where), tr(STR_WALLET_PAGE_OF), pageIndex_ + 1, static_cast<int>(pageCount_ > 0 ? pageCount_ : 1));
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, where, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WALLET_LEVEL), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(mode);
}
