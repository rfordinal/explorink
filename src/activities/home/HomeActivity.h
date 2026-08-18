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
  // and refuses selection -- Trips has no activity at all and Pins lives inside
  // the map's own popup (../../../docs/home-screen.md).
  //
  // **Wallet is enabled on this branch**, because this is the branch its activity
  // lives on. On develop the same row is there with `enabled == false`, which is
  // why a rebase lands here every time: keep develop's table, flip that one flag,
  // and add the WALLET case to activate().
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
  void onTileSyncOpen();
  void onWalletOpen();
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
