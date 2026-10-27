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

// Held Select opens the delete prompt. 700 ms, the same number the reader's
// bookmark list uses for the same gesture (EpubReaderBookmarksActivity,
// ENTER_DELETE_MODE_MS) -- one device, one hold. Copied rather than tuned: a
// second hold length on a second screen is a thing for a thumb to learn twice.
constexpr unsigned long kDeleteHoldMs = 700;

}  // namespace

RouteSelectActivity::RouteSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("RouteSelect", renderer, mappedInput) {}

void RouteSelectActivity::onEnter() {
  Activity::onEnter();

  entries_ = makeUniqueNoThrow<MapRouteStore::Entry[]>(MapRouteStore::kMaxRoutes);
  if (!entries_) {
    LOG_ERR(kLogTag, "OOM: %lu route entries", static_cast<unsigned long>(MapRouteStore::kMaxRoutes));
    // No list to choose from, so do not show an empty one. Straight to the map
    // rather than through the satellite wait like the rows below: this branch is
    // an allocation that already failed, and the wait screen is another one.
    activityManager.goToMap();
    return;
  }

  entryCount_ = MapRouteStore::list(entries_.get(), MapRouteStore::kMaxRoutes, foundCount_);
  // Highlight Continue rather than the first route: opening the map is the
  // default path, picking a route is the detour. A rider who wants a route
  // presses Down once; one who doesn't just presses Confirm.
  selected_ = 0;
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

  // The popup owns every button while it is up, and it redraws itself. Taken
  // first and returned from, the same shape the reader's bookmark list uses:
  // without it Up/Down below would also move the highlight under the prompt.
  if (confirmPopup_.handleInput(mappedInput, [this] { renderScreen(); })) return;
  if (confirming_) return;

  // Re-arm on the button being up, never on a timer: anything time-based would
  // re-open the prompt for a rider who simply keeps their thumb down.
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) deleteHoldArmed_ = true;

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
  // Held, not released: the delete prompt opens while the button is still down,
  // so the release that follows it belongs to the prompt rather than riding the
  // route. Short press is still "ride this one".
  if (deleteHoldArmed_ && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= kDeleteHoldMs) {
    deleteHoldArmed_ = false;
    askDeleteSelected();
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
    activityManager.goToGnssAcquire();
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
  activityManager.goToGnssAcquire(path);
}

void RouteSelectActivity::askDeleteSelected() {
  // Skip is not a file. Nothing to confirm and nothing to delete.
  if (isSkipRow(selected_)) return;

  const MapRouteStore::Entry& entry = entries_[selected_ - 1];
  LOG_INF(kLogTag, "asking to delete %s", entry.fileName);
  confirming_ = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  char prompt[128];
  snprintf(prompt, sizeof(prompt), tr(STR_ROUTE_DELETE_CONFIRM), entry.name);
  confirmPopup_.show(prompt, options, 2, 0, [this](int index) {
    confirming_ = false;
    if (index == 1) deleteSelected();
    renderScreen();
  });
  renderScreen();
}

void RouteSelectActivity::deleteSelected() {
  if (isSkipRow(selected_)) return;

  // Copied, not referenced: the re-list below overwrites entries_ underneath
  // this row, and the log line after it would then name whatever moved up into
  // the slot.
  char fileName[MapRouteStore::kMaxFileNameBytes];
  snprintf(fileName, sizeof(fileName), "%s", entries_[selected_ - 1].fileName);

  if (!MapRouteStore::remove(fileName)) {
    deleteFailed_ = true;
    return;
  }
  deleteFailed_ = false;

  // Re-read the card rather than shuffling the array: the file is gone, and the
  // card is what the next screen reads anyway. foundCount_ comes back with it,
  // so a list that was truncated stops saying so once it no longer is.
  entryCount_ = MapRouteStore::list(entries_.get(), MapRouteStore::kMaxRoutes, foundCount_);

  // Keep the highlight where the rider was looking. Past the end after deleting
  // the last row, so step back onto what is now the last one -- never onto Skip
  // unless Skip is all that is left.
  const int rows = rowCount();
  if (selected_ >= rows) selected_ = rows - 1;
  if (selected_ < 0) selected_ = 0;

  if (entryCount_ == 0) {
    // The card has no trips at all now, and this screen's own reason to exist
    // is gone with them -- ActivityManager::goToRouteSelect() would not have
    // opened it. The empty screen explains what to do next; a one-row list
    // whose only row is Skip explains nothing.
    LOG_INF(kLogTag, "last trip deleted, showing the empty screen");
    // Through goToRouteSelect() rather than naming RouteEmptyActivity here: it
    // already asks MapRouteStore::anyRoutes() and picks between the two, so the
    // decision stays in one place.
    activityManager.goToRouteSelect();
  }
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
  if (deleteFailed_) {
    // Louder than the count it replaces: the row is still on the list and a
    // silent re-render reads as "nothing happened".
    snprintf(status, sizeof(status), "%s", tr(STR_ROUTE_DELETE_FAILED));
  } else if (entryCount_ > 0) {
    // Held Select is the only way to delete and the four button hints are full,
    // so the status line says it. Appended to that line rather than given one
    // of its own: listRect() measures the list from a top that allows for
    // exactly one line here, and a second drawn line is simply overdrawn by the
    // list -- measured on a T5 S3 Pro, 2026-10-27, which is what the first
    // version of this screen did.
    const size_t used = strlen(status);
    snprintf(status + used, sizeof(status) - used, "  %s", tr(STR_ROUTE_DELETE_HINT));
  }
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, status, true);
  y += lineHeight;

  drawList();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // The prompt draws over the finished list, so the rider can still read which
  // route they are about to lose.
  if (confirmPopup_.processRender(renderer, mappedInput)) return;
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
