#pragma once

#include <Icon.h>

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
  // The two fixed dialog boxes, as a percent of the panel the HAL reports.
  //
  // Board-relative on purpose: the numbers are per device without being written
  // per device -- a 480x800 X4 and a wider panel both get a box of the same
  // proportion, and nothing here is an X4 pixel count. A caller asks for a size
  // class (OptionPopupSize) and gets this box exactly, neither shrunk to its own
  // content nor grown by it.
  //
  // Menu is the browsing box: the map menu, the pin and POI lists. Confirm is the
  // yes/no box, deliberately smaller so a confirmation reads as a different kind
  // of dialog. Confirm must stay inside Menu on both axes -- both are centred, and
  // MapActivity's saved backdrop is taken at the Menu box, so a Confirm that stuck
  // out would leave the previous dialog's frame on the panel after the restore.
  int optionPopupMenuWidthPercent;
  int optionPopupMenuHeightPercent;
  int optionPopupConfirmWidthPercent;
  int optionPopupConfirmHeightPercent;

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
                                 .optionPopupMenuWidthPercent = 70,
                                 .optionPopupMenuHeightPercent = 40,
                                 .optionPopupConfirmWidthPercent = 60,
                                 .optionPopupConfirmHeightPercent = 30,
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
  //
  // topDisabled/bottomDisabled: that one box still exists but its action
  // can't fire right now (a caller at a hard stop, e.g. MapActivity's zoom
  // ladder ends) -- distinct from a null/empty label, which means the box
  // doesn't apply to this screen at all. Base draws it exactly like an empty
  // label (nothing); LyraTheme overrides this to shrink it to a stub instead,
  // matching its own drawButtonHints()'s SMALL-sized button for the same
  // "still there, tucked toward the edge" case.
  //
  // bold: a `bool`, not `EpdFontFamily::Style`, so this header does not need
  // that type -- GfxRenderer is only forward-declared here. Every existing
  // caller (the pan arrows included -- their glyph data is identical in both
  // cuts, borrowed once from OpenDyslexic-Bold, see
  // docs/map-observation-mode.md) keeps `false`; only a caller whose glyph
  // actually has a distinct bold cut and needs it (MapActivity's zoom "+"/"--")
  // passes `true`.
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                                   int fontId = SMALL_FONT_ID, bool topDisabled = false, bool bottomDisabled = false,
                                   bool bold = false) const;
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
  // Where a tap counts as pressing a hardware button, in portrait logical
  // coordinates (the hint boxes are always painted in portrait, whatever the
  // reader is rotated to). `index` is HalGPIO's front-button order, 0 = BTN_BACK
  // to 3 = BTN_RIGHT; sideHintBox() takes 0 = top (BTN_UP), 1 = bottom (BTN_DOWN).
  // False when that box is not drawn.
  //
  // The rect returned is the rect painted. A hit area that is not the box the
  // user can see is a lie they aim at, so both come from the same numbers --
  // which is also why every theme that moves its boxes must override these.
  // portraitWidth/portraitHeight are the panel in portrait logical coordinates,
  // passed in rather than looked up: the drawing takes them from the renderer, so
  // the hit test must come from the same place or a tap can miss a box that is
  // plainly on screen.
  // The padlock that stands in for the boxes when touch is locked. Every theme's
  // drawButtonHints() calls this on the path where it draws no boxes, so the
  // indicator reaches every screen that has hints without any of them knowing.
  // A no-op unless the panel is locked.
  void drawTouchLockIndicator(GfxRenderer& renderer) const;
  // Just the box, in this theme's own style. The caller owns the policy, the
  // orientation and the geometry, so an override changes the look and nothing
  // else -- the padlock has to match the boxes it stands in for, and every theme
  // draws those differently (Lyra and RoundedRaff round their corners).
  virtual void drawTouchLockBox(GfxRenderer& renderer, Rect box) const;
  virtual bool frontHintBox(int index, int portraitWidth, int portraitHeight, Rect& out) const;
  virtual bool sideHintBox(int index, int portraitWidth, int portraitHeight, Rect& out) const;

 protected:
  // The hint boxes are drawn per screen paint with the labels that screen wants,
  // and a box with an empty label is not a button on that screen. The input
  // layer sees no labels, so the last painted set is remembered here: without it
  // a tap on blank glass where a box used to be would still fire its button.
  // Every theme's drawButtonHints()/drawSideButtonHints() must call these.
  void rememberFrontLabels(const char* btn1, const char* btn2, const char* btn3, const char* btn4) const;
  void rememberSideLabels(const char* topBtn, const char* bottomBtn) const;
  bool frontBoxActive(int index) const;
  bool sideBoxActive(int index) const;

 private:
  mutable bool frontLabelDrawn[4] = {false, false, false, false};
  mutable bool sideLabelDrawn[2] = {false, false};

 public:
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
  // The Home screen's row list: icon, label, chevron. One row is selected and
  // draws inverted on a filled block; a row with `enabled == false` keeps its
  // shape and loses half its ink, so a rider sees the feature exists and is not
  // there yet. Rows are icon assets, not UIIcon: Home's glyphs are Lucide-baked
  // freeink::Icons (src/components/icons/home_icons.h) and the UIIcon table is a
  // fixed set of the reader's own bitmaps. See ../../../docs/home-screen.md.
  struct HomeRow {
    const char* label;
    const freeink::Icon* icon;
    bool enabled;
  };
  virtual void drawHomeMenu(const GfxRenderer& renderer, Rect rect, const HomeRow* rows, int rowCount,
                            int selectedIndex, int rowHeight) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  // Which of the fixed boxes this popup opens at, or Auto for the historical
  // behaviour (the dialog measures its own content and lands wherever that puts
  // it). A fixed box is the same rect every time, which is the whole point:
  // MapActivity snapshots the map pixels under the dialog and refreshes exactly
  // that window on close, so a rect that moves with the content costs a full
  // re-render instead (MapActivity::captureMenuBackdrop()). The size numbers are
  // ThemeMetrics::optionPopupMenu*/optionPopupConfirm*.
  //
  // Auto stays the default: every screen outside the map still sizes to content.
  enum class OptionPopupSize : uint8_t { Auto, Menu, Confirm };

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
    // Auto measures the content and lands where that puts it. A size class takes
    // the whole rect from the theme instead -- width, height and position -- and
    // only the row count still comes from measurement, because the title may wrap
    // to two lines and eat a row.
    OptionPopupSize size = OptionPopupSize::Auto;
    // Rows the rider cannot select, parallel to `options`: non-zero means the
    // row is disabled. Drawn dimmed by dimDisabledRow() and skipped by
    // OptionPopup's selection walk. nullptr or short means every row is live.
    const std::vector<uint8_t>* disabled = nullptr;
    // One line of text between the title and the rows, not selectable and not
    // counted as a row. For a fact about the thing the popup is *about* rather
    // than an action on it -- a POI's reliability condition, say. nullptr means
    // no note and no space reserved for one.
    //
    // Added 2026-08-21 because the alternative was worse: Nearby's POI detail
    // screen first carried its two facts as inert rows, and walking a cursor
    // through text that does nothing reads as a hack (maintainer, on hardware).
    const char* note = nullptr;
    // One icon per row, parallel to `options`. A null entry (or a short
    // vector) draws that row with no glyph, but the column is still reserved
    // -- so a picker where every row carries the same kind of icon (a pin
    // type, a POI category) stays aligned even for the one row that does not
    // (Nearby's `Hide all`). nullptr means no icon column at all, the
    // pre-existing layout.
    //
    // Exists so a picker can show the *same* glyph the thing it is naming
    // draws elsewhere -- PinIcons.h's pin balloon glyph, poi_icons.h's
    // kPoiIconByCategory -- rather than a second, different symbol for the
    // same type.
    const std::vector<const freeink::Icon*>* icons = nullptr;
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
    // How many lines the title wrapped to. 1 for every title that fits on one
    // line, which is most of them -- see wrapOptionPopupTitle() in the .cpp.
    int titleLineCount = 1;
    // Where the note goes, and how tall it is. Both 0 when the spec carries no
    // note, and then nothing above reserved space for one.
    int noteY = 0;
    int noteLineHeight = 0;
  };
  virtual OptionPopupGeometry optionPopupGeometry(const GfxRenderer& renderer, const OptionPopupSpec& spec) const;
  virtual void drawOptionPopup(const GfxRenderer& renderer, const OptionPopupSpec& spec) const;

  // A row the rider cannot select: keep the glyph and the label, lose every
  // second pixel row to white. Shared by Home's list and by OptionPopup so the
  // two cannot drift into two different ideas of "disabled" -- the panel has no
  // grey in BW mode (../../../docs/eink-grayscale.md), and a line screen halves
  // the ink without the speckle a checkerboard leaves at this text size.
  static void dimDisabledRow(const GfxRenderer& renderer, int x, int y, int width, int height);

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

  // The box a size class asks for, already centred on the panel. Width and height
  // are 0 for Auto, which is how every caller tells the two modes apart. Clamped
  // to the panel's side margins, so a metric set too wide cannot push a dialog
  // off the screen.
  static Rect optionPopupFixedBox(const GfxRenderer& renderer, const ThemeMetrics& metrics, OptionPopupSize size);
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
