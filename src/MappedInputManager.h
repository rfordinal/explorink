#pragma once

#include <HalGPIO.h>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button {
    Back,
    Confirm,
    Left,
    Right,
    Up,
    Down,
    Power,
    PageBack,
    PageForward,
    NavNext,
    NavPrevious,
    ScreenLeft,
    ScreenRight,
    ScreenUp,
    ScreenDown
  };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  void update() const;
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  bool wasTapInRect(int x, int y, int width, int height) const;
  bool wasListItemTapped(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  bool wasListItemTouchedDown(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                              bool hasSubtitle) const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  bool wasHomeGesture() const;
  // True on the frame the capacitive home key (boards that have one) completed a
  // short tap. Reported as Confirm by wasPressed()/wasReleased(); exposed so a
  // caller that needs to tell the two apart still can.
  bool wasHomeKeyConfirm() const;
  // True on the frame a double tap on the capacitive home key resolves. Only on
  // a board where that gesture means something (TouchPolicy::homeKeyDoubleTapLocksTouch());
  // elsewhere the key has no double tap and this is always false.
  bool wasHomeKeyDoubleTap() const;
  // The home key's hold, filtered. Not gpio.wasHomeKeyLongPressed() directly:
  // that event can arrive from a press this layer has already resolved as a tap.
  bool wasHomeKeyLongPress() const;
  bool wasMenuGesture() const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  const GfxRenderer& getRenderer() const { return renderer; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Maps four screen-direction labels onto the two physical front-button roles
  // using the same live-orientation transform as ScreenLeft/Right/Up/Down.
  Labels mapDirectionalLabels(const char* back, const char* confirm, const char* left, const char* right,
                              const char* up, const char* down) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the control axis is flipped relative to the physical buttons: the user opted into
  // orientation-following front buttons AND the screen is *currently rendered* rotated (INVERTED /
  // LANDSCAPE_CCW). Keyed on the live renderer orientation rather than the persisted reader setting,
  // so portrait UI (home, settings) never swaps while the reader and its menus do.
  [[nodiscard]] bool isNavDirectionSwapped() const;

 private:
  HalGPIO& gpio;
  // Logical-to-physical button mapping depends on what the user is actually looking at: when the
  // screen is rendered rotated, the directional buttons must flip to match. The renderer is the only
  // authority on the *live* orientation (the reader rotates it and restores portrait on exit), so we
  // read it here instead of CrossPointSettings.orientation, which is just the persisted reader
  // preference and stays "rotated" even while portrait UI like home/settings is on screen.
  const GfxRenderer& renderer;

  Button mapScreenDirection(Button button) const;
  // A tap on a hint box acts as its hardware button, so every hardware read goes
  // through here rather than straight to HalGPIO.
  bool rawButton(uint8_t index, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool hintButton(uint8_t index, bool (HalGPIO::*fn)(uint8_t) const) const;
  // Turn this frame's touch into hint-box button edges. No-op outside BUTTONS mode.
  //
  // Driven lazily off HalGPIO's frame counter rather than from update(), because
  // the frame tick is a bare gpio.update() in loop() (main.cpp) that never passes
  // through this class -- hooking update() alone left the boxes drawn and dead.
  void ensureHintTouchPumped() const;
  // Resolve this frame's home-key gesture. Runs from the same per-frame pump as
  // the hint boxes, so the pending single tap is timed out by whatever query the
  // activity makes rather than by a tick of its own.
  void pumpHomeKey() const;
  void pumpHintTouch() const;
  bool hintBoxAt(int px, int py, uint8_t& hwButton) const;
  // Normalized touch to *portrait* logical coordinates. The renderer's own
  // tapToLogical() maps to the orientation currently being drawn, which the
  // reader rotates; the hint boxes are always painted in portrait, so the hit
  // test has to be done there too.
  void tapToPortrait(float nx, float ny, int& x, int& y) const;
  Labels mapFrontLabels(const char* back, const char* confirm, const char* left, const char* right) const;
  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  bool wasBackGesture() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  bool listItemFromPoint(int x, int y, int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  void rememberTouchHeldTime() const;

  static constexpr uint8_t kNoHintButton = 0xFF;
  // Hint-box button edges for this frame: pressed and released last one frame,
  // down persists while the finger stays on the box.
  mutable uint32_t hintPumpedSeq = 0xFFFFFFFFu;
  // When the finger landed on the box currently held. getHeldTime() reports the
  // live duration from this, because HalGPIO's own answer describes the last
  // HARDWARE press and nothing else -- with no button down it returns the length
  // of the previous one, which is how a 600 ms frontlight hold left every later
  // box tap looking like a half-second hold and firing ButtonNavigator's
  // continuous step on top of its press step.
  mutable unsigned long hintDownAtMs = 0;

  // The home key's single tap cannot fire Confirm the moment it lands: a second
  // tap inside the window means the rider asked for the touch lock instead, and
  // selecting first would have activated whatever the cursor was on before
  // locking the panel. So the tap is held, and resolves either way.
  //
  // Only on a board whose home key carries the double tap at all -- elsewhere
  // the tap is Confirm the instant it arrives, with no window and no latency.
  mutable unsigned long homeTapPendingSince = 0;  // 0 = no tap waiting
  mutable bool homeConfirmResolved = false;       // this frame: the single tap won
  mutable bool homeDoubleTapResolved = false;     // this frame: the second tap won
  mutable bool homeLongResolved = false;          // this frame: a hold this layer believes in
  // Ignore further taps until this time. Measured: one physical double tap
  // produced a lock, a Select and a frontlight toggle. That extra tap events
  // caused it is inferred, not observed -- see pumpHomeKey() and
  // docs/input-gestures.md.
  mutable unsigned long homeRefractoryUntil = 0;
  // Whether a tap has already been made of the press the key is currently
  // holding. The SDK fires its hold from a latched down-state that survives a
  // missed release edge, so a hold can arrive seconds after the gesture it
  // belongs to was already spent -- see pumpHomeKey().
  mutable bool homeTapConsumedSinceDown = false;
  mutable uint8_t hintDownButton = kNoHintButton;
  mutable uint8_t hintPressedButton = kNoHintButton;
  mutable uint8_t hintReleasedButton = kNoHintButton;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
};
