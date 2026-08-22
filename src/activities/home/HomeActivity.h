#pragma once
#include <I18n.h>
#include <Icon.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  const HomeMenuItem initialMenuItem;

  // The menu, in the order it is drawn. `enabled == false` draws the row dimmed
  // and refuses selection -- Pins lives inside the map's own popup, and Wallet's
  // activity is on the wallet-viewer branch and not on develop yet
  // (../../../docs/home-screen.md). Trips is enabled: it opens the trip picker
  // (onTripsOpen() -> ActivityManager::goToRouteSelect()).
  struct Row {
    HomeMenuItem item;
    StrId label;
    const freeink::Icon* icon;
    bool enabled;
  };
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
  static constexpr int kRowCount = 8;  // + the grayscale bench row
#else
  static constexpr int kRowCount = 7;
#endif
  static const Row* rows();
  static int indexOf(HomeMenuItem item);

  // Skip disabled rows: `step` is +1 or -1 and the walk wraps.
  int nextSelectable(int from, int step) const;

  void onSettingsOpen();
  void onFileTransferOpen();
  void onMapOpen();
  void onTripsOpen();
  void onTileSyncOpen();
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
  void onPreviewOpen();
#endif
  void activate(int index);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
