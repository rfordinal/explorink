#include "MappedInputManager.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "DebugInput.h"
#include "TouchPolicy.h"
#include "components/UITheme.h"

bool MappedInputManager::isNavDirectionSwapped() const {
  // Key the swap on the orientation the screen is *actually* rendered at, not the persisted reader
  // setting. The reader (and its modal menus) render rotated, so navigation/labels flip there; the
  // home and settings UI render in portrait, so they never flip even when a rotated reader is configured.
  const auto orientation = renderer.getOrientation();
  return SETTINGS.frontButtonFollowOrientation &&
         (orientation == GfxRenderer::PortraitInverted || orientation == GfxRenderer::LandscapeCounterClockwise);
}

MappedInputManager::Button MappedInputManager::mapScreenDirection(const Button button) const {
  // Rows follow GfxRenderer::Orientation's declared order.
  static constexpr Button directions[][4] = {
      {Button::Left, Button::Right, Button::Up, Button::Down},
      {Button::Down, Button::Up, Button::Left, Button::Right},
      {Button::Right, Button::Left, Button::Down, Button::Up},
      {Button::Up, Button::Down, Button::Right, Button::Left},
  };

  uint8_t direction = 0;
  switch (button) {
    case Button::ScreenLeft:
      direction = 0;
      break;
    case Button::ScreenRight:
      direction = 1;
      break;
    case Button::ScreenUp:
      direction = 2;
      break;
    case Button::ScreenDown:
      direction = 3;
      break;
    default:
      return button;
  }

  const uint8_t orientation =
      SETTINGS.frontButtonFollowOrientation ? static_cast<uint8_t>(renderer.getOrientation()) : 0;
  return directions[orientation][direction];
}

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return rawButton(SETTINGS.frontButtonBack, fn);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return rawButton(SETTINGS.frontButtonConfirm, fn);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return rawButton(SETTINGS.frontButtonLeft, fn);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return rawButton(SETTINGS.frontButtonRight, fn);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return rawButton(HalGPIO::BTN_UP, fn);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return rawButton(HalGPIO::BTN_DOWN, fn);
    case Button::Power:
      // Power button bypasses remapping.
      return rawButton(HalGPIO::BTN_POWER, fn);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return rawButton(HalGPIO::BTN_UP, fn);
        case CrossPointSettings::NEXT_PREV:
          return rawButton(HalGPIO::BTN_DOWN, fn);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return rawButton(HalGPIO::BTN_DOWN, fn);
        case CrossPointSettings::NEXT_PREV:
          return rawButton(HalGPIO::BTN_UP, fn);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::NavNext:
      // Logical "next item" navigation: side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW (frontButtonFollowOrientation) so it matches the rotated hint labels.
      return isNavDirectionSwapped() ? (mapButton(Button::Up, fn) || mapButton(Button::Left, fn))
                                     : (mapButton(Button::Down, fn) || mapButton(Button::Right, fn));
    case Button::NavPrevious:
      // Logical "previous item" navigation: side Up + front Left, axis-flipped in the same orientations.
      return isNavDirectionSwapped() ? (mapButton(Button::Down, fn) || mapButton(Button::Right, fn))
                                     : (mapButton(Button::Up, fn) || mapButton(Button::Left, fn));
    case Button::ScreenLeft:
    case Button::ScreenRight:
    case Button::ScreenUp:
    case Button::ScreenDown:
      return mapButton(mapScreenDirection(button), fn);
  }

  return false;
}

