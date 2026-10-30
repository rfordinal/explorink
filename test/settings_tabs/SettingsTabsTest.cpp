// Does the Settings tab bar still fit on a 480 px panel?
//
// **The fifth tab is why this test exists.** The Group tab was added
// 2026-10-30 and `LyraTheme::drawTabBar()` lays the tabs out left to right in
// one row with no wrap, no scroll and no ellipsis: a row that overflows is not
// clipped into something readable, the last tab simply runs off the glass and
// the rider cannot see which one is selected.
//
// Nothing enforced that before, because four tabs fit with room to spare and
// nobody was going to add a fifth. Now somebody has.
//
// The arithmetic is the theme's own, copied rather than called: drawTabBar()
// takes a GfxRenderer and a Rect, and standing one of those up on the host
// means the display HAL. What it needs from the renderer is one number per
// label -- `getTextWidth(UI_10_FONT_ID, label, REGULAR)` -- and that is
// `EpdFont(&ubuntu_10_regular).getTextDimensions()`, the same font data the
// device registers for UI_10 (src/main.cpp, `ui10RegularFont`). So the widths
// here are the device's widths; only the loop around them is restated.
//
// If this fails, the fix is a shorter label, not a bigger number: the panel is
// 480 px and that is not negotiable.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "EpdFont.h"
#include "PreviewFont.h"

namespace {

// LyraTheme.cpp and LyraTheme.h, read 2026-10-30. Lyra is the default theme.
constexpr int kContentSidePadding = 20;  // LyraMetrics::values.contentSidePadding
constexpr int kTabSpacing = 8;           // LyraMetrics::values.tabSpacing
constexpr int kHPaddingInSelection = 8;  // LyraTheme.cpp, file-scope constant

// The panel. Portrait, and the tab bar spans it (SettingsActivity::render()).
constexpr int kScreenWidth = 480;

int textWidth(const std::string& label) {
  const void* face = PreviewFont::pick(/*sizePx=*/10, /*bold=*/false);
  int w = 0, h = 0;
  EXPECT_TRUE(PreviewFont::measure(face, label.c_str(), w, h));
  return w;
}

// Exactly drawTabBar()'s cursor walk: each tab is its text plus the selection
// padding on both sides, and tabs are separated by tabSpacing.
int tabBarWidth(const std::vector<std::string>& labels) {
  int x = kContentSidePadding;
  for (const auto& label : labels) {
    x += textWidth(label) + kTabSpacing + 2 * kHPaddingInSelection;
  }
  // The last tab takes no trailing spacing on the panel -- the cursor is just
  // left past it -- so the drawn right edge is one spacing back.
  return x - kTabSpacing;
}

}  // namespace

TEST(SettingsTabs, FiveTabsFitTheScreen) {
  // english.yaml: STR_CAT_DISPLAY, STR_CAT_MAP, STR_CAT_GROUP,
  // STR_CAT_CONTROLS, STR_CAT_SYSTEM.
  const std::vector<std::string> tabs = {"Display", "Map", "Group", "Controls", "System"};
  const int width = tabBarWidth(tabs);
  EXPECT_LE(width, kScreenWidth) << "the tab bar is " << width << " px wide on a " << kScreenWidth
                                 << " px panel -- shorten a label, do not widen the panel";
}

TEST(SettingsTabs, TheGroupTabIsWhatMadeItTight) {
  // Four tabs were never in danger. Kept so a future failure says which half
  // of the problem moved: a longer label somewhere else, or one tab too many.
  const std::vector<std::string> four = {"Display", "Map", "Controls", "System"};
  const std::vector<std::string> five = {"Display", "Map", "Group", "Controls", "System"};
  EXPECT_LE(tabBarWidth(four), kScreenWidth);
  EXPECT_GT(tabBarWidth(five), tabBarWidth(four));
}
