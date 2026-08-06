#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>

#include "MapRouteStore.h"
#include "activities/Activity.h"

// Asks which route to load, on the way into the map. One row per `.tir` file on
// the card, plus **Skip**, which opens the map with no route -- exactly what the
// map did before routes existed.
//
// ## Why a screen and not a map-menu item
//
// The rider is choosing what the map is *for* before it draws. Putting it behind
// CONFIRM would mean the map first draws a frame with no route, then redraws the
// whole panel with one -- two full refreshes, about four seconds, for a choice
// that was already made walking out of the door. The map screen's button budget
// is also exactly full (docs/architecture-plan.md), and this needs a list with
// scrolling, which CONFIRM's flat OptionPopup cannot give.
//
// The same reasoning that put the tile fetch on its own screen
// (TileSyncActivity.h): preparation is a separate screen, riding is the map.
//
// ## Skipped entirely when there is nothing to pick
//
// ActivityManager::goToRouteSelect() checks MapRouteStore::anyRoutes() first and
// goes straight to the map when the card has no routes. A one-row list whose only
// row is Skip is a screen that exists to be dismissed.
//
// `CMD:GOTO_MAP` over serial still goes straight to the map with no route, so
// the screenshot and preview tooling in the parent repo is unaffected.
//
// ## Rows are read from headers, not file names
//
// Each row shows the route's own name out of its header plus its point count
// (MapRouteStore.h). That costs about 100 bytes a file -- the `.tir` format
// checks its point array separately, so listing two dozen routes does not read
// two dozen route geometries.
//
// A file whose header is refused is still listed, marked unreadable. A route the
// rider pushed and cannot see at all is worse than one they can see is broken.
class RouteSelectActivity final : public Activity {
 public:
  RouteSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void renderScreen();
  void drawList();
  void drawRow(int index, int y, int rowHeight);
  void listRect(int& x, int& y, int& w, int& h) const;
  int visibleRowCount() const;
  int firstVisibleRow() const;
  // Row 0 is Skip, so a route's index into entries_ is one less than its row.
  int rowCount() const { return static_cast<int>(entryCount_) + 1; }
  bool isSkipRow(int row) const { return row == 0; }
  // Opens the map with the selected route, or with none for the Skip row.
  void chooseSelected();

  // Heap, not a member array: 24 entries is 2.8 KB and this screen is up for a
  // few seconds at the start of a ride. Freed in onExit(), same bracket as
  // TileSyncActivity's own row snapshot.
  std::unique_ptr<MapRouteStore::Entry[]> entries_;
  uint32_t entryCount_ = 0;
  // How many .tir files were on the card. Larger than entryCount_ when the cap
  // truncated the list, which the screen then says out loud rather than hiding.
  uint32_t foundCount_ = 0;
  int selected_ = 0;
};