namespace {
constexpr float LEFT_EDGE_BACK_GESTURE_FRAC_X = 0.25f;
constexpr float BOTTOM_EDGE_BACK_GESTURE_FRAC_Y = 0.14f;
constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.14f;
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
// How long the home key's single tap waits to find out whether a second one is
// coming. 300 ms was too short on hardware: a deliberate double tap regularly
// landed outside it and read as two separate selects. The GT911 reports this key
// only on a fresh touch frame (InputManager::pollGt911, the 0x80 gate), so the
// second tap is seen later than the finger made it.
constexpr unsigned long HOME_KEY_DOUBLE_TAP_WINDOW_MS = 500;
// After a gesture resolves, ignore the key for this long.
//
// What was MEASURED (2026-09-05): one physical double tap produced a lock, a
// Select and a frontlight toggle. Three tap events is the INFERRED explanation,
// not an observation -- nobody logged the events, and whether the extra ones are
// contact bounce or stale GT911 frames is open (docs/input-gestures.md). That
// question decides whether the real answer is a minimum press width or rejecting
// stale frames; this window is a filter over a noisy stream either way.
constexpr unsigned long HOME_KEY_REFRACTORY_MS = 500;
}  // namespace

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

void MappedInputManager::update() const {
  gpio.update();
  ensureHintTouchPumped();
}

void MappedInputManager::ensureHintTouchPumped() const {
  const uint32_t seq = gpio.updateSequence();
  if (seq == hintPumpedSeq) return;
  hintPumpedSeq = seq;
  pumpHintTouch();
  pumpHomeKey();
}

void MappedInputManager::pumpHomeKey() const {
  homeConfirmResolved = false;
  homeDoubleTapResolved = false;
  homeLongResolved = false;

  const unsigned long now = millis();

  // A press edge means a new hold has begun, so whatever was made of the last
  // one no longer applies.
  if (gpio.wasHomeKeyPressed()) homeTapConsumedSinceDown = false;

  if (!TouchPolicy::homeKeyDoubleTapLocksTouch()) {
    homeTapPendingSince = 0;
    homeLongResolved = gpio.wasHomeKeyLongPressed();
    return;
  }

  // The hold, filtered. The SDK fires it from a latched down-state read BEFORE
  // its fresh-frame gate (InputManager::pollGt911), on purpose -- a motionless
  // hold stops producing frames, so the timer could not run otherwise. The cost
  // is that a MISSED release edge leaves that state latched, and the hold then
  // fires from a press this layer already turned into a tap. Measured on
  // hardware: one double tap locked the panel, selected, and then lit the
  // frontlight seconds later when a map render let polling resume.
  //
  // So a hold is only believed while no tap has been made of the current press.
  if (gpio.wasHomeKeyLongPressed()) {
    homeTapPendingSince = 0;
    if (!homeTapConsumedSinceDown) {
      homeLongResolved = true;
      homeRefractoryUntil = now + HOME_KEY_REFRACTORY_MS;
    }
    return;
  }

  if (gpio.wasHomeKeyTapped()) {
    homeTapConsumedSinceDown = true;
    // Inside the refractory window this is the tail of a gesture already
    // resolved, not a new one.
    if (homeRefractoryUntil != 0 && static_cast<long>(now - homeRefractoryUntil) < 0) {
      homeTapPendingSince = 0;
      return;
    }
    if (homeTapPendingSince != 0) {
      // Second tap inside the window: the lock is what was asked for, and the
      // held Confirm is dropped rather than fired first.
      homeTapPendingSince = 0;
      homeDoubleTapResolved = true;
      homeRefractoryUntil = now + HOME_KEY_REFRACTORY_MS;
    } else {
      homeTapPendingSince = now;
      // millis() can be 0 for one tick after boot, and 0 is this field's "no tap
      // waiting". One tick later is close enough and keeps the sentinel honest.
      if (homeTapPendingSince == 0) homeTapPendingSince = 1;
    }
    return;
  }

  // Nothing arrived: the window decides. Timed off whatever query the activity
  // makes this frame, which is every loop in practice -- an activity that asked
  // for no input at all would hold the Confirm a little longer, and would also
  // have nothing to do with it.
  if (homeTapPendingSince != 0 && now - homeTapPendingSince >= HOME_KEY_DOUBLE_TAP_WINDOW_MS) {
    homeTapPendingSince = 0;
    // A locked screen must not select. The tap still had to be held for the
    // window -- a second one inside it is the unlock and that path is above --
    // but once it resolves as a single tap on a locked panel it means nothing.
    // Locking is the rider saying "ignore what I touch", and the key is the one
    // control that is still listened to, for exactly one thing.
    homeConfirmResolved = !TouchPolicy::locked();
    homeRefractoryUntil = now + HOME_KEY_REFRACTORY_MS;
  }
}

