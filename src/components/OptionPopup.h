#pragma once
#include <I18n.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    ownedValues.clear();
    leftAligned = false;
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    layoutValid = false;
    active = true;
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr;
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    ownedValues.clear();
    leftAligned = false;
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    layoutValid = false;
    active = true;
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings = options;
    ownedValues.clear();
    leftAligned = false;
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    layoutValid = false;
    active = true;
  }

  // Settings-style rows: label left, current value right in a highlight box on
  // the selected row. `values` is parallel to `options`; an empty entry means
  // the row is a plain action (Refresh) with no value to show. Rows are
  // left-aligned, because a column of labels that each start at a different x
  // is unreadable once half of them carry a value.
  void showWithValues(StrId titleId, const std::vector<std::string>& options, const std::vector<std::string>& values,
                      int currentIndex, std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings = options;
    ownedValues = values;
    ownedValues.resize(ownedStrings.size());
    leftAligned = true;
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    layoutValid = false;
    active = true;
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const int count = static_cast<int>(ownedStrings.size());
    int tx = 0;
    int ty = 0;
    if (input.wasScreenTouchDown(tx, ty)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      for (int i = 0; i < static_cast<int>(hitLayout.options.size()); i++) {
        if (contains(hitLayout.options[i], tx, ty)) {
          if (selectedIndex != i) {
            selectedIndex = i;
            requestUpdate();
          }
          break;
        }
      }
      return true;
    }
    if (input.wasScreenTapped(tx, ty)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      for (int i = 0; i < static_cast<int>(hitLayout.options.size()); i++) {
        if (contains(hitLayout.options[i], tx, ty)) {
          selectedIndex = i;
          active = false;
          if (onSelectCallback) onSelectCallback(selectedIndex);
          requestUpdate();
          return true;
        }
      }
      // Taps on the dialog chrome (title, padding) keep the popup open; taps outside dismiss it
      if (contains(hitLayout.dialog, tx, ty)) return true;
      active = false;
      requestUpdate();
      return true;
    }

    if (input.wasPressed(MappedInputManager::Button::NavPrevious)) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::NavNext)) {
      selectedIndex = (selectedIndex + 1) % count;
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::Confirm)) {
      active = false;
      if (onSelectCallback) onSelectCallback(selectedIndex);
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::Back)) {
      active = false;
      requestUpdate();
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    GUI.drawOptionPopup(renderer, title.c_str(), ownedStrings, selectedIndex, ownedValues, leftAligned);
  }

  bool isActive() const { return active; }

  // The screen rect the dialog covers, frame included. A caller that owns an
  // expensive background (MapActivity) snapshots this before the popup draws
  // and restores it on close -- one window refresh instead of a full redraw.
  Rect frameRect(const GfxRenderer& renderer) const {
    const int thickness = UITheme::getInstance().getMetrics().popupFrameThickness;
    const Rect& dialog = getLayout(renderer).dialog;
    return Rect{dialog.x - thickness, dialog.y - thickness, dialog.width + thickness * 2,
                dialog.height + thickness * 2};
  }

 private:
  struct Layout {
    Rect dialog{0, 0, 0, 0};
    std::vector<Rect> options;
  };

  // Text measurement is expensive and wasScreenTouchDown() is level-triggered, so the
  // layout is computed once per show() and cached rather than rebuilt every loop().
  const Layout& getLayout(const GfxRenderer& renderer) const {
    if (layoutValid) return layout;

    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    const int optionFontId = metrics.optionPopupUseSmallFont ? UI_10_FONT_ID : UI_12_FONT_ID;
    const EpdFontFamily::Style optionStyle =
        metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    const int itemSpacing = metrics.optionPopupItemSpacing;
    const int innerPadding = metrics.optionPopupInnerPadding;
    const int selectionHPadding = metrics.optionPopupSelectionHPadding;
    const int selectionVPadding = metrics.optionPopupSelectionVPadding;

    const int optionLineHeight = renderer.getLineHeight(optionFontId);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int rowHeight = optionLineHeight + selectionVPadding * 2;

    // Same width math as BaseTheme::drawOptionPopup() -- the two must agree or
    // the touch rects land off the drawn dialog.
    int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
    for (size_t i = 0; i < ownedStrings.size(); i++) {
      int width = renderer.getTextWidth(optionFontId, ownedStrings[i].c_str(), optionStyle);
      if (i < ownedValues.size() && !ownedValues[i].empty()) {
        width += selectionHPadding + renderer.getTextWidth(optionFontId, ownedValues[i].c_str(), optionStyle) +
                 BaseTheme::optionPopupValuePadding(selectionHPadding) * 2;
      }
      if (width > maxTextWidth) maxTextWidth = width;
    }

    const int optionCount = static_cast<int>(ownedStrings.size());
    const int listHeight = rowHeight * optionCount + itemSpacing * (optionCount - 1);
    const int dialogW = std::min((maxTextWidth + innerPadding * 2 + selectionHPadding * 2) * 12 / 10,
                                 pageWidth - metrics.optionPopupDialogSideMargin * 2);
    const int contentHeight = titleLineHeight + metrics.optionPopupTitleGap + listHeight;
    const int dialogH = contentHeight + innerPadding * 2;
    const int dialogX = (pageWidth - dialogW) / 2;
    const int dialogY = (pageHeight - dialogH) / 2;
    const int itemRectX = dialogX + innerPadding;
    const int itemRectW = dialogW - innerPadding * 2;
    const int firstItemY = dialogY + innerPadding + titleLineHeight + metrics.optionPopupTitleGap;

    layout.dialog = Rect{dialogX, dialogY, dialogW, dialogH};
    layout.options.clear();
    layout.options.reserve(optionCount);
    for (int i = 0; i < optionCount; i++) {
      layout.options.push_back(Rect{itemRectX, firstItemY + i * (rowHeight + itemSpacing), itemRectW, rowHeight});
    }
    layoutValid = true;
    return layout;
  }

  static bool contains(const Rect& rect, const int x, const int y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
  }

  bool active = false;
  std::string title;
  std::vector<std::string> ownedStrings;
  // Empty unless showWithValues() was used; sized to ownedStrings there.
  std::vector<std::string> ownedValues;
  bool leftAligned = false;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
  mutable Layout layout;
  mutable bool layoutValid = false;
};
