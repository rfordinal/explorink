#include "BaseTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/bookmark.h"
#include "components/icons/home_icons.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;
constexpr int bookmarkStatusIconWidth = 16;
constexpr int bookmarkStatusIconHeight = 14;
constexpr int bookmarkStatusIconGap = 4;
constexpr int bookmarkStatusIconTopCrop = 2;

// Greedy word-wrap for an OptionPopup title: short, one-off dynamic sentences
// like "Replace Camp with current location? (fix 7 min old)", never the
// paragraph-length text DictionaryDefinitionActivity::wrapText() handles, so
// a plain space-split with no hyphenation or hard character breaks is enough.
// Reported on the S8 2026-08-24: a long confirm title was drawn centered on
// the full screen width with no wrap at all, so it ran off both edges of the
// dialog (and the screen) instead of fitting inside it.
std::vector<std::string> wrapOptionPopupTitle(const GfxRenderer& renderer, const char* title, int maxWidth) {
  std::vector<std::string> lines;
  if (title == nullptr || *title == '\0') {
    lines.emplace_back();
    return lines;
  }
  if (maxWidth <= 0) {
    lines.emplace_back(title);
    return lines;
  }
  const std::string text(title);
  std::string current;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t next = text.find(' ', pos);
    const std::string word = text.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
    const std::string candidate = current.empty() ? word : current + " " + word;
    if (!current.empty() &&
        renderer.getTextWidth(UI_12_FONT_ID, candidate.c_str(), EpdFontFamily::BOLD) > maxWidth) {
      lines.push_back(current);
      current = word;
    } else {
      current = candidate;
    }
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  lines.push_back(current);
  return lines;
}

void drawBookmarkStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int bytesPerRow = bookmarkStatusIconWidth / 8;
  for (int row = 0; row < bookmarkStatusIconHeight; ++row) {
    for (int col = 0; col < bookmarkStatusIconWidth; ++col) {
      const uint8_t byte = BookmarkStatusIcon[(row + bookmarkStatusIconTopCrop) * bytesPerRow + col / 8];
      const uint8_t mask = 1U << (7 - (col % 8));
      renderer.drawPixel(x + col, y + row, (byte & mask) != 0);
    }
  }
}

}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  const int maxFillWidth = rect.width - 5;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  // +1 to round up so we always fill at least one pixel
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight);

  if (charging) {
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  }
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str());
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is already positioned for the icon (drawHeader calculated it)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str());
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height);
  fillBatteryIcon(renderer, iconRect, percentage);
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4, int fontId, int btn3FontId, int btn4FontId) const {
  if (fontId == 0) fontId = UI_10_FONT_ID;
  if (btn3FontId == 0) btn3FontId = fontId;
  if (btn4FontId == 0) btn4FontId = fontId;
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // X3 has wider screen in portrait (528 vs 480), use more spacing
  constexpr int x4ButtonPositions[] = {25, 130, 245, 350};
  constexpr int x3ButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int fontIds[] = {fontId, fontId, btn3FontId, btn4FontId};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(fontIds[i], labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(fontIds[i], textX, pageHeight - buttonY + textYOffset, labels[i]);
    }
  }

  renderer.setOrientation(orig_orientation);
}

namespace {
// Side-hint box geometry, in one place because two functions read it: the drawing
// below and sideButtonHintsRect(), which a caller repainting part of the panel
// needs in order to refresh exactly what these boxes cover. Two copies of these
// numbers would drift the first time one of them moved.
constexpr int kSideHintWidth = BaseMetrics::values.sideButtonHintsWidth;  // width on screen (height when rotated)
constexpr int kSideHintHeight = 80;                                       // height on screen (width when rotated)
constexpr int kSideHintMargin = 4;
constexpr int kSideHintX4TopY = 345;  // X4: both boxes stacked on the right
constexpr int kSideHintX3Y = 155;     // X3: one box per side, higher up
}  // namespace

Rect BaseTheme::buttonHintsRect(const GfxRenderer& renderer) const {
  if (gpio.hasTouch()) return Rect{0, 0, 0, 0};  // drawButtonHints() draws nothing on a touch panel
  // Full width: the four boxes are one band as far as anything trying to stay out
  // of their way is concerned. Height and offset are drawButtonHints()' own.
  const int height = BaseMetrics::values.buttonHintsHeight;
  return Rect{0, renderer.getScreenHeight() - height, renderer.getScreenWidth(), height};
}

