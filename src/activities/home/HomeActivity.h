#pragma once
#include <I18n.h>
#include <Icon.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  const HomeMenuItem initialMenuItem;

  // The menu, in the order it is drawn. Pins lives inside the map's own popup
  // and Wallet's activity is on the wallet-viewer branch, not on develop --
  // neither has a row here (../../../docs/home-screen.md). Trips is enabled: it
  // opens the trip picker (onTripsOpen() -> ActivityManager::goToRouteSelect()).
  struct Row {
    HomeMenuItem item;
    StrId label;
    const freeink::Icon* icon;
    // Optional `(text glyph)` aside after the label -- what the row runs on,
    // kept out of its name (BaseTheme::HomeRow, ../../docs/home-screen.md).
    // STR_NONE_OPT means no badge text.
    StrId badgeLabel = StrId::STR_NONE_OPT;
    const freeink::Icon* badgeIcon = nullptr;
  };
  // Five rider rows, plus one per bench row this build carries. Spelled as a
  // sum rather than as four literals because the static_assert in
  // HomeActivity.cpp checks it against the table, and two independent flags
  // would otherwise need four spellings of the same number.
  static constexpr int kRowCount = 5
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
                                   + 1  // the grayscale bench
#endif
#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH
                                   + 1  // the LoRa node list
#endif
      ;
  static const Row* rows();
  static int indexOf(HomeMenuItem item);

  // Wraps `from` by `step` (+1 or -1) within the row count.
  static int stepRow(int from, int step);

  void onSettingsOpen();
  void onFileTransferOpen();
  void onMapOpen();
  void onTripsOpen();
  void onTileSyncOpen();
#if defined(ENABLE_PREVIEW_BENCH) && ENABLE_PREVIEW_BENCH
  void onPreviewOpen();
#endif
#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH
  void onGroupOpen();
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
