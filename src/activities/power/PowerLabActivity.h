#pragma once

// Build-flag gated, like the grayscale bench: OFF unless -DENABLE_POWER_LAB=1.
// Without it this header declares nothing and the .cpp compiles to nothing.
//
// The flag lives in its own PlatformIO environment (`env:powerlab`), not in
// `default` and not in `platformio.local.ini`. Two reasons. A lab screen has no
// business on a rider's build for the same reason the preview bench does not
// (platformio.ini says it: a menu item on a handlebar is something to open by
// mistake at 90 km/h). And the power campaign's frozen baseline has to be
// rebuildable from git alone (docs/power-plan.md, "The frozen baseline",
// condition 2) -- a build flag that only exists in a gitignored file is not.
#if defined(ENABLE_POWER_LAB) && ENABLE_POWER_LAB

#include <cstdint>

#include "activities/Activity.h"

// The power campaign's instrument screen: enter one power state deliberately,
// hold it, and name it in power.csv. Spec and rationale:
// docs/power-idle-sleep.md, "The power lab screen".
//
// The point is not fewer confounders (though it removes several -- no tile
// reads, no MapFollow refreshes, no transfer receiver, no autosync). The point
// is that **the binary stays identical across states**. Every comparison this
// campaign has made so far compared two builds as well as two states, so it
// rested on the two builds differing only where the author thought. Here the
// state is a runtime choice, so what a run measures is the state.
//
// Four states, chosen because each one prices something the campaign has an
// open question about:
//
// | Label      | Radio | Clock  | What it answers                              |
// |------------|-------|--------|----------------------------------------------|
// | idle-160   | down  | 160    | the plain full-clock floor, nothing running  |
// | idle-10    | down  | 10     | the 10 MHz floor -- legal only with BLE down |
// | radio-160  | up    | 160    | what the map screen does today               |
// | radio-80   | up    | 80     | route A's floor, built and never measured    |
//
// `radio-80` is the interesting one. HalPowerManager::lowPowerFloorMhz() asks
// the BT controller and returns BLE_SAFE_FREQ (80 MHz) whenever it is enabled,
// so this state is safe by construction rather than by anyone remembering: 80
// and 160 MHz are both PLL-sourced and both leave APB at 80 MHz, which is the
// requirement 10 MHz violates (docs/power-management.md, "Why 10 MHz breaks
// BLE").
//
// What it deliberately does NOT have: the deep-sleep-with-the-latch-held state.
// That is experiment 1, it needs a meter to be worth entering, and its safety
// rules (GPIO13 driven HIGH, a timer wake always armed, the pad reconfigured
// before gpio_hold_dis()) are a separate piece of work with a real brownout
// risk on the non-self-latching field revision. Adding it half-done to a screen
// that otherwise cannot hurt anything would be the wrong trade.
//
// Paints once per state change and never again. No clock, no battery icon, no
// periodic redraw -- e-ink holds the image for free, and a redraw is exactly the
// kind of work a power measurement must not contain.
class PowerLabActivity final : public Activity {
 public:
  explicit PowerLabActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerLab", renderer, mappedInput) {}

  // Index order matches kStates in the .cpp.
  enum class State : uint8_t { IdleFull, IdleThrottled, RadioFull, RadioThrottled, Count };

  // Enter a state without touching a button. The bench needs this: a button
  // press restores the full clock and resets the inactivity timer
  // (src/main.cpp:836), so pressing one is itself a change to the thing being
  // measured. Reached from CMD:POWERLAB_STATE.
  static bool selectByName(const char* name);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Never let a run end because nobody pressed anything.
  bool preventAutoSleep() override { return true; }
  // The whole point of two of the four states is that the throttle is allowed
  // to engage. HalPowerManager still refuses to go below BLE_SAFE_FREQ while
  // the controller is enabled, so returning false here cannot put the radio in
  // an unsupported state.
  bool preventThrottle() override;

 private:
  void applyState();
  void logEntryFacts() const;

  State state_ = State::IdleFull;
  // Set by selectByName() from another context (the CMD: dispatch runs in the
  // main loop, before this activity's loop()), picked up on the next tick.
  static volatile int pendingState_;
  bool radioUp_ = false;
};

#endif  // ENABLE_POWER_LAB