Rect BaseTheme::sideButtonHintsRect(const GfxRenderer& renderer) const {
  const int screenWidth = renderer.getScreenWidth();
  if (gpio.hasTouch()) return Rect{0, 0, 0, 0};  // drawSideButtonHints() draws nothing on a touch panel
  if (gpio.deviceIsX3()) {
    // Both sides, so the rect spans the full width -- the two boxes are the left
    // and the right edge of the same band.
    return Rect{0, kSideHintX3Y, screenWidth, kSideHintHeight};
  }
  return Rect{screenWidth - kSideHintMargin - kSideHintWidth, kSideHintX4TopY, kSideHintWidth, kSideHintHeight * 2};
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                                    int fontId) const {
  if (gpio.hasTouch()) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = kSideHintWidth;
  constexpr int buttonHeight = kSideHintHeight;
  constexpr int buttonMargin = kSideHintMargin;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = kSideHintX3Y;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = buttonMargin;
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(fontId, topBtn);
      const int textHeight = renderer.getTextHeight(fontId);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(fontId, textX, textY, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(fontId, bottomBtn);
      const int textHeight = renderer.getTextHeight(fontId);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(fontId, textX, textY, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = kSideHintX4TopY;
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    // White backing first: this call draws no fill of its own, only the
    // border and text, so a caller over live content (a map, a rendered
    // page) shows through it -- unlike drawButtonHints() just above, which
    // does fill each box.
    const bool hasTop = topBtn != nullptr && topBtn[0] != '\0';
    const bool hasBottom = bottomBtn != nullptr && bottomBtn[0] != '\0';
    if (hasTop || hasBottom) {
      const int footprintHeight = (hasTop && hasBottom) ? 2 * buttonHeight : buttonHeight;
      const int footprintY = (hasTop) ? topButtonY : topButtonY + buttonHeight;
      renderer.fillRect(x, footprintY, buttonWidth, footprintHeight, false);
    }

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(fontId, labels[i]);
        const int textHeight = renderer.getTextHeight(fontId);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(fontId, textX, textY, labels[i]);
      }
    }
  }
}

int BaseTheme::getListRowStep(bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  return rowHeight;
}

int BaseTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  const int rowStep = getListRowStep(hasSubtitle);
  if (rowStep <= 0) return 1;
  return std::max(1, contentHeight / rowStep);
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  int pageItems = rowHeight > 0 ? std::max(1, rect.height / rowHeight) : 1;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Draw up arrow at top (^) - narrow point at top, wide base at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      const int startX = centerX - i;
      renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
    }

    // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  // Draw selection
  int contentWidth = rect.width - 5;
  if (selectedIndex >= 0) {
    renderer.fillRect(rect.x, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width, rowHeight);
  }
  constexpr int minValueGap = 10;

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    int rowTextWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2;
    std::string valueText;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        int maxValW = std::max(0, rowTextWidth - 40 - minValueGap);
        valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxValW);
        int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + minValueGap;
        rowTextWidth -= valueWidth;
      }
    }

    auto itemName = rowTitle(i);
    auto font = UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), rowTextWidth);
    renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(), i != selectedIndex);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(font, item.c_str());
      const int lineH = renderer.getLineHeight(font);
      const int tx = rect.x + BaseMetrics::values.contentSidePadding;
      for (int py = itemY; py < itemY + lineH; py++)
        for (int px = tx; px < tx + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowSubtitle != nullptr) {
      std::string subtitleText = rowSubtitle(i);
      if (!subtitleText.empty()) {
        auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
        renderer.drawText(SMALL_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY + 22, subtitle.c_str(),
                          i != selectedIndex);
      }
    }

    if (!valueText.empty()) {
      const auto valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      int valueY = itemY;
      if (rowSubtitle != nullptr) {
        valueY = itemY + 10;
      }
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - BaseMetrics::values.contentSidePadding - valueTextWidth,
                        valueY, valueText.c_str(), i != selectedIndex);
    }
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  // Hide last battery draw
  constexpr int maxBatteryWidth = 80;
  renderer.fillRect(rect.x + rect.width - maxBatteryWidth, rect.y + 5, maxBatteryWidth,
                    BaseMetrics::values.batteryHeight + 10, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - BaseMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, BaseMetrics::values.batteryWidth, BaseMetrics::values.batteryHeight},
                   showBatteryPercentage);

  if (title) {
    int padding = rect.width - batteryX + BaseMetrics::values.batteryWidth;
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title,
                                                 rect.width - padding * 2 - BaseMetrics::values.contentSidePadding * 2,
                                                 EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, rect.y + 5, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(
        SMALL_FONT_ID, subtitle, rect.width - BaseMetrics::values.contentSidePadding * 2, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - BaseMetrics::values.contentSidePadding - truncatedSubtitleWidth, subtitleY,
                      truncatedSubtitle.c_str(), true);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int maxListValueWidth = 200;

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  int rightSpace = BaseMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + 10;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_12_FONT_ID, label, rect.width - BaseMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, currentX, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth =
        renderer.getTextWidth(UI_12_FONT_ID, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    // Draw underline for selected tab
    if (tab.selected) {
      if (selected) {
        renderer.fillRect(currentX - 3, rect.y, textWidth + 6, lineHeight + underlineGap);
      } else {
        renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textWidth, underlineHeight);
      }
    }

    // Draw tab label
    renderer.drawText(UI_12_FONT_ID, currentX, rect.y, tab.label, !(tab.selected && selected),
                      tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }
}

