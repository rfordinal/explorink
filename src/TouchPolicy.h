#pragma once

#include <BoardConfig.h>
#include <HalGPIO.h>

#include "CrossPointSettings.h"

// One place that answers "may this touch do anything, and are the hint boxes on
// screen". Before this existed, gpio.hasTouch() answered both questions at once:
// a board with a digitizer got no hint boxes and a fully live screen, and there
// was no way to ask for one without the other. Themes and MappedInputManager
// now ask here instead, so the three modes are decided once rather than in
// thirty call sites.
//
// hasTouch() is read live rather than cached: the touch controller finishes its
// init after static construction, so the flag flips once during boot.
namespace TouchPolicy {

inline bool panelPresent() { return gpio.hasTouch(); }

// The mode in force, which is not the same thing as the stored preference: the
// lock is a separate flag and it wins while it is set. Anything asking "what may
// touch do right now" wants this; only the Settings row reads SETTINGS.touchMode.
inline CrossPointSettings::TOUCH_MODE mode() {
  if (SETTINGS.touchLocked != 0) return CrossPointSettings::TOUCH_DISABLED;
  const uint8_t stored = SETTINGS.touchMode;
  // DISABLED included: it is never a legal stored value, so a settings file
  // carrying it (an older build wrote one) falls back rather than locking a
  // device whose owner cannot unlock it from the screen.
  if (stored >= CrossPointSettings::TOUCH_DISABLED) return CrossPointSettings::TOUCH_ANYWHERE;
  return static_cast<CrossPointSettings::TOUCH_MODE>(stored);
}

// True while the panel is locked. Toggled by the gesture that owns the lock on
// this board; nothing else may set it, and Settings does not offer it.
inline bool locked() { return panelPresent() && SETTINGS.touchLocked != 0; }

// The whole screen is live: list rows, swipes, edge gestures, map panning.
inline bool touchAnywhere() { return panelPresent() && mode() == CrossPointSettings::TOUCH_ANYWHERE; }

// Only the hint boxes are live, and a tap on one acts as its hardware button.
inline bool touchHintBoxes() { return panelPresent() && mode() == CrossPointSettings::TOUCH_BUTTONS_ONLY; }

// Any touch at all reaches the UI. False in OFF, which is also why a stray
// touch must not count as user activity for the sleep timer.
inline bool touchActive() { return panelPresent() && mode() != CrossPointSettings::TOUCH_DISABLED; }

// Whether the six hint boxes are drawn, and whether the layout must reserve
// room for them. Always true on a board without a digitizer -- an X4 labels its
// physical keys this way and has no touch policy to speak of.
//
// On a touch board only BUTTONS draws them, and that is the whole feedback for
// which mode is on: boxes on screen means the boxes are live. Drawing them in
// OFF as well would put six buttons on the glass that do nothing, and on a board
// with no keys under them the labels would name keys that are not there.
inline bool hintsVisible() { return !panelPresent() || mode() == CrossPointSettings::TOUCH_BUTTONS_ONLY; }

// Whether the capacitive home key carries a double tap, and what it means.
//
// On the LilyGo T5 S3 Pro a double tap is the touch lock -- nothing else on that
// board can stop the glass reacting. The single tap stays Confirm there, which
// costs it the double-tap window in latency (MappedInputManager::pumpHomeKey()).
// Everywhere else the key has no double tap and its single tap is Confirm the
// instant it lands.
//
// A board constant rather than a setting, because it decides what a physical key
// means and the answer differs by hardware, not by preference.
// Whether the capacitive home key's double tap is the lock gesture. Two
// conditions, and neither is a board name: there has to be a home key to double
// tap, and a digitizer worth locking. It was `#if FREEINK_DEVICE_LILYGO` until
// 2026-09-09, which shut the gesture out of the X4 Pro -- a board with both, and
// the reference device.
//
// Not constexpr, and not cacheable: `panelPresent()` reads `hasTouch()` live
// because the touch controller finishes its init after static construction, so
// this answers false for the first part of boot and true afterwards. Every
// caller must therefore tolerate the flip. `pumpHomeKey()` does: the branch this
// picks clears the pending-tap state on the way through, so a flip starts the
// gesture fresh rather than half-resolved.
//
// The gesture is load-bearing on a board whose Back and Confirm both come from
// touch. While the lock is on, a single tap deliberately does NOT select
// (`MappedInputManager::pumpHomeKey()`), so the double tap is the *only* way
// back -- which is why it is resolved before that gate and not after it.
inline bool homeKeyDoubleTapLocksTouch() { return panelPresent() && BoardConfig::hasHomeKey(); }

// Whether to draw the lock glyph instead of the hint boxes. The boxes vanishing
// is the signal that touch is off, but on their own they cannot distinguish OFF
// from ANYWHERE, which also draws none.
inline bool lockIndicator() { return panelPresent() && mode() == CrossPointSettings::TOUCH_DISABLED; }

}  // namespace TouchPolicy