void MappedInputManager::tapToPortrait(const float nx, const float ny, int& x, int& y) const {
  const int panelWidth = renderer.getDisplayWidth();
  const int panelHeight = renderer.getDisplayHeight();
  int physicalX = static_cast<int>(nx * panelWidth);
  int physicalY = static_cast<int>(ny * panelHeight);
  physicalX = std::min(std::max(physicalX, 0), panelWidth - 1);
  physicalY = std::min(std::max(physicalY, 0), panelHeight - 1);
  // Same transform GfxRenderer applies for Orientation::Portrait.
  x = panelHeight - 1 - physicalY;
  y = physicalX;
}

bool MappedInputManager::hintBoxAt(const int px, const int py, uint8_t& hwButton) const {
  const auto& theme = UITheme::getInstance().getTheme();
  // Portrait logical size: the renderer's short side is the portrait width.
  const int portraitWidth = renderer.getDisplayHeight();
  const int portraitHeight = renderer.getDisplayWidth();
  const auto inside = [px, py](const Rect& r) {
    return px >= r.x && px < r.x + r.width && py >= r.y && py < r.y + r.height;
  };
  Rect box;
  // Front box index is the hardware button index: both are Back, Confirm, Left,
  // Right in that order, which is also the order the labels are handed to
  // drawButtonHints() by mapFrontLabels().
  static_assert(
      HalGPIO::BTN_BACK == 0 && HalGPIO::BTN_CONFIRM == 1 && HalGPIO::BTN_LEFT == 2 && HalGPIO::BTN_RIGHT == 3,
      "hint box order must match the front button indices");
  for (int i = 0; i < 4; i++) {
    if (theme.frontHintBox(i, portraitWidth, portraitHeight, box) && inside(box)) {
      hwButton = static_cast<uint8_t>(i);
      return true;
    }
  }
  for (int i = 0; i < 2; i++) {
    if (theme.sideHintBox(i, portraitWidth, portraitHeight, box) && inside(box)) {
      hwButton = (i == 0) ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
      return true;
    }
  }
  return false;
}

void MappedInputManager::pumpHintTouch() const {
  hintPressedButton = kNoHintButton;
  hintReleasedButton = kNoHintButton;
  // The held-time override answers with the last TOUCH's duration for 250 ms
  // (getHeldTime()). A hardware button going down in that window starts an input
  // it knows nothing about, and while that button is held there are no more
  // edges to stop it answering -- ButtonNavigator then read a slow screen tap as
  // an instant half-second hold on the key. A press ends the override's claim.
  if (gpio.wasAnyPressed()) touchHeldOverrideValid = false;

  if (!TouchPolicy::touchHintBoxes()) {
    hintDownButton = kNoHintButton;
    return;
  }

  float nx = 0.0f;
  float ny = 0.0f;
  int px = 0;
  int py = 0;
  uint8_t hit = kNoHintButton;

  if (gpio.wasTouchDown(nx, ny)) {
    tapToPortrait(nx, ny, px, py);
    if (hintBoxAt(px, py, hit)) {
      hintDownButton = hit;
      hintPressedButton = hit;
      hintDownAtMs = millis();
    }
  }

  if (gpio.wasTouchTap(nx, ny)) {
    tapToPortrait(nx, ny, px, py);
    // Release only counts on the box it started on, the way a physical button
    // does not fire when the finger slides off it.
    if (hintBoxAt(px, py, hit) && hit == hintDownButton) {
      hintReleasedButton = hit;
      // Feeds getHeldTime(), so a long press on a box behaves like a long press
      // on the key it stands for.
      rememberTouchHeldTime();
    }
    hintDownButton = kNoHintButton;
  } else if (hintDownButton != kNoHintButton) {
    if (!gpio.isTouchHeldAt(nx, ny)) {
      // Lifted without producing a tap (moved past the tap slop). Without this
      // the button would stay held for good.
      hintDownButton = kNoHintButton;
    } else {
      // Still on the glass -- but a finger dragged OFF the box is no longer
      // pressing it. isTouchHeldAt() has no slop gate of its own
      // (InputManager::isTouchHeldAt), so without this check the button stayed
      // held wherever the finger went, and ButtonNavigator's continuous step
      // kept scrolling from a box the finger had left.
      tapToPortrait(nx, ny, px, py);
      uint8_t stillOn = kNoHintButton;
      if (!hintBoxAt(px, py, stillOn) || stillOn != hintDownButton) {
        // A cancel, not a release: nothing is emitted, the way a finger slid off
        // a physical key does not press it.
        hintDownButton = kNoHintButton;
      }
    }
  }
}