bool BaseTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                  const int x, const int y, int& index) const {
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height) {
    return false;
  }

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  for (size_t i = 0; i < tabs.size(); i++) {
    const auto& tab = tabs[i];
    const int textWidth =
        renderer.getTextWidth(UI_12_FONT_ID, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int left = (i == 0) ? rect.x : currentX - BaseMetrics::values.tabSpacing / 2;
    const int right = currentX + textWidth + BaseMetrics::values.tabSpacing / 2;
    if (x >= left && x < right) {
      index = static_cast<int>(i);
      return true;
    }
    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }

  return false;
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    HalFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      HalFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // Draw the cover image (bookWidth and bookHeight already match image aspect ratio)
          renderer.drawBitmap(bitmap, bookX, bookY, bookWidth, bookHeight);

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title and author
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = tr(STR_CONTINUE_READING);
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, tr(STR_CONTINUE_READING), !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_NO_OPEN_BOOK));
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
  }
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = BaseMetrics::values.verticalSpacing + rect.y +
                      static_cast<int>(i) * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY =
        tileY + (BaseMetrics::values.menuRowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

// Every second pixel row to white, over whatever was already drawn there. A
// line screen and not a checkerboard: the panel has no grey in BW mode
// (../../../docs/eink-grayscale.md), and at this text size a checkerboard
// speckles the glyphs instead of greying them. Called after the label, the icon
// and the value are down, so it dims all three at once.
void BaseTheme::dimDisabledRow(const GfxRenderer& renderer, const int x, const int y, const int width,
                               const int height) {
  constexpr int kDimStep = 2;
  for (int line = y; line < y + height; line += kDimStep) {
    renderer.fillRect(x, line, width, 1, false);
  }
}

// Home's row list. Geometry is deliberately not in ThemeMetrics: these are the
// Home screen's own numbers (a 480 px panel, one screenful of seven rows), and a
// theme that wants a different Home overrides this method instead.
void BaseTheme::drawHomeMenu(const GfxRenderer& renderer, const Rect rect, const HomeRow* rows, const int rowCount,
                             const int selectedIndex, const int rowHeight) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The selected block's inset, the gap after the glyph, the block's radius.
  constexpr int kRowInset = 16;
  constexpr int kIconGap = 18;
  constexpr int kBlockRadius = 10;
  constexpr int kBlockMargin = 2;
  // Disabled rows are dimmed by BaseTheme::dimDisabledRow(), which is where the
  // line-screen reasoning lives -- OptionPopup draws its own disabled rows
  // through the same helper.

  const int left = rect.x + metrics.contentSidePadding;
  const int width = rect.width - metrics.contentSidePadding * 2;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  for (int i = 0; i < rowCount; ++i) {
    const int top = rect.y + i * rowHeight;
    const bool selected = i == selectedIndex;
    const bool enabled = rows[i].enabled;

    if (selected) {
      renderer.fillRoundedRect(left, top + kBlockMargin, width, rowHeight - kBlockMargin * 2, kBlockRadius,
                               Color::Black);
    }

    int x = left + kRowInset;
    if (rows[i].icon != nullptr) {
      const freeink::Icon& icon = *rows[i].icon;
      renderer.drawMono1bpp(icon.bits, x, top + (rowHeight - icon.h) / 2, icon.w, icon.h, !selected);
      x += icon.w + kIconGap;
    }
    renderer.drawText(UI_12_FONT_ID, x, top + (rowHeight - lineHeight) / 2, rows[i].label, !selected);

    const freeink::Icon& chevron = icon_chevron;
    renderer.drawMono1bpp(chevron.bits, left + width - kRowInset - chevron.w, top + (rowHeight - chevron.h) / 2,
                          chevron.w, chevron.h, !selected);

    if (!enabled) {
      dimDisabledRow(renderer, left, top + kBlockMargin, width, rowHeight - kBlockMargin * 2);
    }

    // Hairline between rows, drawn after the dimming so a dimmed row does not
    // eat the separator above it. Not above the first row and not against the
    // selected block, which is its own boundary.
    if (i > 0 && !selected && selectedIndex != i - 1) {
      renderer.drawLine(left + 4, top, left + width - 4, top, 1, true);
    }
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  // Scale y position proportionally to screen height
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, metrics.popupTextInverted, popupFontFamily);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool fillMargin, const bool isPageBookmarked, const bool pageCountEstimated) const {
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const auto sb = SETTINGS.statusBarSpec();
  const bool showStatusBarTextLane = sb.textLaneVisible(halClock.isAvailable());

  // Draw Progress Text
  const auto screenHeight = renderer.getScreenHeight();
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;

  const int leftClusterX = metrics.statusBarHorizontalMargin + orientedMarginLeft + 1;
  const int rightClusterX = renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight;
  int leftClusterWidth = 0;
  int rightClusterWidth = 0;

  if (sb.showBookProgressPercent || sb.showChapterPageCount) {
    // Right aligned text for progress counter
    char progressStr[32];

    // Prefix the page count with "~" while a still-building spine only yields an estimated total.
    const char* estimatePrefix = pageCountEstimated ? "~" : "";

    if (sb.showBookProgressPercent && sb.showChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%s%d/%d  %.0f%%", estimatePrefix, currentPage, pageCount,
               bookProgress);
    } else if (sb.showBookProgressPercent) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%s%d/%d", estimatePrefix, currentPage, pageCount);
    }

    int progressTextWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    renderer.drawText(SMALL_FONT_ID, rightClusterX - progressTextWidth, textY, progressStr);

    rightClusterWidth += progressTextWidth;
  }

  // Draw Progress Bar
  if (sb.showsProgressBar()) {
    const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
    const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
    const int progressBarMaxWidth = renderer.getScreenWidth() - barMarginLeft - barMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom - sb.progressBarHeightPx -
                             paddingBottom + (fillMargin ? 1 : 0);
    size_t progress;
    if (sb.progressBarMode == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      // Chapter progress
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    const int barHeight = sb.progressBarHeightPx + (fillMargin ? orientedMarginBottom - 1 : 0);
    renderer.fillRect(barMarginLeft, progressBarY, barWidth, barHeight, true);
  }

  // Draw Battery
  const bool showBatteryPercentage = sb.showBatteryPercent;

  if (sb.showBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{leftClusterX + leftClusterWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
    int batteryWidth = metrics.batteryWidth;

    if (showBatteryPercentage) {
      const uint16_t percentage = powerManager.getBatteryPercentage();
      // width of icon + spacing + text for layout purposes
      batteryWidth +=
          batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, (std::to_string(percentage) + "%").c_str());
    }

    leftClusterWidth += batteryWidth;
  }

  // Draw Clock (X3 only — DS3231 RTC)
  if (sb.showsClock() && halClock.isAvailable()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), sb.clockUtcOffsetQ, sb.clock12h)) {
      int clockTextWidth = renderer.getTextWidth(SMALL_FONT_ID, timeBuf);
      int clockX = 0;
      // Position to the left or right of the progress text (with a small gap)
      if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_LEFT) {
        clockX = leftClusterX + leftClusterWidth + (leftClusterWidth > 0 ? 10 : 0);
        leftClusterWidth += clockTextWidth + 10;
      } else if (sb.clockMode == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT) {
        clockX = rightClusterX - rightClusterWidth - (rightClusterWidth > 0 ? 10 : 0) - clockTextWidth;
        rightClusterWidth += clockTextWidth + 10;
      }
      renderer.drawText(SMALL_FONT_ID, clockX, textY, timeBuf);
    }
  }

  // Draw Bookmark
  if (showStatusBarTextLane && isPageBookmarked) {
    const int bookmarkGap = leftClusterWidth > 0 ? bookmarkStatusIconGap : 0;
    const int bookmarkX = leftClusterX + leftClusterWidth + bookmarkGap;
    const int bookmarkY = textY + 5;
    drawBookmarkStatusIcon(renderer, bookmarkX, bookmarkY);
    leftClusterWidth += bookmarkStatusIconWidth + bookmarkGap;
  }

  // Draw Title
  if (!title.empty()) {
    textY -= textYOffset;
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth =
        renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = leftClusterWidth + 30;
    const int titleMarginRight = rightClusterWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleWidth;
    titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    }

    renderer.drawText(SMALL_FONT_ID,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str());
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

