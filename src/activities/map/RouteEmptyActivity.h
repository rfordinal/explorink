#pragma once

#include <I18n.h>

#include <cstdint>

#include "activities/Activity.h"

// Shown instead of RouteSelectActivity when the card carries no `.tir` files
// (`MapRouteStore::anyRoutes()` false). Before this screen existed,
// ActivityManager::goToRouteSelect() went straight to the map with nothing
// said -- which reads as broken, not as empty: a rider who expected the trip
// they pushed to be there sees the same map either way. This explains once and
// asks once.
//
// Same two ways out as RouteSelectActivity's own Skip row and Back key:
// **Continue to map** opens the map with no route, exactly what Skip does
// there. **Back** goes home, not into a route list that would have nothing to
// show -- same reasoning as RouteSelectActivity::loop()'s own Back handling.
//
// The ridge (src/images/Mountains.h, scripts/gen_mountains.py) and the
// backpack (components/icons/home_icons.h's icon_trips, the same glyph the
// home menu's Trips row already uses) are decoration, not new information --
// this screen would say the same thing with plain text. They exist so the
// "nothing to pick" screen does not read as an error page.
class RouteEmptyActivity final : public Activity {
 public:
  RouteEmptyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;

 private:
  enum class Action : uint8_t { Continue = 0, Back = 1 };
  static constexpr int kActionCount = 2;

  void renderScreen();
  void drawRidge();
  void drawActions();
  void actionRect(int index, int& x, int& y, int& w, int& h) const;
  void activate(Action action);

  Action selected_ = Action::Continue;
};
