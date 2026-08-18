#include "WalletActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "WalletCodeActivity.h"
#include "WalletCryptoDevice.h"
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

WalletActivity::WalletActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int openItem,
                               const int openCode)
    : Activity("Wallet", renderer, mappedInput), openItem_(openItem), openCode_(openCode) {}

void WalletActivity::onEnter() {
  Activity::onEnter();

  entries_ = makeUniqueNoThrow<wallet::ItemEntry[]>(kMaxItems);
  if (!entries_) {
    LOG_ERR(kLogTag, "OOM: %u item rows", static_cast<unsigned>(kMaxItems));
    stored_ = 0;
    seen_ = 0;
    error_ = wallet::Error::ManifestUnreadable;
  } else if (!wallet::Store::listItems(entries_.get(), kMaxItems, renderer, stored_, seen_, declared_, error_)) {
    LOG_INF(kLogTag, "no list: error %u", static_cast<unsigned>(error_));
  }
  selected_ = 0;
  LOG_INF(kLogTag, "heap with the browse list up (%s manifest): %lu free, %lu largest block",
          wallet::treeIsEncrypted() ? "encrypted" : "cleartext", static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  // The manifest is read; now, and only now, is it known whether a
  // CMD:GOTO_WALLET index exists. Pushing a child activity from onEnter() is the
  // supported pattern -- pendingActivity is already emptied by then
  // (ActivityManager.cpp:153-159).
  if (applyGotoTarget()) return;
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

  // The wallet key dies if the rider walks away with a wallet screen up. Touched by
  // any button, so the timeout measures idleness and not how long the screen has
  // been open (docs/wallet-crypto.md, "The key's lifetime").
  auto& session = wallet::Session::instance();
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) session.touch();
  if (session.expireIfIdle()) {
    onGoHome(HomeMenuItem::WALLET);
    return;
  }

  const int rows = rowCount();
  bool moved = false;
  // The selection moves on the side pair only. LEFT and RIGHT used to be a second
  // way to move it; they walk the selected document's machine-readable codes now
  // (P2, docs/wallet-viewer.md, "The code walk"). The side pair is where this
  // device puts list movement anyway.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    // Hard stops, not a wrap -- the same as every other list on this device.
    if (selected_ > 0) {
      --selected_;
      moved = true;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
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
  // RIGHT opens the first code, LEFT the last -- the other way round the same
  // ring the code screen cycles. One code: either button opens it. No codes: both
  // do nothing, which is why the row says how many there are.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    openCodeStep(+1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    openCodeStep(-1);
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

void WalletActivity::openCodeStep(const int delta) {
  if (stored_ == 0) return;
  if (selected_ < 0 || selected_ >= static_cast<int>(stored_)) return;

  const uint16_t codes = entries_[selected_].codeCount;
  if (codes == 0) {
    // Nothing to open, and nothing to say: a document with no code is the normal
    // case, and spending a 1.7 s HALF refresh on a message about a button that did
    // nothing would be worse than the silence. The row already states the count.
    LOG_INF(kLogTag, "item %d has no codes", selected_);
    return;
  }
  // RIGHT steps onto the ring from before its start, LEFT steps back off its
  // beginning -- one rule, wallet::walkCodeIndex(), shared with the code screen.
  const int from = delta > 0 ? -1 : 0;
  openCodeAt(wallet::walkCodeIndex(from, delta, codes), codes);
}

bool WalletActivity::openCodeAt(const int codeIndex, const uint16_t codeCount) {
  if (codeIndex < 0 || codeIndex >= static_cast<int>(codeCount)) return false;
  LOG_INF(kLogTag, "open item %d code %d of %u", selected_, codeIndex, static_cast<unsigned>(codeCount));
  startActivityForResult(
      std::make_unique<WalletCodeActivity>(renderer, mappedInput, selected_, entries_[selected_].title, codeIndex),
      [this](const ActivityResult&) { renderScreen(HalDisplay::HALF_REFRESH); });
  return true;
}

bool WalletActivity::applyGotoTarget() {
  const int wantItem = openItem_;
  const int wantCode = openCode_;
  // Consumed: coming back out of the viewer must land on the list, not open the
  // same document again for ever.
  openItem_ = -1;
  openCode_ = -1;
  if (wantItem < 0) return false;

  if (wantItem >= static_cast<int>(stored_)) {
    // Refused, not clamped. A host script that asks for document 9 of a two-document
    // wallet must not be handed document 0 and believe it got what it asked for.
    LOG_ERR(kLogTag, "GOTO_WALLET: no item %d (wallet has %u)", wantItem, static_cast<unsigned>(stored_));
    snprintf(gotoError_, sizeof(gotoError_), tr(STR_WALLET_NO_ITEM), wantItem);
    return false;
  }
  selected_ = wantItem;

  if (wantCode < 0) {
    // Document only. openSelected() paints the viewer over this screen.
    LOG_INF(kLogTag, "GOTO_WALLET: item %d", wantItem);
    openSelected();
    return true;
  }
  const uint16_t codes = entries_[wantItem].codeCount;
  if (!openCodeAt(wantCode, codes)) {
    LOG_ERR(kLogTag, "GOTO_WALLET: item %d has %u codes, none at %d", wantItem, static_cast<unsigned>(codes), wantCode);
    snprintf(gotoError_, sizeof(gotoError_), tr(STR_WALLET_NO_CODE), wantItem, wantCode);
    return false;
  }
  LOG_INF(kLogTag, "GOTO_WALLET: item %d code %d", wantItem, wantCode);
  return true;
}

void WalletActivity::renderScreen(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WALLET),
                 tr(STR_WALLET_HINT));

  const int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing / 2;
  char status[112];
  if (gotoError_[0] != '\0') {
    // A CMD:GOTO_WALLET index that is not in this wallet. On the panel as well as
    // in the log, because a host-driven loop verifies by screenshot.
    snprintf(status, sizeof(status), "%s", gotoError_);
  } else if (error_ == wallet::Error::PanelMismatch) {
    // Name both panels. "Wrong screen" without saying which is a dead end for
    // whoever has to fix the card.
    const wallet::PanelGeometry live = wallet::livePanel(renderer);
    snprintf(status, sizeof(status), tr(STR_WALLET_PANEL_MISMATCH), declared_.name[0] != '\0' ? declared_.name : "?",
             static_cast<int>(declared_.width), static_cast<int>(declared_.height), static_cast<int>(live.width),
             static_cast<int>(live.height));
  } else if (stored_ == 0) {
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

  // LEFT/RIGHT are the code walk now, so the hints say so -- and they say WHICH
  // WAY. Two boxes both reading "Code" cost a real misdiagnosis on the panel: the
  // maintainer could not tell which was which, pressed one, landed on the last
  // code and concluded the code itself was drawn wrong. A label that forces a
  // guess is a defect (../../../docs/wallet-viewer.md, "The hints are
  // directional"). mapLabels() takes (previous, next) and swaps them itself when
  // the nav direction is swapped.
  //
  // Moving the selection is the side pair, which has no hint box on the X4 -- the
  // device-wide convention for a list, and the reason the front pair could be given
  // to codes at all.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_WALLET_CODE_PREV), tr(STR_WALLET_CODE_NEXT));
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
  char detail[96];
  if (entry.pageCount == 1) {
    snprintf(detail, sizeof(detail), "%s", tr(STR_WALLET_PAGE_ONE));
  } else {
    snprintf(detail, sizeof(detail), tr(STR_WALLET_PAGES), static_cast<int>(entry.pageCount));
  }
  // A code count on the row is what makes LEFT/RIGHT discoverable: without it a
  // rider cannot tell a document with no code from a button that does nothing.
  if (entry.codeCount > 0) {
    const size_t at = std::strlen(detail);
    char codes[32];
    if (entry.codeCount == 1) {
      snprintf(codes, sizeof(codes), "%s", tr(STR_WALLET_CODE_ONE));
    } else {
      snprintf(codes, sizeof(codes), tr(STR_WALLET_CODES), static_cast<int>(entry.codeCount));
    }
    snprintf(detail + at, sizeof(detail) - at, ", %s", codes);
  }

  const char* title = entry.title[0] != '\0' ? entry.title : tr(STR_WALLET_UNTITLED);
  renderer.drawText(UI_10_FONT_ID, lx + metrics.contentSidePadding / 2, y, title, ink, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, lx + metrics.contentSidePadding / 2, y + lineHeight, detail, ink);
}
