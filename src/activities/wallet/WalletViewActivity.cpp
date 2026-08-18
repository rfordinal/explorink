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
  winNativeX_ = 0;
  winNativeY_ = 0;
  // showCurrent() positions the view once it knows which source this level has.
  needsReposition_ = true;
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

  // One press, one stable screen. No held-button repeat: a FAST refresh is 500 ms
  // of waveform and a windowed page-image read is another 283 ms (measured,
  // ../../../docs/wallet-viewer.md), so a repeat would queue frames the rider
  // never sees. Design B changed the *size* of a step, not this.
  //
  // The steps are named for what the rider feels, not for a native axis. See the
  // axis note above stepView(): `windowStepX` is a step DOWN the document and
  // `windowStepY` a step ACROSS it, because the payload is stored rotated.
  const int stepDown =
      usingPageImage_
          ? static_cast<int>(page_.spec().windowStepX > 0 ? page_.spec().windowStepX : renderer.getDisplayWidth())
          : 1;
  const int stepAcross =
      usingPageImage_
          ? static_cast<int>(page_.spec().windowStepY > 0 ? page_.spec().windowStepY : renderer.getDisplayHeight())
          : 1;

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (stepView(0, +stepAcross)) showCurrent(HalDisplay::FAST_REFRESH);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (stepView(0, -stepAcross)) showCurrent(HalDisplay::FAST_REFRESH);
    return;
  }

  // The overloaded pair. Where the view cannot move vertically at all -- always
  // the case at FIT -- the side buttons turn the page instead.
  const bool idle = verticalAxisIdle();
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (idle) {
      if (stepPage(-1)) showCurrent(HalDisplay::HALF_REFRESH);
    } else if (stepView(-stepDown, 0)) {
      showCurrent(HalDisplay::FAST_REFRESH);
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (idle) {
      if (stepPage(+1)) showCurrent(HalDisplay::HALF_REFRESH);
    } else if (stepView(+stepDown, 0)) {
      showCurrent(HalDisplay::FAST_REFRESH);
    }
    return;
  }
}

uint32_t WalletViewActivity::downLimit() const {
  // Down the document is native x, whose travel is a byte count times eight, so
  // every reachable origin on this axis is 8-aligned by construction.
  return wallet::maxWindowX(page_.spec(), renderer.getDisplayWidthBytes());
}

uint32_t WalletViewActivity::acrossLimit() const {
  const uint32_t span = page_.spec().nativeHeight;
  const uint32_t window = renderer.getDisplayHeight();
  return span > window ? span - window : 0;
}

bool WalletViewActivity::verticalAxisIdle() const {
  // A page image with no room to travel down has nothing to pan; a one-row grid
  // has no row to step. Same question, two sources. At FIT the page image is
  // exactly one panel, so both limits are zero and the side buttons turn pages --
  // the same behaviour the tile path had.
  if (usingPageImage_) return downLimit() == 0;
  return currentGrid().rows <= 1;
}

void WalletViewActivity::jumpToLevelDefault() {
  if (usingPageImage_) {
    // Design B's focal *point*, not a focal tile: the window opens where the
    // generator says the content is, clamped into the image.
    const wallet::PageImageSpec& page = page_.spec();
    winNativeX_ = wallet::clampWindowOrigin(static_cast<int32_t>(page.focalX), downLimit(), 0);
    winNativeY_ = wallet::clampWindowOrigin(static_cast<int32_t>(page.focalY), acrossLimit(), 0);
    // focalX is 8-aligned by the generator and downLimit() is a byte count times
    // eight, so the clamp cannot produce anything else. Refused rather than
    // rounded: rounding would hide a generator bug behind a half-pixel shift.
    if ((winNativeX_ % 8) != 0) {
      LOG_ERR(kLogTag, "focalX %u is not 8-aligned; opening at 0", static_cast<unsigned>(page.focalX));
      winNativeX_ = 0;
    }
    return;
  }
  const wallet::LevelGrid& grid = currentGrid();
  const uint8_t cols = grid.cols > 0 ? grid.cols : 1;
  const uint8_t rows = grid.rows > 0 ? grid.rows : 1;
  col_ = grid.defaultCol < cols ? grid.defaultCol : 0;
  row_ = grid.defaultRow < rows ? grid.defaultRow : 0;
}

