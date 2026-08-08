#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/ExplorinkWordmark.h"
#include "images/Logo120.h"

namespace {
// Wordmark's authored (on-screen) size -- the raw buffer is pre-rotated 90
// degrees (EXPLORINKWORDMARK_WIDTH/HEIGHT), same convention as Logo120.
constexpr int kWordmarkOnScreenWidth = 170;
constexpr int kWordmarkOnScreenHeight = 40;
constexpr int kWordmarkToSubtitleGap = 8;
}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int wordmarkTop = pageHeight / 2 + 70;

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawImage(ExplorinkWordmark, (pageWidth - kWordmarkOnScreenWidth) / 2, wordmarkTop, EXPLORINKWORDMARK_WIDTH,
                     EXPLORINKWORDMARK_HEIGHT);
  renderer.drawCenteredText(SMALL_FONT_ID, wordmarkTop + kWordmarkOnScreenHeight + kWordmarkToSubtitleGap,
                            tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, TRAILINK_VERSION);
  renderer.displayBuffer();
}
