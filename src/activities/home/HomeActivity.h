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
  // Seven rider rows, plus one per bench row this build carries. Spelled as a
  // sum rather than as four literals because the static_assert in
  // HomeActivity.cpp checks it against the table, and two independent flags
  // would otherwise need four spellings of the same number.
  static constexpr int kRowCount = 7
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
                                   + 1  // the grayscale bench
#endif
#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH
                                   + 1  // the LoRa node list
#endif
      ;
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
#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH
  void onLoraPeersOpen();
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