bool MappedInputManager::hintButton(const uint8_t index, bool (HalGPIO::*fn)(uint8_t) const) const {
  if (fn == &HalGPIO::wasPressed) return hintPressedButton == index;
  if (fn == &HalGPIO::wasReleased) return hintReleasedButton == index;
  if (fn == &HalGPIO::isPressed) return hintDownButton == index;
  return false;
}

namespace {

// The same fn-pointer dispatch hintButton() does, for the serial console's
// injected presses (DebugInput.h). Both synthetic sources sit in front of the
// real read, so an activity cannot tell them from a thumb.
bool injectedButton(const uint8_t index, bool (HalGPIO::*fn)(uint8_t) const) {
  if (fn == &HalGPIO::wasPressed) return DebugInput::button(index, DebugInput::Query::Pressed);
  if (fn == &HalGPIO::wasReleased) return DebugInput::button(index, DebugInput::Query::Released);
  if (fn == &HalGPIO::isPressed) return DebugInput::button(index, DebugInput::Query::Down);
  return false;
}

}  // namespace

bool MappedInputManager::rawButton(const uint8_t index, bool (HalGPIO::*fn)(uint8_t) const) const {
  ensureHintTouchPumped();
  return hintButton(index, fn) || injectedButton(index, fn) || (gpio.*fn)(index);
}

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  if (!TouchPolicy::touchAnywhere()) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  if (!TouchPolicy::touchAnywhere()) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  if (!TouchPolicy::touchAnywhere()) return false;
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
  (void)x;
  if (itemCount <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& theme = UITheme::getInstance().getTheme();
  const int rowStep = theme.getListRowStep(hasSubtitle);
  if (rowStep <= 0) return false;

  const int pageItems = theme.getListPageItems(listHeight, hasSubtitle);
  if (pageItems <= 0) return false;
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  if (!TouchPolicy::touchAnywhere()) return false;
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) {
    return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  }
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
}

