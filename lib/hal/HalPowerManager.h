#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;      // MHz
  int appliedLowFreq = 0;  // the low-power clock actually in force, 0 when not throttled
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif

  // The floor while the BLE controller is enabled. Not a tuning knob -- a
  // correctness bound.
  //
  // Below 80 MHz the CPU leaves the PLL for the crystal and
  // rtc_clk_apb_freq_update() drags **APB down with it**; 80 and 160 MHz are
  // both PLL-sourced and leave APB pinned at 80. The BLE controller states its
  // requirement as an ESP_PM_APB_FREQ_MAX lock whose every call site sits
  // inside #ifdef CONFIG_PM_ENABLE -- which this firmware does not set -- so
  // the controller's only defence against a slow APB compiles out, and nothing
  // stops the clock being pulled out from under a live radio.
  //
  // Measured the hard way on 2026-08-16: two builds that throttled to 10 MHz
  // with the controller up hung the device solid, one at init and one in
  // steady state. See docs/power-management.md, "Why 10 MHz breaks BLE".
  static constexpr int BLE_SAFE_FREQ = 80;                     // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms

  // Which low-power clock is legal right now: BLE_SAFE_FREQ while the BT
  // controller is enabled, LOW_POWER_FREQ otherwise. Asked the controller
  // itself on every throttle and never cached -- an earlier attempt keyed on a
  // cached application-level view of the link and threw the device away when
  // that view went stale.
  static int lowPowerFloorMhz();
  static constexpr unsigned long BATTERY_POLL_MS = 1500;  // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // Raw battery voltage in millivolts, averaged over `samples` reads. 0 when
  // the board has no battery backend.
  //
  // The percentage above is the wrong instrument for measuring power draw: on
  // an ADC board it is a third-order polynomial over this number
  // (BatteryMonitor::percentageFromMillivolts), and one percent of a 650 mAh
  // cell is 6.5 mAh -- around twenty minutes of riding, so two firmware builds
  // cannot be compared by it. Millivolts can. Averaged because a single
  // analogRead() on this divider is visibly noisy.
  uint16_t getBatteryMillivolts(uint8_t samples = 8) const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