void WalletViewActivity::cycleLevel() {
  level_ = wallet::nextLevel(level_);
  // Not the top-left of the new level: it opens at that level's own focal point
  // (page image) or focal tile (tile grid). Carrying the old position across
  // would land the rider somewhere they never pointed at -- the levels are
  // different sizes, so there is no honest mapping between their coordinates.
  needsReposition_ = true;
  // HALF on a level change: the whole image changes, and the previous level is
  // a different picture, not a shifted one.
  showCurrent(HalDisplay::HALF_REFRESH);
}

// ## The axis note
//
// A page image is stored panel-native, which means it is the document turned a
// quarter. `GfxRenderer::rotateCoordinates()` maps logical portrait (x, y) to
// panel (y, panelHeight - 1 - x) (GfxRenderer.cpp:216-223), so in the stored
// image:
//
//   native x  = logical y   -> runs DOWN the document. 0 is the top.
//               This is also the byte-offset axis (a row read starts at x/8), so
//               this is the axis that must stay 8-aligned.
//   native y  = logicalWidth - 1 - logical x  -> runs ACROSS the document,
//               INVERTED. The largest native y is the document's LEFT edge.
//
// Verified by rendering the corners through the host preview
// (test/wallet_preview): native (0, max) is the top-left of the page, title and
// left margin. Wiring these the wrong way round rotates the pan by ninety
// degrees, and wiring the sign the wrong way makes LEFT go right -- both were
// caught here before this code ever reached the panel.
//
// So: `down` is a delta on native x, `across` is a delta on native y with LEFT
// positive.
bool WalletViewActivity::stepView(const int down, const int across) {
  if (usingPageImage_) {
    const uint32_t nextX = wallet::clampWindowOrigin(static_cast<int32_t>(winNativeX_) + down, downLimit(), 0);
    const uint32_t nextY = wallet::clampWindowOrigin(static_cast<int32_t>(winNativeY_) + across, acrossLimit(), 0);
    if (nextX == winNativeX_ && nextY == winNativeY_) return false;
    winNativeX_ = nextX;
    winNativeY_ = nextY;
    return true;
  }

  // Tile grid: one column, one row. `across` positive is LEFT, and a tile column
  // grows to the right, so the sign flips here.
  const wallet::LevelGrid& grid = currentGrid();
  const int cols = grid.cols > 0 ? grid.cols : 1;
  const int rows = grid.rows > 0 ? grid.rows : 1;
  int nextCol = static_cast<int>(col_) + (across < 0 ? 1 : (across > 0 ? -1 : 0));
  int nextRow = static_cast<int>(row_) + (down > 0 ? 1 : (down < 0 ? -1 : 0));
  // Clamp, never wrap: at the edge of a document the next press does nothing,
  // which is what a piece of paper does.
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
  // A new page is a new document surface: start at its FIT view, positioned at
  // whatever that level's focal point turns out to be.
  level_ = wallet::Level::Fit;
  needsReposition_ = true;
  return true;
}