BaseTheme::OptionPopupSpacing BaseTheme::optionPopupSpacing(const ThemeMetrics& metrics, const bool compact) {
  const int hPadding = metrics.optionPopupSelectionHPadding;
  if (!compact) {
    return OptionPopupSpacing{
        metrics.optionPopupItemSpacing, metrics.optionPopupInnerPadding, hPadding, metrics.optionPopupSelectionVPadding,
        metrics.optionPopupTitleGap,    std::max(4, hPadding / 2),       120};
  }
  // Horizontal padding is left alone: it is what keeps a label off the frame
  // and a value off the label, and it costs one column, not one per row.
  return OptionPopupSpacing{std::max(2, metrics.optionPopupItemSpacing / 2),
                            std::max(8, metrics.optionPopupInnerPadding * 7 / 10),
                            hPadding,
                            std::max(4, metrics.optionPopupSelectionVPadding / 2),
                            std::max(6, metrics.optionPopupTitleGap * 6 / 10),
                            std::max(4, hPadding / 2),
                            100};
}

BaseTheme::OptionPopupGeometry BaseTheme::optionPopupGeometry(const GfxRenderer& renderer,
                                                              const OptionPopupSpec& spec) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto spacing = optionPopupSpacing(metrics, spec.compact);
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const int optionFontId = metrics.optionPopupUseSmallFont ? UI_10_FONT_ID : UI_12_FONT_ID;
  const EpdFontFamily::Style optionStyle =
      metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

  const int optionLineHeight = renderer.getLineHeight(optionFontId);
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  // The note rides in the option font, one line, with the row gap under it. It
  // is not a row: nothing scrolls it and nothing can select it.
  const int noteLineHeight = spec.note != nullptr ? optionLineHeight : 0;
  const int noteBlock = spec.note != nullptr ? noteLineHeight + spacing.itemSpacing : 0;
  const int rowHeight = optionLineHeight + spacing.selectionVPadding * 2;
  const int rowStep = rowHeight + spacing.itemSpacing;
  const int optionCount = spec.options ? static_cast<int>(spec.options->size()) : 0;

  // The title wraps to the widest a dialog can ever be, not to whatever this
  // dialog's content happens to need -- that budget cannot depend on dialogW
  // below, which itself depends on the title's wrapped width, or the two
  // would need to agree on which comes first. A title this wide only reaches
  // a caller with a very long dynamic string (MapActivity::savePin()'s replace
  // confirmation, "Replace <label> with current location? (fix <age> old)"),
  // which used to run off both edges of the dialog with no wrap at all
  // (reported on the S8 2026-08-24).
  // The title's first line has to leave room for the "n/m" scroll counter in
  // the same top-right corner, or the two collide -- optionCount alone decides
  // this, not the row-fit math below, because that math needs titleBlockHeight
  // and titleBlockHeight needs the title wrapped first. optionCount >
  // kOptionPopupMaxVisibleRows is the same "will it scroll" condition as
  // `optionCount > visibleRows` in the common case (the row cap, not the
  // height budget, is what forces most lists to scroll); reserving space on a
  // false positive just centres the title a little less perfectly, not
  // wrongly.
  int counterReserve = 0;
  if (optionCount > kOptionPopupMaxVisibleRows) {
    char counter[12];
    snprintf(counter, sizeof(counter), "%d/%d", spec.selectedIndex + 1, optionCount);
    counterReserve = renderer.getTextWidth(UI_10_FONT_ID, counter) + spacing.itemSpacing;
  }
  const int titleMaxWidth =
      pageWidth - metrics.optionPopupDialogSideMargin * 2 - spacing.innerPadding * 2 - counterReserve;
  const std::vector<std::string> titleLines =
      spec.title != nullptr ? wrapOptionPopupTitle(renderer, spec.title, titleMaxWidth) : std::vector<std::string>{};
  const int titleLineCount = static_cast<int>(std::max<size_t>(1, titleLines.size()));

  // A row with a value is label + gap + boxed value; a plain row is just the
  // label. The title has to fit too, and it is drawn in the bigger font --
  // the first line also carries the counter reserve, since that is the line
  // it shares a row with.
  int maxTextWidth = 0;
  for (size_t i = 0; i < titleLines.size(); ++i) {
    int w = renderer.getTextWidth(UI_12_FONT_ID, titleLines[i].c_str(), EpdFontFamily::BOLD);
    if (i == 0) w += counterReserve;
    if (w > maxTextWidth) maxTextWidth = w;
  }
  if (spec.note != nullptr) {
    const int noteWidth = renderer.getTextWidth(optionFontId, spec.note, EpdFontFamily::REGULAR);
    if (noteWidth > maxTextWidth) maxTextWidth = noteWidth;
  }
  for (int i = 0; i < optionCount; i++) {
    int w = renderer.getTextWidth(optionFontId, (*spec.options)[i].c_str(), optionStyle);
    if (spec.values && i < static_cast<int>(spec.values->size()) && !(*spec.values)[i].empty()) {
      w += spacing.selectionHPadding + renderer.getTextWidth(optionFontId, (*spec.values)[i].c_str(), optionStyle) +
           spacing.valuePadding * 2;
    }
    if (w > maxTextWidth) maxTextWidth = w;
  }

  // How many rows are on screen at once. Two bounds, and the point of both is
  // that the dialog must not grow with its list: a caller that snapshots the
  // pixels under it (MapActivity's menu backdrop) pays for every row in RAM,
  // and a dialog taller than half the panel stops reading as a dialog.
  // Anything past the window scrolls.
  const int titleBlockHeight = titleLineHeight * titleLineCount;
  const int chromeHeight = titleBlockHeight + spacing.titleGap + noteBlock + spacing.innerPadding * 2;
  const int heightBudget = pageHeight * kOptionPopupMaxHeightPercent / 100;
  int visibleRows = optionCount;
  if (rowStep > 0 && optionCount > 0) {
    const int fits = (heightBudget - chromeHeight + spacing.itemSpacing) / rowStep;
    visibleRows = std::min(optionCount, std::min(kOptionPopupMaxVisibleRows, std::max(1, fits)));
  }

  // A caller can ask for a floor on both, so a popup that replaces another one
  // keeps its size instead of shrinking to its own content -- see
  // OptionPopupSpec::minVisibleRows. The ceilings above still win: the floor can
  // never grow the dialog past the row cap or the panel's side margins.
  if (spec.minVisibleRows > visibleRows) {
    const int fitsCap = rowStep > 0 ? (heightBudget - chromeHeight + spacing.itemSpacing) / rowStep : visibleRows;
    visibleRows = std::min(spec.minVisibleRows, std::min(kOptionPopupMaxVisibleRows, std::max(1, fitsCap)));
  }

  const int listHeight = visibleRows > 0 ? rowHeight * visibleRows + spacing.itemSpacing * (visibleRows - 1) : 0;
  int dialogW =
      std::min((maxTextWidth + spacing.innerPadding * 2 + spacing.selectionHPadding * 2) * spacing.widthPercent / 100,
               pageWidth - metrics.optionPopupDialogSideMargin * 2);
  if (spec.minDialogWidth > dialogW) {
    dialogW = std::min(spec.minDialogWidth, pageWidth - metrics.optionPopupDialogSideMargin * 2);
  }
  const int dialogH = titleBlockHeight + spacing.titleGap + noteBlock + listHeight + spacing.innerPadding * 2;

  OptionPopupGeometry geometry;
  geometry.dialog = Rect{(pageWidth - dialogW) / 2, (pageHeight - dialogH) / 2, dialogW, dialogH};
  geometry.rowX = geometry.dialog.x + spacing.innerPadding;
  geometry.rowWidth = dialogW - spacing.innerPadding * 2;
  geometry.noteY =
      spec.note != nullptr ? geometry.dialog.y + spacing.innerPadding + titleBlockHeight + spacing.titleGap : 0;
  geometry.noteLineHeight = noteLineHeight;
  geometry.firstRowY = geometry.dialog.y + spacing.innerPadding + titleBlockHeight + spacing.titleGap + noteBlock;
  geometry.rowHeight = rowHeight;
  geometry.rowStep = rowStep;
  geometry.visibleRows = visibleRows;
  geometry.titleLineHeight = titleLineHeight;
  geometry.titleLineCount = titleLineCount;
  return geometry;
}

