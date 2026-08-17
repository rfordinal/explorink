#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fontIds.h"

class GfxRenderer;
struct RecentBook;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct TabInfo {
  const char* label;
  bool selected;
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  int homeMenuTopOffset;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int optionPopupItemSpacing;
  int optionPopupInnerPadding;
  int optionPopupSelectionHPadding;
  int optionPopupSelectionVPadding;
  int optionPopupTitleGap;
  bool optionPopupUseSmallFont;
  bool optionPopupOptionFontBold;
  int optionPopupSelectionRadius;
  bool optionPopupSelectionLight;
  bool optionPopupDrawAllRows;
  int optionPopupDialogSideMargin;
  bool optionPopupTitleSeparator;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;
};

enum UIIcon {
  None = 0,
  Folder,
  Text,
  Image,
  Book,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Wifi,
  Hotspot,
  Bookmark,
  Bluetooth,
};

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 16,
                                 .optionPopupSelectionHPadding = 8,
                                 .optionPopupSelectionVPadding = 4,
                                 .optionPopupTitleGap = 10,
                                 .optionPopupUseSmallFont = true,
                                 .optionPopupOptionFontBold = true,
                                 .optionPopupSelectionRadius = 0,
                                 .optionPopupSelectionLight = false,
                                 .optionPopupDrawAllRows = false,
                                 .optionPopupDialogSideMargin = 20,
                                 .optionPopupTitleSeparator = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        bool showPercentage = true) const;  // Right aligned (UI headers)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  // fontId 0 means "this theme's own default" -- NOT a default argument
  // (BaseTheme, LyraTheme and RoundedRaffTheme each want a *different*
  // fallback font, but GUI is `const BaseTheme&` (UITheme.h), a fixed
  // static type; a default argument resolves against the static type at the
  // call site, not the override that actually runs, so three different
  // per-class defaults would silently collapse to BaseTheme's one --
  // confirmed the hard way on hardware 2026-08-08: every other screen's
  // hint text grew because it silently got BaseTheme's default instead of
  // its own theme's). Each override checks for 0 and substitutes its own
  // font in the body instead. 0 is safe as a sentinel -- fontIds.h already
  // reserves it as the "not found" value, never a real font ID.
  //
  // btn3FontId/btn4FontId: 0 means "same as fontId" -- btn1/btn2 (Back/
  // Confirm, always words) and btn3/btn4 (Left/Right, words in Follow but
  // arrow glyphs in MapActivity's Observe mode) don't always want the same
  // size. Split only for the pair that actually needs it, so every caller
  // that wants all four uniform (everyone except Observe mode) still passes
  // nothing past fontId.
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4, int fontId = 0, int btn3FontId = 0, int btn4FontId = 0) const;
  // fontId defaults to SMALL_FONT_ID -- every existing caller keeps the same
  // glyph it always had. A caller with its own larger/bolder use for this
  // box (MapActivity's pan hints, which need real arrow glyphs no shared
  // hint font carries at readable size) passes a different one explicitly.
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                                   int fontId = SMALL_FONT_ID) const;
  // The four-box hint band along the bottom. Empty on a touch panel, where
  // drawButtonHints() draws nothing. Same purpose as sideButtonHintsRect(): a
  // caller placing something near an edge has to know what is already there.
  Rect buttonHintsRect(const GfxRenderer& renderer) const;
  // What those boxes cover, for a caller that repaints part of the panel and has
  // to refresh exactly the region they changed. Empty on a touch panel, where
  // drawSideButtonHints() draws nothing.
  //
  // This exists because guessing it is expensive: the map screen refreshed the
  // whole panel instead, and a full-window refresh allocates a buffer inside the
  // display driver -- on a map screen with 38 KB free that allocation failed and
  // aborted the device (measured 2026-08-17, crash_report.txt:
  // Ssd1677Driver::displayWindow -> operator new -> bad_alloc -> terminate).
  Rect sideButtonHintsRect(const GfxRenderer& renderer) const;
  virtual int getListRowStep(bool hasSubtitle) const;
  virtual int getListPageItems(int contentHeight, bool hasSubtitle) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        const std::function<bool(int index)>& rowDimmed = nullptr) const;
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                                 int& index) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  // What the option popup draws. `values` is parallel to `options` and may be
  // null or shorter -- a row with no value entry draws label-only.
  // `leftAlign` puts every label at the same left edge instead of centring
  // each one; a value column only reads as a column when the labels line up.
  // `compact` shrinks the paddings (optionPopupSpacing). `scrollTop` is the
  // first row on screen: the dialog shows a fixed window of rows and the list
  // scrolls through it (optionPopupGeometry).
  struct OptionPopupSpec {
    const char* title = nullptr;
    const std::vector<std::string>* options = nullptr;
    const std::vector<std::string>* values = nullptr;
    int selectedIndex = 0;
    int scrollTop = 0;
    bool leftAlign = false;
    bool compact = false;
    // Floors, both optional (0 = no floor). For a popup that *replaces* another
    // one over the same background: without them a submenu shrinks to its own
    // content and lands as a differently sized box in the middle of the previous
    // one, which reads as a different kind of thing rather than the next step of
    // the same one. The ceilings still win -- neither can push the dialog past
    // kOptionPopupMaxVisibleRows or the panel's side margins.
    //
    // A matching size also keeps the caller's saved backdrop valid, which is what
    // makes closing the second popup as cheap as closing the first
    // (MapActivity::captureMenuBackdrop()).
    int minDialogWidth = 0;
    int minVisibleRows = 0;
  };
  // Where the dialog and its visible rows land. One function, two readers: the
  // drawing pass and OptionPopup's hit test, which must agree or a tap misses
  // the row it landed on.
  struct OptionPopupGeometry {
    Rect dialog{0, 0, 0, 0};  // frame not included
    int rowX = 0;
    int rowWidth = 0;
    int firstRowY = 0;
    int rowHeight = 0;
    int rowStep = 0;  // rowHeight plus the gap between rows
    int visibleRows = 0;
    int titleLineHeight = 0;
  };
  virtual OptionPopupGeometry optionPopupGeometry(const GfxRenderer& renderer, const OptionPopupSpec& spec) const;
  virtual void drawOptionPopup(const GfxRenderer& renderer, const OptionPopupSpec& spec) const;

  // Hard ceiling on the dialog: rows past it scroll rather than making it
  // taller. Both bounds exist because the dialog's size is a RAM cost for
  // MapActivity, which snapshots the pixels underneath it, and because a
  // dialog that covers the screen is not a dialog.
  static constexpr int kOptionPopupMaxVisibleRows = 6;
  static constexpr int kOptionPopupMaxHeightPercent = 50;

  // Every length the option popup's geometry needs, in one place.
  struct OptionPopupSpacing {
    int itemSpacing;
    int innerPadding;
    int selectionHPadding;
    int selectionVPadding;
    int titleGap;
    int valuePadding;  // inside the selected row's value box, both sides
    int widthPercent;  // slack on the measured text width
  };
  // compact halves the vertical air and drops the width slack. The slack is
  // there for centred label-only rows, where the text is measured but the
  // layout is not; a settings-style row's width is computed exactly (label +
  // gap + boxed value), so padding it out only wastes screen and backdrop.
  static OptionPopupSpacing optionPopupSpacing(const ThemeMetrics& metrics, bool compact);
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                     const bool fillMargin = true, const bool isPageBookmarked = false,
                     const bool pageCountEstimated = false) const;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual bool showsFileIcons() const { return false; }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};
