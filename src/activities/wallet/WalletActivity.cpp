#include "WalletActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "WalletViewActivity.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "WALLET";
// Title on one line, page count under it -- a document name and its length do
// not fit side by side at 480 px in gloves (RouteSelectActivity.cpp).
constexpr int kRowGap = 8;

}  // namespace

WalletActivity::WalletActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Wallet", renderer, mappedInput) {}

void WalletActivity::onEnter() {
  Activity::onEnter();

  entries_ = makeUniqueNoThrow<wallet::ItemEntry[]>(kMaxItems);
  if (!entries_) {
    LOG_ERR(kLogTag, "OOM: %u item rows", static_cast<unsigned>(kMaxItems));
    stored_ = 0;
    seen_ = 0;
    error_ = wallet::Error::ManifestUnreadable;
  } else if (!wallet::Store::listItems(entries_.get(), kMaxItems, stored_, seen_, error_)) {
    LOG_INF(kLogTag, "no list: error %u", static_cast<unsigned>(error_));
  }
  selected_ = 0;
  // HALF on entry: the panel is carrying whatever screen came before, and a
  // differential refresh cannot clear what it never saw
  // (../../../docs/refresh-modes.md).
  renderScreen(HalDisplay::HALF_REFRESH);
}

void WalletActivity::onExit() {
  Activity::onExit();
  entries_.reset();
  stored_ = 0;
}

void WalletActivity::loop() {
  Activity::loop();

  const int rows = rowCount();
  bool moved = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Hard stops, not a wrap -- the same as every other list on this device.
    if (selected_ > 0) {
      --selected_;
      moved = true;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (selected_ + 1 < rows) {
      ++selected_;
      moved = true;
    }
  }
  if (moved) renderScreen(HalDisplay::FAST_REFRESH);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::WALLET);
  }
}

void WalletActivity::openSelected() {
  if (stored_ == 0) return;
  if (selected_ < 0 || selected_ >= static_cast<int>(stored_)) return;

  const int itemIndex = selected_;
  const uint16_t pageCount = entries_[itemIndex].pageCount;
  LOG_INF(kLogTag, "open item %d (%u pages)", itemIndex, static_cast<unsigned>(pageCount));
  // Pushed, not replaced: BACK out of the viewer returns to this list without
  // reading the manifest again. The result handler repaints, because this screen
  // does not override render() and the automatic post-pop update would leave the
  // document on the panel.
  startActivityForResult(
      std::make_unique<WalletViewActivity>(renderer, mappedInput, itemIndex, entries_[itemIndex].title, pageCount),
      [this](const ActivityResult&) { renderScreen(HalDisplay::HALF_REFRESH); });
}

void WalletActivity::renderScreen(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WALLET),
                 tr(STR_WALLET_HINT));

  const int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing / 2;
  char status[112];
  if (stored_ == 0) {
    snprintf(status, sizeof(status), "%s", wallet::errorText(error_));
  } else if (seen_ > stored_) {
    // A truncated list must say so out loud. Silently hiding a document the
    // rider synced is the kind of quiet lie this codebase refuses elsewhere too
    // (RouteSelectActivity.cpp).
    snprintf(status, sizeof(status), tr(STR_WALLET_LIST_TRUNCATED), static_cast<int>(stored_), static_cast<int>(seen_));
  } else {
    snprintf(status, sizeof(status), tr(STR_WALLET_COUNT), static_cast<int>(stored_));
  }
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, status, true);

  drawList();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(mode);
}

void WalletActivity::listRect(int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + lineHeight;

  x = metrics.contentSidePadding;
  w = pageWidth - metrics.contentSidePadding * 2;
  y = top;
  h = pageHeight - top - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
}

int WalletActivity::visibleRowCount() const {
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) * 2 + kRowGap;
  const int fits = rowHeight > 0 ? lh / rowHeight : 0;
  return fits < 1 ? 1 : fits;
}

int WalletActivity::firstVisibleRow() const {
  const int fits = visibleRowCount();
  const int rows = rowCount();
  if (rows <= fits) return 0;
  int first = selected_ - fits / 2;
  if (first < 0) first = 0;
  if (first > rows - fits) first = rows - fits;
  return first;
}

void WalletActivity::drawList() {
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  renderer.fillRect(lx, ly, lw, lh, false);

  const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) * 2 + kRowGap;
  const int first = firstVisibleRow();
  const int fits = visibleRowCount();
  const int rows = rowCount();
  for (int i = first; i < rows && i < first + fits; ++i) {
    drawRow(i, ly + (i - first) * rowHeight, rowHeight);
  }
}

void WalletActivity::drawRow(const int index, const int y, const int rowHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  const bool highlighted = index == selected_;
  if (highlighted) {
    renderer.fillRect(lx, y - 2, lw, rowHeight - kRowGap + 4, true);
  }
  const bool ink = !highlighted;

  const wallet::ItemEntry& entry = entries_[index];
  char detail[64];
  if (entry.pageCount == 1) {
    snprintf(detail, sizeof(detail), "%s", tr(STR_WALLET_PAGE_ONE));
  } else {
    snprintf(detail, sizeof(detail), tr(STR_WALLET_PAGES), static_cast<int>(entry.pageCount));
  }

  const char* title = entry.title[0] != '\0' ? entry.title : tr(STR_WALLET_UNTITLED);
  renderer.drawText(UI_10_FONT_ID, lx + metrics.contentSidePadding / 2, y, title, ink, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, lx + metrics.contentSidePadding / 2, y + lineHeight, detail, ink);
}
