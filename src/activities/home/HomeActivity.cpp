#include "HomeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/home_icons.h"
#include "fontIds.h"
#include "images/HomeHeader.h"

namespace {
// The brand block: mountain line art with the logo mark and the wordmark over
// it, one bitmap (src/images/home-header.svg, scripts/gen_home_header.py). The
// buffer is pre-rotated, so drawImage() takes the raw dimensions while the
// centring math uses the on-screen ones -- same constraint as the wordmark on
// the boot screen (../boot_sleep/BrandSplash.cpp, ../../../docs/sleep-screen.md).
constexpr int kHeaderGap = 8;
}  // namespace

// The menu, in the order it is drawn. Flash-resident: static constexpr, so the
// table costs no DRAM.
const HomeActivity::Row* HomeActivity::rows() {
  static constexpr Row kRows[] = {
      {HomeMenuItem::MAP, StrId::STR_HOME_EXPLORE, &icon_explore, true},
      {HomeMenuItem::TRIPS, StrId::STR_HOME_TRIPS, &icon_trips, false},
      {HomeMenuItem::PINS, StrId::STR_HOME_PINS, &icon_pins, false},
      {HomeMenuItem::WALLET, StrId::STR_HOME_WALLET, &icon_wallet, false},
      {HomeMenuItem::TILE_SYNC, StrId::STR_HOME_SYNC, &icon_sync, true},
      {HomeMenuItem::FILE_TRANSFER, StrId::STR_FILE_TRANSFER, &icon_transfer, true},
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
      // The grayscale bench is a build-flag lab instrument, not a rider's row
      // (platformio.ini).
      {HomeMenuItem::PREVIEW, StrId::STR_PREVIEW, &icon_settings, true},
#endif
      {HomeMenuItem::SETTINGS_MENU, StrId::STR_SETTINGS_TITLE, &icon_settings, true},
  };
  static_assert(sizeof(kRows) / sizeof(kRows[0]) == kRowCount, "kRowCount must match the table");
  return kRows;
}

int HomeActivity::indexOf(const HomeMenuItem item) {
  const Row* table = rows();
  for (int i = 0; i < kRowCount; ++i) {
    if (table[i].item == item) return i;
  }
  return 0;
}

int HomeActivity::nextSelectable(const int from, const int step) const {
  const Row* table = rows();
  int index = from;
  // The walk visits at most kRowCount rows, so a table with every row disabled
  // returns `from` instead of looping.
  for (int tries = 0; tries < kRowCount; ++tries) {
    index = (index + step + kRowCount) % kRowCount;
    if (table[index].enabled) return index;
  }
  return from;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : indexOf(initialMenuItem);
  if (!rows()[selectorIndex].enabled) selectorIndex = nextSelectable(selectorIndex, 1);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::activate(const int index) {
  if (index < 0 || index >= kRowCount || !rows()[index].enabled) return;
  switch (rows()[index].item) {
    case HomeMenuItem::FILE_TRANSFER:
      onFileTransferOpen();
      break;
    case HomeMenuItem::MAP:
      onMapOpen();
      break;
    case HomeMenuItem::TILE_SYNC:
      onTileSyncOpen();
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
}

void HomeActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int count = kRowCount;

  buttonNavigator.onNext([this] {
    selectorIndex = nextSelectable(selectorIndex, 1);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectorIndex = nextSelectable(selectorIndex, -1);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = nextSelectable(selectorIndex, 1);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = nextSelectable(selectorIndex, -1);
    requestUpdate();
    return;
  }

  // Rows are contiguous now (the brand block eats the space the old menu spent
  // on gaps), so the touch step is the row height itself.
  const int menuTop = metrics.homeTopPadding + kHeaderGap + HOMEHEADER_ON_SCREEN_HEIGHT + kHeaderGap;
  const int rowHeight = metrics.menuRowHeight;
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, rowHeight, count, 0, INT32_MAX, rowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    if (!rows()[menuRow].enabled) return;
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != menuRow) {
        selectorIndex = menuRow;
        requestUpdate();
      }
    } else {
      selectorIndex = menuRow;
      activate(menuRow);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selectorIndex);
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  const int headerTop = metrics.homeTopPadding + kHeaderGap;
  renderer.drawImage(HomeHeader, (pageWidth - HOMEHEADER_ON_SCREEN_WIDTH) / 2, headerTop, HOMEHEADER_WIDTH,
                     HOMEHEADER_HEIGHT);

  const int menuTop = headerTop + HOMEHEADER_ON_SCREEN_HEIGHT + kHeaderGap;
  const int rowHeight = metrics.menuRowHeight;

  // The theme draws rows, not menu items: 7 x 12 bytes of stack, well inside the
  // 256-byte local budget (../../../CLAUDE.md, The Resource Protocol).
  const Row* table = rows();
  BaseTheme::HomeRow drawRows[kRowCount];
  for (int i = 0; i < kRowCount; ++i) {
    drawRows[i] = BaseTheme::HomeRow{I18n::getInstance().get(table[i].label), table[i].icon, table[i].enabled};
  }
  GUI.drawHomeMenu(renderer, Rect{0, menuTop, pageWidth, pageHeight - menuTop - metrics.buttonHintsHeight}, drawRows,
                   kRowCount, selectorIndex, rowHeight);

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

#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
void HomeActivity::onPreviewOpen() { activityManager.goToPreview(); }
#endif
