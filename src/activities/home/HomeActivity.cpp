#include "HomeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
// File transfer, Map, Sync tiles, Wallet, Preview, Settings
int HomeActivity::getMenuItemCount() const { return 6; }
#else
// File transfer, Map, Sync tiles, Wallet, Settings -- the grayscale bench is a
// build-flag lab instrument and stays out of a rider's menu (platformio.ini).
int HomeActivity::getMenuItemCount() const { return 5; }
#endif

void HomeActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : menuItemToIndex(initialMenuItem);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    switch (indexToMenuItem(selectorIndex)) {
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::MAP:
        onMapOpen();
        break;

      case HomeMenuItem::TILE_SYNC:
        onTileSyncOpen();
        break;
      case HomeMenuItem::WALLET:
        onWalletOpen();
        break;
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
      case HomeMenuItem::PREVIEW:
        onPreviewOpen();
        break;
#endif
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeMenuTopOffset;
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing, menuCount,
                                              0, INT32_MAX, metrics.menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != menuRow) {
        selectorIndex = menuRow;
        requestUpdate();
      }
    } else {
      selectorIndex = menuRow;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  // STR_MAP/Bookmark are placeholders -- Map has no dedicated icon asset yet
  // (see docs/firmware-implementation-plan.md Phase 2), swap for a real one
  // once the icon pipeline work happens.
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
  const std::vector<const char*> menuItems = {tr(STR_FILE_TRANSFER), tr(STR_MAP),     tr(STR_TILE_SYNC),
                                              tr(STR_WALLET),        tr(STR_PREVIEW), tr(STR_SETTINGS_TITLE)};
  const std::vector<UIIcon> menuIcons = {Transfer, Bookmark, Bluetooth, Wallet, Image, Settings};
#else
  const std::vector<const char*> menuItems = {tr(STR_FILE_TRANSFER), tr(STR_MAP), tr(STR_TILE_SYNC), tr(STR_WALLET),
                                              tr(STR_SETTINGS_TITLE)};
  // Bluetooth, not Wifi: tile sync goes over BLE and nothing else, and a WiFi
  // glyph sends a rider to the wrong settings page. Wallet is Lucide `wallet`
  // through gen_icons.py (components/icons/wallet.h) -- Map is still on the
  // Bookmark placeholder.
  const std::vector<UIIcon> menuIcons = {Transfer, Bookmark, Bluetooth, Wallet, Settings};
#endif

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), selectorIndex,
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

// The picker first, then the map. It falls through to the map on its own when
// the card has no routes (ActivityManager::goToRouteSelect).
void HomeActivity::onMapOpen() { activityManager.goToRouteSelect(); }

void HomeActivity::onTileSyncOpen() { activityManager.goToTileSync(); }

void HomeActivity::onWalletOpen() { activityManager.goToWallet(); }

#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
void HomeActivity::onPreviewOpen() { activityManager.goToPreview(); }
#endif
