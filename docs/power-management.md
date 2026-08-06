# Power management

CPU frequency scaling on idle, and what it breaks if you switch activities
without going through a button press.

> **Optimisation review, 2026-08-06.**
> [`optimization/07-power-and-lifecycle.md`](optimization/07-power-and-lifecycle.md)
> adds the other half of this: `preventAutoSleep()` is one bool answering two
> questions, so the map screen pins the CPU at 160 MHz for as long as it is up
> and can never auto-sleep. Nothing has measured the device's current draw in any
> state, and that measurement is step 1 of that plan. Record the numbers here
> when they exist.

## Power-saving mode drops CPU frequency after idle

`main.cpp:638` calls `powerManager.setPowerSaving(true)` after the device has
been idle for a while (no button press, no touch, no tilt, nothing holding
`activityManager.preventAutoSleep()` true). This lowers CPU frequency to save
battery: `HalPowerManager::LOW_POWER_FREQ` is **10 MHz** on X4 (80 MHz only
where `BOARD_HAS_PSRAM`), and the threshold is
`HalPowerManager::IDLE_POWER_SAVING_MS`, **3 seconds**
(`lib/hal/HalPowerManager.h:29-33`). Confirmed on real hardware via `LOG_DBG`
output: `[PWR] Going to low-power mode`, ~3 s after the last input.

`main.cpp:555` restores full speed (`setPowerSaving(false)`) whenever
`gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() ||
halTiltSensor.hadActivity() || activityManager.preventAutoSleep()` is true --
i.e. on the very next iteration after any physical input, before that input's
effect (a menu selection, an activity switch) is even acted on.

## NimBLEDevice::init() hangs solid in low-power mode

**Verified on real hardware, 2026-08-04, reproduced twice identically.**
Entering `MapActivity` (which calls `BlePositionServer::begin()` ->
`NimBLEDevice::init()`, `lib/BlePositionServer/src/BlePositionServer.cpp:92`)
while the CPU is still in power-saving mode hangs the device solid: serial
dead, buttons dead, screen frozen on whatever was last drawn. The ROM
bootloader still answers (`esptool` reaches it -- that's a separate,
lower-level thing bootloader entry always resets into), so the device is not
bricked, but the running app is gone until reflashed.

Pinned down with `LOG_DBG` breadcrumbs between every call inside `begin()`:
the last line printed is always `begin: calling NimBLEDevice::init`, then
silence. `begun_=0` and `isInitialized=0` at the top of `begin()` both times
-- this is a cold, first-ever init, not the self-heal deinit/reinit path.

**Why buttons never hit this**: every physical path into Map (Home menu ->
select Map -> confirm) goes through several button presses first, each one
restoring full CPU speed (`main.cpp:555`) before `MapActivity::onEnter()`
ever runs. A programmatic activity switch with no button behind it --
`CMD:GOTO_MAP` (`main.cpp`, the `CMD:` dispatch block) is the first one this
firmware has -- can enter an activity while the CPU is still throttled, and
`NimBLEDevice::init()` does not tolerate that.

**Not fully explained**: exactly which part of NimBLE/BT-controller init
needs full clock speed, or whether it's a fixed timeout vs. a genuine
deadlock. Would need scope-probing the radio init sequence at reduced
frequency to pin down further -- not done, the fix below sidesteps the
question rather than answering it.

## 10 MHz starves a 48 KB serial dump

**Measured on hardware 2026-08-05.** `CMD:SCREENSHOT` and
`CMD:SCREENSHOT_GRAY` both truncated at ~4 KB of 48,000 -- 4288 and 4096 bytes
in two consecutive attempts. Not a `writeAllChunked()` regression: the device was
simply in low-power mode.

At 10 MHz the CDC drain is slow enough that `writeAllChunked()` burns its whole
3-second budget on roughly the first TX-buffer-full (4096 bytes) and gives up.
Serial traffic is **not** "user activity" -- the wake check reads gpio, touch and
tilt only -- so a host that just sends a command talks to a 10 MHz CPU, and the
3-second idle threshold means that is the normal case, not an edge case.

The 100/100 clean result in `docs/debug-screenshot-channel-plan.md`'s gate 2 was
taken on a device being poked by hand, which is why this never showed up there.

## The fix

**Every `CMD:` handler runs at full CPU.** `main.cpp` calls
`powerManager.setPowerSaving(false)` once, right after a line is recognised as a
command, before dispatch. That covers screenshots, `GOTO_MAP` and anything added
later.

Two separate reasons it has to be there, both hardware-confirmed:

- a 48 KB dump starves at 10 MHz (above);
- `NimBLEDevice::init()` (`MapActivity::onEnter()` ->
  `BlePositionServer::begin()`) hangs solid if entered while still in
  power-saving mode after idle (the section above).

The device drops back to low power on its own after the handler returns and the
idle window passes again -- nothing to restore.

Same rule for any *other* programmatic path that switches activities or talks to
the radio without a button press behind it (an automation hook, a BLE command
that switches screens): it needs the same call. This is a class of bug, not a
one-off.
