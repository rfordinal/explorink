#include "RouteEmptyActivity.h"

#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/icons/home_icons.h"
#include "fontIds.h"
#include "images/Mountains.h"

namespace {

constexpr const char* kLogTag = "ROUTEEMPTY";

constexpr int kTitleFont = UI_12_FONT_ID;  // bold
constexpr int kBodyFont = UI_10_FONT_ID;

}  // namespace

RouteEmptyActivity::RouteEmptyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("RouteEmpty", renderer, mappedInput) {}

void RouteEmptyActivity::onEnter() {
  Activity::onEnter();
  LOG_INF(kLogTag, "card has no trips, showing the explanation");
  renderScreen();
}

void RouteEmptyActivity::loop() {
  Activity::loop();

  bool selectionMoved = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (selected_ != Action::Continue) {
      selected_ = Action::Continue;
      selectionMoved = true;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (selected_ != Action::Back) {
      selected_ = Action::Back;
      selectionMoved = true;
    }
  }

  // A direct tap on a row, same convention as GnssAcquireActivity's rows: the
  // default touch mode draws no hint boxes, so the rows are the buttons.
  for (int index = 0; index < kActionCount; ++index) {
    int x, y, w, h;
    actionRect(index, x, y, w, h);
    if (mappedInput.wasTapInRect(x, y, w, h)) {
      activate(static_cast<Action>(index));
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selected_);
    return;
  }
  // Back means the same thing here as it does in RouteSelectActivity: home, not
  // a route list with nothing in it.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::MAP);
    return;
  }

  if (selectionMoved) drawActions();
}

void RouteEmptyActivity::activate(Action action) {
  switch (action) {
    case Action::Continue:
      LOG_INF(kLogTag, "continuing to the map with no trip");
      activityManager.goToMap();
      return;
    case Action::Back:
      LOG_INF(kLogTag, "back to home");
      onGoHome(HomeMenuItem::MAP);
      return;
  }
}

void RouteEmptyActivity::actionRect(int index, int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  h = metrics.menuRowHeight;
  x = metrics.contentSidePadding;
  w = pageWidth - metrics.contentSidePadding * 2;
  const int blockBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int blockTop = blockBottom - (h * kActionCount + metrics.menuSpacing * (kActionCount - 1));
  y = blockTop + index * (h + metrics.menuSpacing);
}

void RouteEmptyActivity::drawRidge() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  int ax, ay, aw, ah;
  actionRect(0, ax, ay, aw, ah);
  (void)ax;
  (void)aw;
  (void)ah;

  const int top = metrics.topPadding + renderer.getLineHeight(kTitleFont) + renderer.getLineHeight(kBodyFont) +
                   metrics.verticalSpacing;
  const int bottom = ay - metrics.verticalSpacing;
  const int horizon = bottom;

  renderer.fillRect(0, top, pageWidth, bottom - top, false);

  // Same clipped blit GnssAcquireActivity uses: the asset is wider than the
  // panel and seated so its bottom runs past the horizon, and
  // GfxRenderer::drawPixel() LOG_ERRs every out-of-range pixel rather than
  // clipping quietly.
  const int ridgeX = (pageWidth - MOUNTAINS_WIDTH) / 2;
  const int ridgeY = horizon - MOUNTAINS_HEIGHT + 40;  // seated a bit below the horizon, same crop as the wait screen
  const int rowBytes = (MOUNTAINS_WIDTH + 7) / 8;
  for (int row = 0; row < MOUNTAINS_HEIGHT; ++row) {
    const int py = ridgeY + row;
    if (py < top || py > horizon) continue;
    for (int col = 0; col < MOUNTAINS_WIDTH; ++col) {
      const int px = ridgeX + col;
      if (px < 0 || px >= pageWidth) continue;
      const uint8_t byte = Mountains[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) renderer.drawPixel(px, py, true);
    }
  }
  renderer.drawLine(0, horizon, pageWidth - 1, horizon, true);

  // The backpack -- the same glyph the home menu's Trips row uses
  // (components/icons/home_icons.h), standing on the ridge rather than
  // pointing at it. Drawn at 2x: at its native 32px it read as a smudge next
  // to a 700px-wide ridge. Ground rubbed out behind it first: the ridge is
  // line art, not a filled silhouette, so the mark would otherwise read as
  // more ridge.
  constexpr int kIconScale = 2;
  const int iconW = icon_trips.w * kIconScale;
  const int iconH = icon_trips.h * kIconScale;
  const int iconX = pageWidth / 2 - iconW / 2;
  const int iconY = horizon - iconH + 6;
  renderer.fillRect(iconX - 3, iconY - 3, iconW + 6, iconH + 6, false);
  // No scaled asset exists, so this blits each source pixel as a kIconScale
  // block -- nearest-neighbour, same trade-off gen_icons.py's rasteriser makes
  // at small sizes. A 32px glyph has no anti-aliasing to lose either way.
  const int iconRowBytes = (icon_trips.w + 7) / 8;
  for (int row = 0; row < icon_trips.h; ++row) {
    for (int col = 0; col < icon_trips.w; ++col) {
      const uint8_t byte = icon_trips.bits[row * iconRowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (!ink) continue;
      for (int dy = 0; dy < kIconScale; ++dy) {
        for (int dx = 0; dx < kIconScale; ++dx) {
          renderer.drawPixel(iconX + col * kIconScale + dx, iconY + row * kIconScale + dy, true);
        }
      }
    }
  }
}

void RouteEmptyActivity::drawActions() {
  const int lineHeight = renderer.getLineHeight(kBodyFont);

  for (int index = 0; index < kActionCount; ++index) {
    int x, y, w, h;
    actionRect(index, x, y, w, h);
    const bool highlighted = static_cast<Action>(index) == selected_;
    renderer.fillRect(x, y, w, h, false);
    if (highlighted) renderer.fillRect(x, y, w, h, true);
    const bool ink = !highlighted;

    const char* label = index == 0 ? tr(STR_ROUTE_SKIP) : tr(STR_BACK);
    const char* detail = index == 0 ? tr(STR_ROUTE_SKIP_DETAIL) : tr(STR_ROUTE_EMPTY_BACK_DETAIL);
    const int textY = y + (h - (lineHeight * 2)) / 2;
    renderer.drawCenteredText(kBodyFont, textY, label, ink, EpdFontFamily::BOLD);
    renderer.drawCenteredText(kBodyFont, textY + lineHeight, detail, ink);
  }

  int x0, y0, w0, h0;
  int x1, y1, w1, h1;
  actionRect(0, x0, y0, w0, h0);
  actionRect(1, x1, y1, w1, h1);
  renderer.displayBufferWindow(x0, y0, w0, (y1 + h1) - y0);
}

void RouteEmptyActivity::renderScreen() {
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  renderer.drawCenteredText(kTitleFont, metrics.topPadding, tr(STR_ROUTE_NONE_ON_CARD), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(kBodyFont, metrics.topPadding + renderer.getLineHeight(kTitleFont),
                            tr(STR_ROUTE_EMPTY_SUB), true);

  drawRidge();
  drawActions();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
