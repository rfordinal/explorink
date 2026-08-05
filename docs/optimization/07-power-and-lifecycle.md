# Plan 07 — power and lifecycle

This is a battery-powered navigation device. Nothing in the tree measures its
own power draw, and the one thing that governs it is a side effect of a flag
meant for something else.

## The finding

**read.** `main.cpp:637-641`:

```cpp
static unsigned long lastActivityTime = millis();
if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() ||
    halTiltSensor.hadActivity() || activityManager.preventAutoSleep()) {
  lastActivityTime = millis();         // Reset inactivity timer
  powerManager.setPowerSaving(false);  // Restore normal CPU frequency
}
```

`preventAutoSleep()` does two things, not one: it holds off auto-sleep **and** it
pins the CPU at full speed, because both hang off the same `lastActivityTime`
reset and the same `setPowerSaving(false)` call.

`MapActivity::preventAutoSleep()` returns `BlePositionServer::isRunning()`
(`MapActivity.cpp:768`), which is true for the whole time the map screen is up
(`begin()` in `onEnter()` at `:379`). `TileSyncActivity` does the same
(`TileSyncActivity.cpp:128`).

So, on the map screen:

- the CPU never drops to `LOW_POWER_FREQ` — **10 MHz on X4**
  (`lib/hal/HalPowerManager.h:31`), which it would otherwise do after
  `IDLE_POWER_SAVING_MS` = 3 s (`:33`);
- `lastActivityTime` is reset every single `loop()` iteration, so the auto-sleep
  check at `main.cpp:668-674` can never fire.

Both are intended for the sleep half. Neither was chosen for the clock half.

**open, and it is the measurement this whole plan waits on:** what that costs in
milliamps. Nothing in either repo has measured device current in any state. Until
that number exists, everything below is a hypothesis about a device that is
either fine or eating its battery on a mountain, and there is no way to tell
which from the code.

## Step 1 — measure the current draw

Four states, USB detached, on a bench supply or an inline meter:

| State | What it isolates |
|---|---|
| Home screen, idle 30 s | the 10 MHz floor |
| Map screen, no phone connected | BLE advertising + 160 MHz |
| Map screen, phone connected, fix every 10 s | the real riding case |
| Tile sync, transfer running | the worst case |

Record all four in `docs/power-management.md`, which is the right home — it is
already the topic doc for CPU scaling and it already carries two
hardware-verified findings (`NimBLEDevice::init()` hanging at 10 MHz, and the
48 KB serial dump starving).

This is a hardware measurement and it needs the user, not an agent. Flag it as
such.

## Step 2 — separate "do not sleep" from "do not throttle"

Only worth doing if step 1 says the clock matters.

`preventAutoSleep()` is one bool answering two questions. Split it:

```cpp
virtual bool preventAutoSleep() const { return false; }   // do not deep sleep
virtual bool preventThrottle()  const { return false; }   // do not drop to 10 MHz
```

`main.cpp` then resets `lastActivityTime` on the first and calls
`setPowerSaving(false)` on the second. Default both to `false` in `Activity` so
every inherited activity behaves exactly as it does today
(`src/activities/Activity.h`), and have the existing callers
(`CrossPointWebServerActivity`, `OtaUpdateActivity`, and the map screens) return
`true` from both — which reproduces current behaviour for all of them, so the
split is a no-op until something opts out.

Then the map screen can answer:

- `preventAutoSleep()` → **true**. A rider must not have the device deep-sleep
  mid-ride.
- `preventThrottle()` → **only while something needs the clock.**

## Step 3 — work out what actually needs the clock

Two things do, both already documented as hardware-verified in
`docs/power-management.md`:

- **`NimBLEDevice::init()` hangs solid at 10 MHz.** Reproduced twice,
  2026-08-04. So `begin()` must run at full speed. It already does on every
  button path, and `CMD:GOTO_MAP` forces it (`main.cpp:563`).
- **A 48 KB CDC dump starves at 10 MHz.** Measured 2026-08-05. Every `CMD:`
  handler forces full speed before dispatch.

What is **open**:

- Can a NimBLE link that is already up survive a drop to 10 MHz? The measured
  finding is about `init()`, not about steady state. Nothing has tested a
  throttle with a connection live. If it cannot, `preventThrottle()` stays true
  whenever BLE is up and this plan ends at step 1.
- How long a viewport reset takes at 10 MHz. A reset is already the better part
  of two seconds at 160 MHz, and plan 01 says a large share of that is software
  floating point — which scales directly with clock. A 16× slower reset is not
  acceptable, so the reset itself must run at full speed regardless.

If both answers are favourable, the shape is: throttle while waiting for a fix,
full speed for the reset and for any transfer. A fix arrives every 10 s in hike
mode and the device is idle between them, which is where the saving would be.

Do not build that until step 1 says there is a saving worth having and the two
open questions are answered on hardware.

## Step 4 — the sleep screen already does the right thing

Worth recording so it is not undone: the sleep screen shows the last known
location instead of a book cover (`36b88544`), and it defaults to LIGHT
(`088f7b57`). Both are right for this device — an e-ink panel holds its image at
zero cost, so a sleeping device showing where you were is free information.

The last fix reaches the card through the same debounced settings save as the
ladders (`MapActivity.cpp:740-759`), and only for fixes this session actually
produced, never the one bootstrapped at `onEnter()`
(`showingPersistedFix_`, `:748-751`). That distinction is what stops every
re-entry writing the same fix back at itself. Correct, and easy to break — do not
"simplify" it.

## Step 5 — the 10-minute hike cadence

The design's premise is that hike mode updates every 10 minutes and the rider can
force a fresh picture from CONFIRM → Refresh (`MapActivity.h`, the button table).
That cadence is a phone-side decision — the Android bridge sends on distance, not
on a timer (`MapFollow.h:37-40`) — so nothing in the firmware implements it and
nothing needs to.

What the firmware does contribute: `MapFollow::kMinMovePx = 8` drops a fix that
would move the marker under 8 px (`MapFollow.h:40`), which is the floor that
stops a slow walker paying a panel refresh for a marker that visibly did not
move. Good. Host-tested.

One open question worth writing down: `kMaxPartialMoves = 12` forces a clean full
frame after 12 windowed refreshes, to clear ghosting (`MapFollow.h:41-45`). The
comment says it needs on-device tuning and cites the implementation plan's open
decision 4. Still untuned. The measurement is: replay a ride, watch for
ghosting, and find the number where it becomes visible. `tools/blereplay.py`
already drives the input side.

## What not to do

- **Do not make the map screen sleepable.** A device that deep-sleeps while
  strapped to a handlebar is useless, and waking it costs a boot.
- **Do not remove the `CMD:` full-speed call** (`main.cpp:563`). It is
  load-bearing twice over, both hardware-verified.
- **Do not lower `IDLE_POWER_SAVING_MS`.** It is inherited and the reader UI
  depends on its feel.
