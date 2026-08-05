#pragma once
#include <functional>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  const HomeMenuItem initialMenuItem;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item) {
    int i = 0;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::MAP) return i;
    ++i;
    if (item == HomeMenuItem::TILE_SYNC) return i;
    ++i;
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
    if (item == HomeMenuItem::PREVIEW) return i;
    ++i;
#endif
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i++) return HomeMenuItem::MAP;
    if (idx == i++) return HomeMenuItem::TILE_SYNC;
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
    if (idx == i++) return HomeMenuItem::PREVIEW;
#endif
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSettingsOpen();
  void onFileTransferOpen();
  void onMapOpen();
  void onTileSyncOpen();
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
  void onPreviewOpen();
#endif

  int getMenuItemCount() const;

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
