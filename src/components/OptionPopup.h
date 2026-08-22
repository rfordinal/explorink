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
    compact = false;
    selectedIndex = currentIndex;
    scrollTop = 0;
    onSelectCallback = std::move(onSelect);
    layoutValid = false;
    active = true;
    note.clear();
    resetRowChrome();
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
    compact = false;
    selectedIndex = currentIndex;
    scrollTop = 0;
    onSelectCallback = std::move(onSelect);
    layoutValid = false;
    active = true;
    note.clear();
    resetRowChrome();
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings = options;
    ownedValues.clear();
    leftAligned = false;
    compact = false;
    selectedIndex = currentIndex;
    scrollTop = 0;
    onSelectCallback = std::move(onSelect);
    layoutValid = false;
    active = true;
    note.clear();
    resetRowChrome();
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
    compact = true;
    selectedIndex = currentIndex;
    scrollTop = 0;
    onSelectCallback = std::move(onSelect);
    layoutValid = false;
    active = true;
    note.clear();
    resetRowChrome();
  }

  // Same rows, with a title that is not a translated constant -- a POI's own
  // name, or a category label chosen at runtime. Everything else is identical to
  // the StrId form above; the title is the only difference, so this delegates
  // rather than repeating the field assignments.
  void showWithValues(const char* titleStr, const std::vector<std::string>& options,
                      const std::vector<std::string>& values, int currentIndex, std::function<void(int)> onSelect) {
    showWithValues(StrId::STR_MAP, options, values, currentIndex, std::move(onSelect));
    title = titleStr;
  }

  // One line of text between the title and the rows, not selectable
  // (BaseTheme::OptionPopupSpec::note). Set after a show*() call, which clears
  // it -- so a popup carries a note only when it asks for one. Empty string
  // means none.
  void setNote(const char* text) {
    note = text != nullptr ? text : "";
    layoutValid = false;
  }

  // Per-row actions, opt-in.
  //
  // The front Left and Right buttons are **not free** in a popup: they are
  // aliases for scrolling (NavNext is side Down *or* front Right, NavPrevious is
  // side Up *or* front Left -- MappedInputManager.cpp). So a popup that wants row
  // actions does not gain buttons, it takes that pair away from scrolling:
  // selection then moves on the screen's up/down pair and the screen's left/right
  // pair acts on the selected row. Same split the map screen already teaches --
  // one pair moves through something, the other does something to it.
  //
  // Everything here goes through the Screen* buttons and mapDirectionalLabels(),
  // never raw Left/Right with mapLabels(): the physical left and right swap in
  // INVERTED and LANDSCAPE_CCW, and a hint on the wrong button means the rider
  // presses Delete because the label said Replace.
  //
  // Opt-in per popup: every existing popup keeps front-button scrolling, so
  // nothing else changes. show() clears the actions, so a popup can never inherit
  // the previous one's -- set them right after show().
  //
  // A row action does **not** close the popup and does not repaint. And it must
  // not call show() on this popup either, directly or indirectly: the callback
  // runs from inside handleInput(), so a show() there reassigns the very
  // std::function that is executing. Record what should happen and act on it from
  // the caller's own loop (MapActivity::PinPopup).
  struct RowActions {
    std::function<void(int)> onLeft;
    std::function<void(int)> onRight;
    // Hint text, already translated. Nullptr or empty draws no label.
    const char* leftLabel = nullptr;
    const char* rightLabel = nullptr;
    // What CONFIRM says. Nullptr keeps tr(STR_SELECT).
    const char* confirmLabel = nullptr;
  };

  void setRowActions(RowActions actions) {
    rowActions = std::move(actions);
    hasRowActions = true;
  }

  // The side buttons carry no slot in the four-box hint bar, so a popup that has
  // taken the front pair has to say so somewhere or scrolling looks broken. Both
  // glyphs are drawn rotated by the theme (BaseTheme::drawSideButtonHints), and
  // the boxes sit where the physical side buttons are -- which is the screen's
  // up/down pair on a portrait-rendered screen, the only way the map runs.
  //
  // Drawn outside the dialog, so an owner that snapshots the pixels under the
  // popup (MapActivity's menu backdrop) must cover these too when it restores.
  void setSideHints(const char* top, const char* bottom, int fontId = SMALL_FONT_ID) {
    sideHintTop = top;
    sideHintBottom = bottom;
    sideHintFontId = fontId;
  }

  bool hasSideHints() const { return sideHintTop != nullptr || sideHintBottom != nullptr; }

  // Keep this popup at least as big as the one it replaced: same width, same row
  // count. A submenu that shrinks to its own content reads as a different kind of
  // dialog instead of the next step of the same one, and a caller holding a
  // backdrop of the previous dialog can reuse it only while the new one fits
  // inside it. Cleared by every show(), like the row actions.
  void setSizeHint(int dialogWidth, int visibleRows) {
    minDialogWidth = dialogWidth;
    minVisibleRows = visibleRows;
    layoutValid = false;
  }

  // The dialog's own geometry, for a caller that wants the next popup to match it
  // (setSizeHint above). frameRect() is the same box plus the frame thickness.
  int dialogWidth(const GfxRenderer& renderer) const { return getLayout(renderer).dialog.width; }
  int visibleRows(const GfxRenderer& renderer) const { return getLayout(renderer).visibleRows; }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const int count = static_cast<int>(ownedStrings.size());
    int tx = 0;
    int ty = 0;
    if (input.wasScreenTouchDown(tx, ty)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      for (int row = 0; row < static_cast<int>(hitLayout.rows.size()); row++) {
        if (contains(hitLayout.rows[row], tx, ty)) {
          const int i = scrollTop + row;
          if (i < count && selectedIndex != i) {
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
      for (int row = 0; row < static_cast<int>(hitLayout.rows.size()); row++) {
        if (!contains(hitLayout.rows[row], tx, ty)) continue;
        const int i = scrollTop + row;
        if (i >= count) break;
        selectedIndex = i;
        active = false;
        if (onSelectCallback) onSelectCallback(selectedIndex);
        requestUpdate();
        return true;
      }
      // Taps on the dialog chrome (title, padding) keep the popup open; taps outside dismiss it
      if (contains(hitLayout.dialog, tx, ty)) return true;
      active = false;
      requestUpdate();
      return true;
    }

    if (hasRowActions) {
      // Selection on the screen's up/down pair only -- the front pair is taken.
      if (input.wasPressed(MappedInputManager::Button::ScreenUp)) {
        moveSelection(-1, input.getRenderer());
        requestUpdate();
        return true;
      }
      if (input.wasPressed(MappedInputManager::Button::ScreenDown)) {
        moveSelection(1, input.getRenderer());
        requestUpdate();
        return true;
      }
      // The callback runs last and nothing here touches this object afterwards:
      // it is allowed to show() something else over the top.
      if (input.wasPressed(MappedInputManager::Button::ScreenLeft) && rowActions.onLeft) {
        rowActions.onLeft(selectedIndex);
        return true;
      }
      if (input.wasPressed(MappedInputManager::Button::ScreenRight) && rowActions.onRight) {
        rowActions.onRight(selectedIndex);
        return true;
      }
    }

    if (!hasRowActions && input.wasPressed(MappedInputManager::Button::NavPrevious)) {
      moveSelection(-1, input.getRenderer());
      requestUpdate();
      return true;
    } else if (!hasRowActions && input.wasPressed(MappedInputManager::Button::NavNext)) {
      moveSelection(1, input.getRenderer());
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
    if (hasRowActions) {
      // mapDirectionalLabels(), so each label lands on the button that performs
      // it whatever the orientation does to left and right. Up/Down are passed
      // as well rather than left blank: in a rotated orientation the screen's
      // up/down pair *is* the front pair, and then they are the labels that
      // belong in the hint bar.
      const auto labels = input.mapDirectionalLabels(
          tr(STR_BACK), rowActions.confirmLabel != nullptr ? rowActions.confirmLabel : tr(STR_SELECT),
          rowActions.leftLabel != nullptr ? rowActions.leftLabel : "",
          rowActions.rightLabel != nullptr ? rowActions.rightLabel : "", sideHintTop != nullptr ? sideHintTop : "",
          sideHintBottom != nullptr ? sideHintBottom : "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    }
    if (hasSideHints()) GUI.drawSideButtonHints(renderer, sideHintTop, sideHintBottom, sideHintFontId);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    // Through the layout first: it is what clamps scrollTop to the window the
    // theme actually gives, and drawing reads scrollTop.
    getLayout(renderer);
    GUI.drawOptionPopup(renderer, spec());
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
    // One rect per *visible* row, top to bottom. Row n is option
    // scrollTop + n -- the list scrolls, the window does not move.
    std::vector<Rect> rows;
    int visibleRows = 0;
  };

  BaseTheme::OptionPopupSpec spec() const {
    BaseTheme::OptionPopupSpec s;
    s.title = title.c_str();
    s.options = &ownedStrings;
    s.values = ownedValues.empty() ? nullptr : &ownedValues;
    s.selectedIndex = selectedIndex;
    s.scrollTop = scrollTop;
    s.leftAlign = leftAligned;
    s.compact = compact;
    s.minDialogWidth = minDialogWidth;
    s.minVisibleRows = minVisibleRows;
    s.note = note.empty() ? nullptr : note.c_str();
    return s;
  }

  // Text measurement is expensive and wasScreenTouchDown() is level-triggered, so the
  // layout is computed once per show() and cached rather than rebuilt every loop().
  // The geometry itself comes from the theme, so hit test and drawing cannot
  // disagree about where a row is (BaseTheme::optionPopupGeometry()).
  const Layout& getLayout(const GfxRenderer& renderer) const {
    if (layoutValid) return layout;

    const auto geometry = GUI.optionPopupGeometry(renderer, spec());
    // A popup can open with a selection past the first window (a picker opens
    // on the current value); the window has to start where that row is.
    const int count = static_cast<int>(ownedStrings.size());
    if (geometry.visibleRows > 0 && geometry.visibleRows < count) {
      if (selectedIndex >= scrollTop + geometry.visibleRows) scrollTop = selectedIndex - geometry.visibleRows + 1;
      if (selectedIndex < scrollTop) scrollTop = selectedIndex;
      if (scrollTop > count - geometry.visibleRows) scrollTop = count - geometry.visibleRows;
      if (scrollTop < 0) scrollTop = 0;
    } else {
      scrollTop = 0;
    }
    layout.dialog = geometry.dialog;
    layout.visibleRows = geometry.visibleRows;
    layout.rows.clear();
    layout.rows.reserve(geometry.visibleRows);
    for (int row = 0; row < geometry.visibleRows; row++) {
      layout.rows.push_back(
          Rect{geometry.rowX, geometry.firstRowY + row * geometry.rowStep, geometry.rowWidth, geometry.rowHeight});
    }
    layoutValid = true;
    return layout;
  }

  // Wraps at both ends, and drags the scroll window along so the selected row
  // is always one of the drawn ones.
  void moveSelection(const int delta, const GfxRenderer& renderer) {
    const int count = static_cast<int>(ownedStrings.size());
    if (count == 0) return;
    selectedIndex = (selectedIndex + delta + count) % count;
    const int visible = getLayout(renderer).visibleRows;
    if (visible <= 0 || visible >= count) return;
    int top = scrollTop;
    if (selectedIndex < top) top = selectedIndex;
    if (selectedIndex >= top + visible) top = selectedIndex - visible + 1;
    if (top < 0) top = 0;
    if (top > count - visible) top = count - visible;
    if (top == scrollTop) return;
    scrollTop = top;
    // Row rects are addressed by visible position, so they are still correct;
    // only which option each one stands for changed. The dialog itself never
    // moves, which is why a scroll costs no relayout of the frame.
    layoutValid = false;
  }

  // Every show() drops the previous popup's row actions and side hints. A menu
  // and a pin list share one OptionPopup on the map screen, and a menu that
  // inherited the list's buttons would delete a pin from a row that means
  // something else entirely.
  void resetRowChrome() {
    hasRowActions = false;
    rowActions = RowActions{};
    sideHintTop = nullptr;
    sideHintBottom = nullptr;
    sideHintFontId = SMALL_FONT_ID;
    minDialogWidth = 0;
    minVisibleRows = 0;
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
  bool compact = false;
  int selectedIndex = 0;
  // First option on screen. Only ever non-zero for a list longer than the
  // dialog's row window (BaseTheme::kOptionPopupMaxVisibleRows). Mutable
  // because getLayout() clamps it: the window size is the theme's answer and
  // is not known until a renderer is in hand.
  mutable int scrollTop = 0;
  std::function<void(int)> onSelectCallback;
  bool hasRowActions = false;
  RowActions rowActions;
  const char* sideHintTop = nullptr;
  const char* sideHintBottom = nullptr;
  int sideHintFontId = SMALL_FONT_ID;
  int minDialogWidth = 0;
  int minVisibleRows = 0;
  mutable Layout layout;
  std::string note;
  mutable bool layoutValid = false;
};