bool MappedInputManager::wasBackGesture() const {
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = sx <= renderer.getScreenWidth() * LEFT_EDGE_BACK_GESTURE_FRAC_X && ex > sx &&
                   std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasMenuGesture() const {
  // Downward swipe starting at the top edge (mirror of the bottom-edge home gesture).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const bool hit = sy <= topEdgeBottom && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasHomeGesture() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (decodeSwipe(sx, sy, ex, ey)) {
    const int bottomEdgeTop =
        renderer.getScreenHeight() - static_cast<int>(renderer.getScreenHeight() * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
    if (sy >= bottomEdgeTop && ey < sy && std::abs(ey - sy) > std::abs(ex - sx)) {
      rememberTouchHeldTime();
      return true;
    }
  }
  return false;
}

// A tap on the capacitive home key below the panel is Select, the same as a tap
// on the board's physical user button. Synthesised here the way a bottom-edge
// swipe becomes Back below: an activity asks for Confirm and never has to know
// which piece of hardware produced it.
//
// The key reports its tap only on RELEASE, and only when the 700 ms hold
// threshold was not crossed (InputManager::serviceTouch). That is what lets the
// same key carry the frontlight hold in main.cpp without ever selecting on the
// way there. Boards with no home key never see this: the SDK leaves the event
// false.
bool MappedInputManager::wasHomeKeyConfirm() const {
  ensureHintTouchPumped();
  // Where the key also carries a double tap, Confirm is the *resolved* single
  // tap -- held for the window, then fired only if no second tap came. Firing on
  // arrival would have selected whatever the cursor was on before the double tap
  // could mean the lock instead.
  if (TouchPolicy::homeKeyDoubleTapLocksTouch()) return homeConfirmResolved;
  return gpio.wasHomeKeyTapped();
}

bool MappedInputManager::wasHomeKeyDoubleTap() const {
  ensureHintTouchPumped();
  return homeDoubleTapResolved;
}

bool MappedInputManager::wasHomeKeyLongPress() const {
  ensureHintTouchPumped();
  return homeLongResolved;
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  if (button == Button::Confirm && wasHomeKeyConfirm()) return true;
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  if (button == Button::Back && wasBackGesture()) return true;
  if (button == Button::Confirm && wasHomeKeyConfirm()) return true;
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

// A hint-box tap is a button press as far as anything asking "did the rider do
// something" is concerned. Without this a screen driven only by the boxes looked
// idle: the map's own "the rider is looking at the screen" test and the remap
// capture below both went through here and saw nothing. An injected press
// (DebugInput.h) is the same kind of synthetic input and counts for the same
// reason.
//
// main.cpp's sleep timer deliberately does NOT come through here -- it reads
// HalGPIO plus wasTouchActivity(), which already counts the touch that made this
// press, and counting it twice would say nothing new. It reads DebugInput
// itself, because nothing else there can see an injected press.
bool MappedInputManager::wasAnyPressed() const {
  ensureHintTouchPumped();
  return gpio.wasAnyPressed() || hintPressedButton != kNoHintButton || DebugInput::anyEdge(DebugInput::Query::Pressed);
}

bool MappedInputManager::wasAnyReleased() const {
  ensureHintTouchPumped();
  return gpio.wasAnyReleased() || hintReleasedButton != kNoHintButton ||
         DebugInput::anyEdge(DebugInput::Query::Released);
}

unsigned long MappedInputManager::getHeldTime() const {
  ensureHintTouchPumped();
  // A finger on a hint box is the input being held right now, and it is the only
  // thing that can answer for itself: HalGPIO tracks hardware presses only.
  // Without this a tap reads as however long the last hardware press lasted.
  if (hintDownButton != kNoHintButton) return millis() - hintDownAtMs;
  // Same for an injected press, release frame included: every long-press path in
  // the UI is `isPressed(X) && getHeldTime() >= N`, and the real hardware time
  // under it is a stale zero.
  if (DebugInput::active()) return DebugInput::heldMs();
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels = isNavDirectionSwapped();
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  return mapFrontLabels(back, confirm, leftLabel, rightLabel);
}

MappedInputManager::Labels MappedInputManager::mapDirectionalLabels(const char* back, const char* confirm,
                                                                    const char* left, const char* right, const char* up,
                                                                    const char* down) const {
  const auto labelForButton = [&](const Button rawButton) {
    if (mapScreenDirection(Button::ScreenLeft) == rawButton) return left;
    if (mapScreenDirection(Button::ScreenRight) == rawButton) return right;
    if (mapScreenDirection(Button::ScreenUp) == rawButton) return up;
    if (mapScreenDirection(Button::ScreenDown) == rawButton) return down;
    return "";
  };
  return mapFrontLabels(back, confirm, labelForButton(Button::Left), labelForButton(Button::Right));
}

MappedInputManager::Labels MappedInputManager::mapFrontLabels(const char* back, const char* confirm, const char* left,
                                                              const char* right) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return left;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return right;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  ensureHintTouchPumped();
  // A tap on a hint box is a press of the button that box stands for, so the
  // remap screen can be driven by touch on a board with no front keys -- it was
  // unusable there otherwise. Front boxes only: the index this returns is a
  // front button index and the side boxes are not among them.
  if (hintPressedButton <= HalGPIO::BTN_RIGHT) {
    return static_cast<int>(hintPressedButton);
  }
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
