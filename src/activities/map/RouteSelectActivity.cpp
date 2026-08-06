#include "RouteSelectActivity.h"

#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "ROUTESEL";

// Row geometry: name on one line, a detail line under it. Two lines because a
// route's name and its length do not fit side by side at 480 px in gloves.
constexpr int kRowGap = 8;

}  // namespace

RouteSelectActivity::RouteSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("RouteSelect", renderer, mappedInput) {}

void RouteSelectActivity::onEnter() {
  Activity::onEnter();

  entries_ = makeUniqueNoThrow<MapRouteStore::Entry[]>(MapRouteStore::kMaxRoutes);
  if (!entries_) {
    LOG_ERR(kLogTag, "OOM: %lu route entries", static_cast<unsigned long>(MapRouteStore::kMaxRoutes));
    // No list to choose from, so do not show an empty one -- go where Skip goes.
    // The map with no route is the behaviour this screen exists to add to, not a
    // failure state.
    activityManager.goToMap();
    return;
  }

  entryCount_ = MapRouteStore::list(entries_.get(), MapRouteStore::kMaxRoutes, foundCount_);
  // Highlight the first real route rather than Skip: a rider who opened this
  // screen came to load something. Skip is one press up.
  selected_ = entryCount_ > 0 ? 1 : 0;
  LOG_INF(kLogTag, "%lu routes listed (%lu on the card)", static_cast<unsigned long>(entryCount_),
          static_cast<unsigned long>(foundCount_));
  renderScreen();
}

void RouteSelectActivity::onExit() {
  Activity::onExit();
  entries_.reset();
  entryCount_ = 0;
}

void RouteSelectActivity::loop() {
  Activity::loop();

  const int rows = rowCount();
  bool moved = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Hard stops, not a wrap. A list this short reads better with ends than with
    // a highlight that jumps from the last row to the first.
    if (selected_ > 0) {
      --selected_;
      moved = true;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (selected_ + 1 < rows) {
      ++selected_;
      moved = true;
    }
  }
  if (moved) renderScreen();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    chooseSelected();
    return;
  }
  // Back goes home, not to the map: this screen is entered from the home menu,
  // and Skip is the row that means "map, no route". Two ways to reach the map
  // would leave Back meaning something different here than everywhere else.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::MAP);
  }
}

void RouteSelectActivity::chooseSelected() {
  if (isSkipRow(selected_)) {
    LOG_INF(kLogTag, "skip: opening the map with no route");
    activityManager.goToMap();
    return;
  }

  const MapRouteStore::Entry& entry = entries_[selected_ - 1];
  char path[MapRouteStore::kMaxFileNameBytes + 32];
  if (!MapRouteStore::buildPath(entry.fileName, path, sizeof(path))) {
    LOG_ERR(kLogTag, "route path did not fit: %s", entry.fileName);
    activityManager.goToMap();
    return;
  }
  LOG_INF(kLogTag, "loading route %s", path);
  // The map validates the file again when it loads it -- the header check this
  // screen did says the file is a route, not that its geometry is intact
  // (MapRouteReader.h, "Two checksums, at two different times"). A route that
  // fails there draws no route and says so, rather than drawing part of one.
  activityManager.goToMap(path);
}

void RouteSelectActivity::renderScreen() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ROUTE_SELECT),
                 tr(STR_ROUTE_SELECT_HINT));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing / 2;
  char status[112];
  if (entryCount_ == 0) {
    snprintf(status, sizeof(status), "%s", tr(STR_ROUTE_NONE_ON_CARD));
  } else if (foundCount_ > entryCount_) {
    // A truncated list must say so. Silently hiding a route the rider pushed is
    // exactly the kind of quiet lie the hatch rule exists to avoid.
    snprintf(status, sizeof(status), tr(STR_ROUTE_LIST_TRUNCATED), static_cast<int>(entryCount_),
             static_cast<int>(foundCount_));
  } else {
    snprintf(status, sizeof(status), tr(STR_ROUTE_COUNT), static_cast<int>(entryCount_));
  }
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, status, true);
  y += lineHeight;

  drawList();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void RouteSelectActivity::listRect(int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + lineHeight;

  x = metrics.contentSidePadding;
  w = pageWidth - metrics.contentSidePadding * 2;
  y = top;
  h = pageHeight - top - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
}

int RouteSelectActivity::visibleRowCount() const {
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) * 2 + kRowGap;
  const int fits = rowHeight > 0 ? lh / rowHeight : 0;
  return fits < 1 ? 1 : fits;
}

int RouteSelectActivity::firstVisibleRow() const {
  const int fits = visibleRowCount();
  const int rows = rowCount();
  if (rows <= fits) return 0;
  // Window follows the highlight, and keeps it off the very edge where possible
  // so there is always a row of context in the scroll direction.
  int first = selected_ - fits / 2;
  if (first < 0) first = 0;
  if (first > rows - fits) first = rows - fits;
  return first;
}

void RouteSelectActivity::drawList() {
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  renderer.fillRect(lx, ly, lw, lh, false);

  const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) * 2 + kRowGap;
  const int first = firstVisibleRow();
  const int fits = visibleRowCount();
  const int rows = rowCount();
  for (int i = first; i < rows && i < first + fits; ++i) {
    drawRow(i, ly + (i - first) * rowHeight, rowHeight);
  }
}

void RouteSelectActivity::drawRow(int index, int y, int rowHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int lx, ly, lw, lh;
  listRect(lx, ly, lw, lh);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  const bool highlighted = index == selected_;
  if (highlighted) {
    // Inverted block, the same way every other list on this device marks the
    // selection -- there is no colour to spend.
    renderer.fillRect(lx, y - 2, lw, rowHeight - kRowGap + 4, true);
  }
  const bool ink = !highlighted;

  char line[96];
  char detail[96];
  if (isSkipRow(index)) {
    snprintf(line, sizeof(line), "%s", tr(STR_ROUTE_SKIP));
    snprintf(detail, sizeof(detail), "%s", tr(STR_ROUTE_SKIP_DETAIL));
  } else {
    const MapRouteStore::Entry& entry = entries_[index - 1];
    snprintf(line, sizeof(line), "%s", entry.name);
    if (entry.valid) {
      snprintf(detail, sizeof(detail), tr(STR_ROUTE_POINTS), static_cast<int>(entry.pointCount));
    } else {
      snprintf(detail, sizeof(detail), "%s", tr(STR_ROUTE_UNREADABLE));
    }
  }

  renderer.drawText(UI_10_FONT_ID, lx + metrics.contentSidePadding / 2, y, line, ink, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, lx + metrics.contentSidePadding / 2, y + lineHeight, detail, ink);
}