void WalletViewActivity::showCurrent(const HalDisplay::RefreshMode mode) {
  wallet::PageLookup page;
  if (!wallet::Store::lookupPage(itemIndex_, pageIndex_, level_, col_, row_, page, error_)) {
    // Zero the grid so the arrows have nowhere to step: a failed lookup means we
    // do not know the shape of this level, and stepping against a stale grid
    // would ask for tiles that may not exist either.
    for (uint8_t i = 0; i < wallet::kLevelCount; ++i) grid_[i] = wallet::LevelGrid{};
    usingPageImage_ = false;
    page_.close();
    drawFailure(HalDisplay::HALF_REFRESH);
    return;
  }
  // The grid is refreshed on every screen, so the arrows always clamp against
  // the page actually on the panel.
  for (uint8_t i = 0; i < wallet::kLevelCount; ++i) grid_[i] = page.grid[i];
  if (page.pageCount > 0) pageCount_ = page.pageCount;
  if (page.title[0] != '\0') std::memcpy(title_, page.title, sizeof(title_));

  // A whole-page image wins when the level offers both: it is the only one of the
  // two that can pan by a fraction of the view, which is the whole point of
  // design B. The tile path below is not dead code -- the generator still emits
  // both and a card in the field may hold either.
  if (page.pageImage.present) {
    if (!page_.open(page.pageImage, renderer, error_)) {
      usingPageImage_ = false;
      drawFailure(HalDisplay::HALF_REFRESH);
      return;
    }
    usingPageImage_ = true;
    if (needsReposition_) {
      jumpToLevelDefault();
      needsReposition_ = false;
    } else {
      // Re-clamp: nothing guarantees the next page's image is the same shape.
      winNativeX_ = wallet::clampWindowOrigin(static_cast<int32_t>(winNativeX_), downLimit(), 0);
      winNativeY_ = wallet::clampWindowOrigin(static_cast<int32_t>(winNativeY_), acrossLimit(), 0);
    }
    bool drawn = false;
    {
      // The window is read straight into the framebuffer, which the render task
      // owns too, and the refresh reads the same bytes -- so both are inside one
      // lock. 480 strided reads take 283 ms on the X4 (measured), which makes this
      // the longest lock the wallet takes.
      RenderLock lock;
      drawn = page_.readWindow(winNativeX_, winNativeY_, renderer, error_);
      if (drawn) renderer.displayBuffer(mode);
    }
    if (!drawn) {
      drawFailure(HalDisplay::HALF_REFRESH);
      return;
    }
    error_ = wallet::Error::None;
    LOG_INF(kLogTag, "item %d page %d %s window %lu,%lu of %ux%u", itemIndex_, pageIndex_, wallet::levelName(level_),
            static_cast<unsigned long>(winNativeX_), static_cast<unsigned long>(winNativeY_),
            static_cast<unsigned>(page_.spec().nativeWidth), static_cast<unsigned>(page_.spec().nativeHeight));
    return;
  }

  // Tile grid.
  usingPageImage_ = false;
  page_.close();
  if (needsReposition_) {
    jumpToLevelDefault();
    needsReposition_ = false;
    // The focal tile is not the tile the lookup above asked for, so the assetId in
    // hand is the wrong one. Re-run for the tile actually wanted.
    if (!wallet::Store::lookupPage(itemIndex_, pageIndex_, level_, col_, row_, page, error_)) {
      drawFailure(HalDisplay::HALF_REFRESH);
      return;
    }
  }
  if (!page.assetFound) {
    error_ = wallet::Error::NotInManifest;
    drawFailure(HalDisplay::HALF_REFRESH);
    return;
  }

  wallet::AssetHeader header;
  bool drawn = false;
  {
    RenderLock lock;
    drawn = wallet::Store::loadAssetIntoFrameBuffer(page.assetId, renderer, header, error_);
    if (drawn) renderer.displayBuffer(mode);
  }
  if (!drawn) {
    drawFailure(HalDisplay::HALF_REFRESH);
    return;
  }

  error_ = wallet::Error::None;
  LOG_INF(kLogTag, "item %d page %d %s tile %u,%u v%lu", itemIndex_, pageIndex_, wallet::levelName(level_),
          static_cast<unsigned>(col_), static_cast<unsigned>(row_), static_cast<unsigned long>(header.version));
  // header.presentation says which way up the generator laid the document out.
  // The generator writes 1 (upright for a device held in portrait) and rotates
  // the bits at build time to make it so, which is the orientation the rest of
  // the UI already uses -- confirmed on real output (docs/wallet-viewer.md,
  // "What the pictures showed"). Nothing acts on the field: the device rotates
  // nothing either way.
}

void WalletViewActivity::drawFailure(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  LOG_ERR(kLogTag, "item %d page %d %s %u,%u failed: error %u", itemIndex_, pageIndex_, wallet::levelName(level_),
          static_cast<unsigned>(col_), static_cast<unsigned>(row_), static_cast<unsigned>(error_));

  // A bad asset must not take the screen down. The rider gets a legible reason
  // and every button still works, so BACK and CONFIRM navigate away from it.
  RenderLock lock;
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
