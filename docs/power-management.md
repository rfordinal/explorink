# Power management

CPU frequency scaling on idle, and what it breaks if you switch activities
without going through a button press.

## Power-saving mode drops CPU frequency after idle

`main.cpp:638` calls `powerManager.setPowerSaving(true)` after the device has
been idle for a while (no button press, no touch, no tilt, nothing holding
`activityManager.preventAutoSleep()` true). This lowers CPU frequency to save
battery. Confirmed on real hardware via `LOG_DBG` output: `[PWR] Going to
low-power mode` at ~20 seconds of idle.

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

## The fix

Any code that can switch to an activity that touches BLE/radio hardware
without a button press behind it must call `powerManager.setPowerSaving(false)`
itself, first. `CMD:GOTO_MAP`'s handler does this (`main.cpp`, right before
`activityManager.goToMap()`). If a future programmatic activity switch is
added anywhere else (another `CMD:` command, an automation hook, a BLE
command that itself switches screens), it needs the same call for the same
reason -- this is a class of bug, not a one-off in `CMD:GOTO_MAP`.