void BaseTheme::drawOptionPopup(const GfxRenderer& renderer, const OptionPopupSpec& spec) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto spacing = optionPopupSpacing(metrics, spec.compact);
  const auto geometry = optionPopupGeometry(renderer, spec);
  const Rect& dialog = geometry.dialog;

  const int optionFontId = metrics.optionPopupUseSmallFont ? UI_10_FONT_ID : UI_12_FONT_ID;
  const EpdFontFamily::Style optionStyle =
      metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int optionLineHeight = renderer.getLineHeight(optionFontId);
  const int optionCount = spec.options ? static_cast<int>(spec.options->size()) : 0;
  const int frameThickness = metrics.popupFrameThickness;
  const int frameRadius = metrics.popupCornerRadius;

  if (frameRadius > 0) {
    renderer.fillRoundedRect(dialog.x - frameThickness, dialog.y - frameThickness, dialog.width + frameThickness * 2,
                             dialog.height + frameThickness * 2, frameRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(dialog.x, dialog.y, dialog.width, dialog.height, frameRadius, Color::Black);
    renderer.fillRoundedRect(dialog.x + frameThickness, dialog.y + frameThickness, dialog.width - frameThickness * 2,
                             dialog.height - frameThickness * 2,
                             frameRadius - frameThickness > 0 ? frameRadius - frameThickness : 0, Color::White);
  } else {
    renderer.fillRect(dialog.x - frameThickness, dialog.y - frameThickness, dialog.width + frameThickness * 2,
                      dialog.height + frameThickness * 2, true);
    renderer.fillRect(dialog.x, dialog.y, dialog.width, dialog.height, false);
  }

  int y = dialog.y + spacing.innerPadding;
  // Same budget optionPopupGeometry() wrapped the title to -- both have to
  // agree, or the dialog's reserved height and what actually gets drawn into
  // it drift apart.
  const int titleMaxWidth = renderer.getScreenWidth() - metrics.optionPopupDialogSideMargin * 2 - spacing.innerPadding * 2;
  const std::vector<std::string> titleLines =
      spec.title != nullptr ? wrapOptionPopupTitle(renderer, spec.title, titleMaxWidth) : std::vector<std::string>{};
  for (const std::string& line : titleLines) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += geometry.titleLineHeight;
  }
  // Only when the list does not fit: which slice of it is on screen. Without
  // it a scrolled list reads as a short list that lost rows. Pinned to the
  // title's first line -- a wrapped title and a scrolled list do not happen
  // together in practice, and the counter is a corner mark, not part of the
  // sentence.
  if (optionCount > geometry.visibleRows) {
    char counter[12];
    snprintf(counter, sizeof(counter), "%d/%d", spec.selectedIndex + 1, optionCount);
    const int counterW = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, dialog.x + dialog.width - spacing.innerPadding - counterW,
                      dialog.y + spacing.innerPadding, counter, true);
  }

  if (metrics.optionPopupTitleSeparator) {
    const int sepY = y + spacing.titleGap / 2;
    renderer.drawLine(dialog.x + spacing.innerPadding, sepY, dialog.x + dialog.width - spacing.innerPadding, sepY,
                      true);
  }

  // The note: one line under the title, centred like it, in the option font so
  // it reads as a fact about the dialog's subject rather than as a row. Never
  // selectable -- the hit test only knows about geometry.rows.
  if (spec.note != nullptr) {
    renderer.drawCenteredText(optionFontId, geometry.noteY, spec.note, true, EpdFontFamily::REGULAR);
  }

  const int selectionRadius = metrics.optionPopupSelectionRadius;

  for (int row = 0; row < geometry.visibleRows; row++) {
    const int i = spec.scrollTop + row;
    if (i < 0 || i >= optionCount) continue;
    const int itemY = geometry.firstRowY + row * geometry.rowStep;
    const bool selected = (i == spec.selectedIndex);
    const bool rowDisabled =
        spec.disabled != nullptr && i < static_cast<int>(spec.disabled->size()) && (*spec.disabled)[i] != 0;
    const char* labelText = (*spec.options)[i].c_str();

    if (metrics.optionPopupDrawAllRows || selected) {
      Color rowColor;
      if (selected) {
        rowColor = metrics.optionPopupSelectionLight ? Color::LightGray : Color::Black;
      } else {
        rowColor = Color::White;
      }
      if (selectionRadius > 0) {
        renderer.fillRoundedRect(geometry.rowX, itemY, geometry.rowWidth, geometry.rowHeight, selectionRadius,
                                 rowColor);
      } else {
        renderer.fillRect(geometry.rowX, itemY, geometry.rowWidth, geometry.rowHeight, rowColor == Color::Black);
      }
    }

    const int textW = renderer.getTextWidth(optionFontId, labelText, optionStyle);
    const int textY = itemY + (geometry.rowHeight - optionLineHeight) / 2;
    const int textX =
        spec.leftAlign ? geometry.rowX + spacing.selectionHPadding : geometry.rowX + (geometry.rowWidth - textW) / 2;
    // Unselected items: text is dark (invert=true means draw on white bg).
    // Selected on dark bg: text must be white (invert=false).
    // Selected on light bg: text stays dark (invert=true).
    const bool invertText = selected ? metrics.optionPopupSelectionLight : true;
    renderer.drawText(optionFontId, textX, textY, labelText, invertText, optionStyle);

    // The value, right-aligned, boxed on the selected row -- the same "this is
    // the changeable part" cue the Settings list gives (LyraTheme::drawList()).
    const bool hasValue =
        spec.values != nullptr && i < static_cast<int>(spec.values->size()) && !(*spec.values)[i].empty();
    if (hasValue) {
      const char* valueText = (*spec.values)[i].c_str();
      const int valueW = renderer.getTextWidth(optionFontId, valueText, optionStyle);
      const int boxW = valueW + spacing.valuePadding * 2;
      const int boxX = geometry.rowX + geometry.rowWidth - boxW;
      if (selected) {
        if (selectionRadius > 0) {
          renderer.fillRoundedRect(boxX, itemY, boxW, geometry.rowHeight, selectionRadius, Color::Black);
        } else {
          renderer.fillRect(boxX, itemY, boxW, geometry.rowHeight, true);
        }
      }
      // Boxed value is white-on-black; everything else is dark on its own row.
      renderer.drawText(optionFontId, boxX + spacing.valuePadding, textY, valueText, !selected, optionStyle);
    }

    // Last, so it dims the label and the value together. A disabled row is
    // never the selected one (OptionPopup's walk skips it), so the lines always
    // land on a white row and never on the selection block.
    if (rowDisabled) {
      dimDisabledRow(renderer, geometry.rowX, itemY, geometry.rowWidth, geometry.rowHeight);
    }
  }
}
