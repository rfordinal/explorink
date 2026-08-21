# Idle sleep on the map screen: what parks the device, what wakes it

The rider leaves the device on a table mid-ride -- a pub stop, a break, a night
in a tent -- with the map up and tracking nominally running. This is the plan for
a parked state that costs almost nothing and comes back **without the rider
touching the device**.

**Status: design. Nothing here is built.** The measurement campaign it feeds is
[`power-plan.md`](power-plan.md); the behaviour findings are in
[`power-management.md`](power-management.md). This file is the design and the
implementation order, so it is the one to read first in a session that means to
build it.

Confidence is marked per claim: **[repo]** read off this code, **[measured]** on
hardware in this repo, **[primary]** from a vendor page or the pinned ESP-IDF
source on disk, **[open]** nobody knows.

## What the X4 cannot do

Four facts kill the obvious designs before they start.

- **No motion sensing.** The X4 board profile carries `NO_SENSORS`
  (`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:707`) -- no IMU,
  no accelerometer. X3 has a QMI8658 (`:749`) and the Sticky an LSM6DS3
  (`:1041`); X4 and X4 Pro have neither (`:1141`). ESP32-C3 also has no
  ULP/LP coprocessor -- `soc_caps.h` defines no `SOC_ULP*` at all. **[repo]**,
  **[primary]**
  So **the device cannot see that the rider moved.** Only the phone can.
- **Only the power button can wake deep sleep.** Deep-sleep GPIO wake on C3 is
  restricted to GPIO0-5 (`SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK`,
  ESP-IDF `components/soc/esp32c3/include/soc/soc_caps.h:177`). The power button
  is GPIO3 (`BoardConfig.h:694`) and qualifies. Every other button is on an ADC
  resistor ladder (`InputStyle::XteinkAdcLadder`, `BoardConfig.h:687`), which is
  polled, not a wake line. **[primary]**, **[repo]**
- **No wake-on-BLE from deep sleep.** The C3 does have a BT wake source
  (`SOC_PM_SUPPORT_BT_WAKEUP` is 1, `soc_caps.h:446`) but it is a **light
  sleep** source; deep sleep powers the radio down and drops the link. **[primary]**
- **It cannot carry a 32.768 kHz crystal either.** On C3 that crystal is fixable
  only across GPIO0/GPIO1; GPIO1 is the button ADC ladder and GPIO0 is the battery
  divider
  ([`power-management.md`](power-management.md), "The X4 cannot have a 32.768 kHz
  crystal"). So the cheap sleep clock is not available and the parked floor lands
  on the main-crystal column. **[primary]**, **[repo]**
- **Today's deep sleep is a power-off, not a sleep.** `startDeepSleep()` drives
  GPIO13 -- the battery latch MOSFET -- LOW and holds it
  (`lib/hal/HalPowerManager.cpp:100-109`), which disconnects the MCU from the
  battery. So there is no RTC-timer wake on battery today, and the wake is the
  button bridging the rail. **[repo]**

## The numbers that decide the design

The figures live in [`power-management.md`](power-management.md), "What a C3
actually draws asleep": on an ESP32-C3 BLE peripheral, modem sleep is 12 mA,
light sleep on the main crystal **2.3 mA**, light sleep on an external
32.768 kHz crystal **140 uA** -- Espressif's own measurement of its NimBLE
`power_save` example. **[primary]**

Two consequences for this design. **Light sleep is the whole win**: it is the
only state that turns 12-24 mA into single milliamps while the radio still
works. And **the crystal column is not available to the X4** -- it would be 16x, ten
days against months, but both crystal pins already carry analog duty (see above). So
the target for this board is the **2.3 mA column plus the board's own floor**, and
the 140 uA column is a requirement to put on a future board rather than an experiment
to run on this one.

Two things to keep straight about that 2.3 mA. It is a **devkit SoC** figure for an
advertising, not-connected peripheral with an **idle CPU**, so it presumes S2 also
parks our own 100 Hz map loop -- which is design work not done here. And it is the
floor for a build that can hold connections; there is a third clock option
(`RTC_SLOW`, the internal RC) that needs no crystal and no pins, is forbidden for
connections but not for advertising, and is measured nowhere. Both caveats:
[`power-management.md`](power-management.md), "What a C3 actually draws asleep" and
"The X4 cannot have a 32.768 kHz crystal".

Those are **SoC** numbers on a devkit. Our board's own floor sits on top, and
nobody has priced it.

## The floor nobody has priced, and it bounds everything

X4 and X3 have no switched peripheral rails, so `powerDownRailsForSleep()` is a
no-op on them and the SD card, the battery divider, the regulator and the panel
controller stay powered in **every** sleep state that keeps the battery latch
closed. Detail and citations: [`power-management.md`](power-management.md), "The
board's own floor is unpriced". **[repo]**

Why it bounds this design: if that floor is 1 mA or more, the SoC's 5 uA deep
sleep is irrelevant, most of the crystal's 16x is unreachable, and S2's expected
2.3 mA is optimistic by whatever the floor turns out to be. **[open]** --
experiment 1 exists to price it, and it comes first for that reason.

## The landmine: `CONFIG_PM_ENABLE` alone saves nothing

**Read this before writing any config.** With the low-power clock set to the main
crystal -- what this tree does (`platformio.ini:160`) -- the BLE controller holds
an `ESP_PM_NO_LIGHT_SLEEP` lock for as long as Bluetooth is enabled, unless
`CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP` is also set. Mechanism, source
lines and the warning string it prints:
[`power-management.md`](power-management.md), "`CONFIG_PM_ENABLE` alone saves
nothing while the radio is up". **[primary]**

So route B as `power-plan.md` writes it -- PM_ENABLE plus tickless idle -- would
compile, boot, hold its link and light-sleep **never**. That is a whole
build-and-run cycle spent learning the wrong lesson.

The full config set for C3, from the NimBLE `power_save` example's own
instructions (ESP-IDF `examples/bluetooth/nimble/power_save/README.md:44-63`):

```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_ESP_PHY_MAC_BB_PD=y                        ; power down MAC+BB with the PHY
CONFIG_BT_CTRL_MODEM_SLEEP=y                      ; already set
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y               ; already set
CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y              ; already set
CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y  ; THE MISSING ONE
```

Two dependencies to respect, both from the Kconfig
(`components/bt/controller/esp32c3/Kconfig.in:410-433`): the PU option
`depends on ... && FREERTOS_USE_TICKLESS_IDLE`, so tickless idle is not
optional; and `BT_CTRL_LPCLK_SEL_EXT_32K_XTAL` `depends on RTC_CLK_SRC_EXT_CRYS
|| RTC_CLK_SRC_EXT_OSC`, so switching to the crystal means changing the RTC
clock source too, not just the BLE clock. **[primary]**

**Write them into `custom_sdkconfig` in `platformio.ini`**, not into
`sdkconfig.defaults` -- that file is generated and gitignored
([`power-management.md`](power-management.md), "`sdkconfig.defaults` is generated
and gitignored").

## The design: two states, and deep sleep is not one of them

### S1 -- quiet. Mostly already built.

Parked-but-linked. Its content already exists:

- The phone already stops sending. `SendPolicy` floors sends at 7 s moving,
  30 s at walking pace, and **one keepalive per hour** when nothing moves
  (`android/app/src/main/java/org/explorink/gpsbridge/SendPolicy.kt:26-40`,
  parent repo). Parked BLE traffic is already near zero, so there is nothing to
  win by asking the phone to slow down further. **[repo]**
- The idle connection set is already 30-50 ms interval with slave latency 4
  (`lib/BlePositionServer/include/BlePositionServer.h:553-569`).
- The map already throttles to the 80 MHz floor (`power-management.md`, "The map
  throttles to 80 MHz").

What is left in S1 is small: **pause the `power.csv` write while parked** (once a
minute keeps the SD card from ever settling -- `PowerLog::kIntervalMs`,
`src/PowerLog.h:33`) and let the map loop take a longer delay when it is only
waiting for a fix. Not a state. The tail of route A.

### S2 -- parked. This is the design.

Light sleep between radio events, with the config set above. Two sub-cases, and
the important claim is that **one mechanism covers both**:

- **Link up.** The device light-sleeps between connection events. A write from the
  phone wakes the radio within about one connection interval -- and the application
  reacts up to one loop period later, which is what makes the parked cadence a
  latency decision and not only a power one ("S2's missing half" below). Nothing on
  the device has to decide anything.
- **Link down** (rider walked away with the phone, supervision timeout dropped
  the link). The device keeps **advertising**, slowly, and light-sleeps between
  advertising events -- which is exactly what the 2.3 mA figure measures, since
  the example is an advertising peripheral. The advertisement stays on the air
  continuously, so a client that wants to resume just connects.

This second case is the one the whole "deep sleep and wake up periodically to
advertise" idea was invented for, and light sleep does it better: no boot, no
lost RAM, no window a scanner can miss, and the button still wakes it instantly
because light-sleep GPIO wake works on any pin.

Expected parked draw: **2.3 mA plus the unpriced board floor** on main XTAL,
**0.14 mA plus the floor** with a 32 kHz crystal. **[primary]** for the SoC
term, **[open]** for the floor, so the total is **[open]** until experiment 1.

### S3 -- rejected: deep sleep on a duty cycle

The tempting design -- deep sleep at ~5 uA, RTC timer wake every 20-120 s,
advertise for a second or two, sleep again -- does not survive arithmetic:

- Every wake is a **full boot** (deep-sleep wake is a chip reset), so SD mount,
  fonts, settings and a NimBLE init at 57 KB of heap
  (`map-memory.md:203`) happen every cycle. Boot-to-advertising time is
  **[open]** -- no C3 figure exists in the datasheet or IDF docs and nobody has
  timed this firmware -- but 1.5-3 s is the realistic band.
- At ~3.5 s awake at ~30-50 mA per cycle, a 20-120 s cadence averages roughly
  **1.2-7 mA**, i.e. the same as S2 or worse, for far more complexity. It only
  reaches genuinely low numbers at wake periods around 10 minutes, by which
  point the no-touch resume promise is gone anyway. **[open]**, arithmetic on
  an open boot time.
- The wake channel is **lossy by construction**: the client has to catch a 1-2 s
  advertising window. Neither Apple nor Google bounds background discovery
  latency, and this repo's own measurement of it was never run
  (`../../docs/ble-fix-plan.md`, H2). Real resume latency would be minutes and
  unbounded.
- It **flaps the phone**: every cycle fires appear/disappear at
  `CompanionDeviceManager`, binding `X4PresenceService` and starting
  `BridgeService` again (`../../docs/ble-app-wake.md`). The phone pays a GPS
  spin-up per flap, and `onDeviceDisappeared` was never designed for a
  deliberately blinking advertiser.

**Deep sleep keeps exactly the role it has today**: the deliberate power-off,
woken by the button, with the sleep screen showing the parked fix
(`sleep-screen.md`). It is not part of the idle path.

## The wake contract, written so a second client can implement it

`CLAUDE.md` bans building a feature whose only trigger is the Android
device-side wake trick, because iOS has no straight equivalent. So the contract
is stated in terms of what goes on the air, not in terms of one OS:

> While parked, the device either holds the link or advertises continuously at a
> slow interval. A client resumes by writing on the open link, or by connecting
> to the advertisement.

Both platforms can implement that:

- **Android**: the existing `CompanionDeviceManager.startObservingDevicePresence`
  path, already verified end to end 2026-08-11 (`../../docs/ble-app-wake.md`).
- **iOS**: a standing `connect()` plus Core Bluetooth **state preservation and
  restoration** -- connection requests do not time out, and the system relaunches
  the app when the peripheral appears. **[primary, reported]** -- the mechanism comes
  from Apple's Core Bluetooth background guide and TN3115 as read by a research pass,
  not opened directly here; verify before a design leans on it.
  **[open]** for its behaviour on this product, and it carries Apple-documented
  gaps (a force-quit stops it until the user reopens the app, toggling Bluetooth
  in Settings stops it, and a background wake gets only seconds of runtime).

Two firmware requirements fall out of the iOS half, and both are cheap if done
now:

1. **A stable advertising address across sleep states.** A rotating random
   address would orphan a standing iOS connect. What NimBLE uses here today is
   **[open]** -- check before relying on it.
2. **Apple-sanctioned intervals and connection parameters.** See
   [`ble-advertising.md`](ble-advertising.md), "What iOS expects", for where the
   current values sit and the one that is already out of range.

## S2's missing half: parking our own loop

The 2.3 mA figure is the floor of a firmware whose **CPU is idle**. Ours is not: the
main loop runs at roughly 100 Hz and every tick does a little work. Turning on light
sleep without addressing that buys a fraction of what the number promises. This
section is that design work. **Nothing here is built or measured.**

### What actually runs every tick today

Per iteration of `loop()` in `src/main.cpp`, on the map screen:

- **Two ADC conversions for the button ladder.** `gpio.update()` reaches
  `InputManager::readButtonAdc()`, which `analogRead()`s both ladder pins on every
  poll (`freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp`, the
  `XteinkAdcLadder` branch). At 100 Hz that is ~200 conversions a second, and it is
  the largest per-tick item that is pure overhead while nobody is touching the
  device. Already on the campaign's ideas list ("gate the ADC button ladder"),
  unmeasured.
- **`MapActivity::loop()`**: pin popup service, input handling, `getLatest()` for a
  new fix, the command console poll on **both** transports, `updateHeaderStatus()`,
  `serviceAdvertising()`, the indication slot, and the redraw/save deadline compares.
  Individually a handful of integer compares -- the code says so and it is right --
  but collectively they are the reason the tick exists at all.
- **`POWER_TELEMETRY.onLoop()`** and **`PowerLog::tick()`**, the latter a no-op on
  all but one iteration a minute, when it writes a row to the SD card.
- Then `delay(10)`.

Two things that are **not** in the way, checked rather than assumed:

- **The FreeRTOS tick is 1000 Hz** (the ESP-IDF/Arduino default; not overridden in
  `platformio.ini`'s `custom_sdkconfig`, so it stays 1000 unless someone sets
  `CONFIG_FREERTOS_HZ` there), so `delay(10)` is ten
  ticks and tickless idle has a window to sleep in. The cadence is not structurally
  hostile to light sleep.
- **Nothing on the map screen holds a power lock.** `HalPowerManager::Lock` appears in
  exactly two places -- deep-sleep preparation (`src/main.cpp:239`) and rendering
  (`src/activities/ActivityManager.cpp:58`) -- both scoped and short. No map path pins
  the clock for the duration. Grepped across `src/`, `lib/` and `freeink-sdk/libs/`:
  no other holder, and no raw `esp_pm_lock` use anywhere in the tree.

### The open question this design turns on

**Is 100 wakes a second fatal to residency, or merely wasteful?** Unmeasured, and it
decides how much of the below is needed. Light-sleep entry and exit cost on C3 is not
published for this case and we have not measured it. If overhead is a few hundred
microseconds, a 10 ms cadence still sleeps most of the time and the win comes mostly
from **dropping the per-tick work**, not from slowing the tick. If overhead is
milliseconds, the cadence itself has to grow.

**Measure before designing further.** Experiment 3 answers it as a side effect: the
same bench run that proves light sleep works can report residency, and the lab
screen's `loops` counter already exists to show the tick rate against it.

### The parked policy, if the cadence does have to grow

- **Loop cadence.** `delay(10)` becomes a parked value. The ceiling is set by what it
  delays, not by power: a fix arriving over BLE is handled by the NimBLE host task
  immediately, but **our reaction to it waits for the next tick**, so the parked
  cadence is added to the fix-to-pixel latency. For a parked device that is fine, and
  the first fix un-parks us anyway. Correction to an earlier claim in this file: a
  phone write wakes the *radio* within one connection interval; it wakes *the
  application* up to one loop period later.
- **Gate the ADC ladder.** Poll it every Nth parked tick, or not at all. The cost is
  input latency: the ladder is polled, never interrupt-driven, so the worst-case delay
  between a press and it being seen is the poll period. The **power button is
  different** -- GPIO3 is a real digital line, so it can wake light sleep directly and
  stays the responsive way back. Whether a ladder press swings its pin far enough to
  trigger a light-sleep level wake is **speculation**; it is a cheap thing to try and
  would make all the buttons responsive while parked.
- **Give every per-tick item a parked budget**, listed above so a reviewer can check a
  diff against it. Most are already cheap; the ones with real cost are the ADC poll,
  the two console polls and `updateHeaderStatus()`.
- **`serviceAdvertising()` is safe to slow down**: its retry is already rate-limited
  to `kAdvertisingRetryMs` = 1000 ms
  (`lib/BlePositionServer/include/BlePositionServer.h:460`), so a parked cadence at or
  under a second changes nothing about how advertising recovers.
- **Pause `PowerLog` while parked.** A once-a-minute SD write is noise beside 24 mA
  and a real share of a 2-3 mA floor -- it is the instrument becoming the thing that
  stops the target being met (already `power-plan.md`, phase 4).

### How this gets guarded, so the next map change cannot quietly undo it

The point is not to write the rule down. It is to make the rule **fail loudly** when
someone adds a timer to the map screen six months from now, without them having read
this file.

1. **Put the policy in the `Activity` interface, next to the two bools that already
   live there.** `preventAutoSleep()` and `preventThrottle()` are the precedent: the
   screen answers the question, and `main.cpp` obeys. A third member -- an idle
   cadence, defaulting to today's behaviour -- means **a new activity author has to
   answer it**, and a new per-tick timer in `MapActivity` has to get its budget from
   somewhere visible. That is a structural guard rather than a documentation one, and
   it is the most valuable item in this list.
2. **One pure function, one host test.** The cadence decision has pure inputs --
   parked or not, queued work, transfer active, recent user input -- so it belongs in
   a function with no hardware in it, and a host test pins the table of expected
   answers. That is the only guard that fires at **CI time**, before a build reaches a
   device. **Built 2026-08-21**: `src/ParkedLoopPolicy.h`, nine cases in
   `test/parked_loop_policy/ParkedLoopPolicyTest.cpp`. Two things it deliberately does
   not do. It does not pick the parked cadence -- both values default to today's 10 ms,
   so taking the header changes no behaviour, and experiment 3's residency number is
   what replaces the placeholder. And nothing calls it yet: wiring it into `main.cpp`'s
   `delay(10)` only pays under `CONFIG_PM_ENABLE`, because a `delay()` with PM off is a
   busy wait at full clock.
3. **A field tripwire in `power.csv`, from columns that already exist.** `loops` is
   logged per row today, and the parked state is going in as a new column for the lab
   screen. The invariant is then checkable by a script over any run: **while parked,
   `loops` per minute must stay under its budget.** A regression shows up as a 50x
   jump in a column nobody had to remember to look at.
4. **A runtime canary.** When parked and the accumulated loop-busy time for a minute
   exceeds its budget, log one line naming it. Turns "somebody added a per-tick call"
   from a silent extra milliamp into a visible warning on the next serial session.
5. **A review question, in the doc and in a comment beside `MapActivity::loop()`**:
   does this change add work to a parked tick? Weakest of the five, and still worth
   having, because it is the one that catches the case at the moment it is written.

Items 1 and 2 are the ones that actually guard. Items 3 and 4 catch what slips
through. Item 5 catches what a careful author would have caught anyway.

## The power lab screen

A separate activity, off the map, whose only job is to enter one power state
deliberately and hold it.

**The strongest reason is not "fewer confounders".** The campaign rule is one
change, one branch, one build, one run (`power-plan.md`). Today every comparison
of two states is also a comparison of two builds, so it rests on the two builds
differing only where the author thinks. A lab screen moves the state into
**runtime**: the binary is identical across states, so the difference measured is
the state. That is a stronger comparison than the campaign can make today.

It also removes real confounders. On the map screen, a measurement sits on top of
tile reads from SD, the renderer, `MapFollow` refreshes (run 2 spent 119 s of
panel time an hour, 95 % of it marker moves), the transfer receiver, autosync and
the freshness check -- all landing in the same `power.csv` rows as the sleep state
under test.

What it must do:

- **Paint once, then never again.** No clock, no battery icon, no periodic
  redraw. E-ink holds the image for free.
- **Name the state on the panel and in `power.csv`.** Append a `state` column --
  `PowerLog`'s format rule allows appending and forbids reordering
  (`src/PowerLog.cpp:15-17`). This also closes the campaign's open TODO about
  rejecting contaminated stretches: the stretch is labelled in the data instead
  of reconstructed afterwards.
- **Print `esp_sleep_get_wakeup_cause()` on entry.** It is the only way to tell
  a real timer wake from a brownout that power-cycled the board -- which is
  precisely what experiment 1 has to distinguish.
- **Be reachable over serial**: `CMD:GOTO_POWERLAB`, with
  `setPowerSaving(false)` before any BLE init, exactly as `CMD:GOTO_MAP` already
  does (`src/main.cpp:734`) -- the bench rig only has a few seconds after reset.
- **Live behind a build flag.** `-DENABLE_POWER_LAB=1` in `default` and `slim`,
  absent from `gh_release`. The precedent is `goToPreview()`, guarded by
  `#if defined(ENABLE_PREVIEW_BENCH)` (`src/activities/ActivityManager.cpp:221-222`,
  example at `platformio.ini:233`).

States to offer, which also cover the four the campaign wants to baseline:

| State | What it isolates |
|---|---|
| idle, radio down, full clock | the plain 160 MHz floor |
| idle, radio down, throttled | the one state where the 10 MHz floor is legal |
| advertising, nobody connected | the radio's advertising duty cycle |
| connected (`tools/blefakephone.py` or the phone) | the riding case |
| light-sleep candidate, radio up | S2, on a `CONFIG_PM_ENABLE` build |
| deep sleep, latch held HIGH, timer wake | experiment 1 |

**Safety, not optional.** That last state leaves a device that looks off and is
not. So: always arm a timer wake, cap the duration, and paint what it is about to
do and when it will return before it sleeps. And on wake, reconfigure GPIO13 as
output-HIGH **before** `gpio_hold_dis()` -- releasing a hold returns the pad to
its default mode (ESP-IDF `driver/gpio.h`, `gpio_hold_en`/`gpio_hold_dis`
notes), and the field revision that does not self-latch
(`BoardConfig.h:710-712`) would power-cycle itself mid-boot. **[primary]**,
**[repo]**

One cost: a lab screen that brings up BLE pays the same 57 KB of heap the map
does, so check the heap after adding it.

**Built 2026-08-21** as `src/activities/power/PowerLabActivity.{h,cpp}`, in its
own `env:powerlab` (not in `default`, not in the gitignored
`platformio.local.ini` -- the frozen baseline has to be rebuildable from git,
`power-plan.md` condition 2). Four states, each one an open question of this
campaign:

| Label | Radio | Clock | What it prices |
|---|---|---|---|
| `idle-160` | down | 160 | the full-clock floor with nothing running |
| `idle-10` | down | 10 | the 10 MHz floor, legal only with BLE down |
| `radio-160` | up | 160 | what the map screen does today |
| `radio-80` | up | 80 | **route A's floor, built and never measured** |

`radio-80` is free and new: `HalPowerManager::lowPowerFloorMhz()` asks the BT
controller and returns `BLE_SAFE_FREQ` = 80 MHz whenever it is enabled
(`lib/hal/HalPowerManager.cpp:18-27`, `lib/hal/HalPowerManager.h:49`), so the
state is safe by construction rather than by anyone remembering the rule.

Reachable only over the console -- no Home row, on purpose -- as
`CMD:GOTO_POWERLAB` or `CMD:POWERLAB_STATE <label>`. The second one exists
because **a button press is itself a change to the measurement**: it restores
the full clock and resets the inactivity timer (`src/main.cpp:836`). The bench
sends the state in the ~3 s full-clock window after reset, then USB comes out.

Not built: the deep-sleep-with-the-latch-held state. That is experiment 1, it
needs a meter to be worth entering, and its safety rules carry a real brownout
risk on the non-self-latching field revision -- a separate piece of work rather
than a fifth row on a screen that otherwise cannot hurt anything.

**One binary, many states.** Selecting the state at runtime is the whole point: the
binary stays bit-identical across a comparison, so what the meter sees is the state
and not a second difference nobody noticed. The compile-time options -- the crystal,
`CONFIG_PM_ENABLE`, the PU flag -- cannot work that way, so those stay one option per
build, all cut from the same frozen base. The campaign runs on a deliberately stale
branch for exactly this reason, with an exemption from the rebase-before-flash rule
and four conditions attached: [`power-plan.md`](power-plan.md), "The frozen
baseline".

What the lab screen does **not** do: it prices **states**, not the product. A
number from it is not an endurance figure, and the map-screen run still has to
validate the real thing.

## Experiments, cheapest and most decisive first

Each one is written out as a followable procedure -- config lines, bench setup,
duration, pass and fail criteria, and what makes us stop -- in
[`power-test-runbook.md`](power-test-runbook.md). What follows is the why and the
order.

1. **The board floor, and whether deep sleep can keep the battery latch.**
   Drive GPIO13 HIGH instead of LOW before the existing hold in
   `startDeepSleep()`, arm `esp_sleep_enable_timer_wakeup()`, panel deep-slept as
   today. **Instrument: a uA-capable meter in series with the battery** --
   `power.csv` cannot see microamps, and a USB meter charges the cell
   (`power-plan.md`, constraint 1). Refutes: a brownout means the
   non-self-latching revision cannot hold its own latch and this whole class of
   state is dead on that hardware; a reading of 1 mA or more means the board
   floor bounds every deep state and most of the crystal's value with it;
   10-100 uA means the deep states are real. Also proves timer wake fires and
   the device boots from it.
2. ~~**The crystal question, with no instrument at all.**~~ **ANSWERED
   2026-08-19, and it needed no experiment at all.** The X4 cannot have a working
   crystal: on C3 it is fixable only across GPIO0/GPIO1, and those are the button
   ADC ladder and the battery divider ([`power-management.md`](power-management.md), "The X4 cannot have a
   32.768 kHz crystal"). The planned boot-log test -- build with
   `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` plus
   `CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL=y` and watch for `32.768kHz XTAL not
   detected, fall back to main XTAL as Bluetooth sleep clock` (`bt.c:1693-1697`)
   -- is still the right test **on another board**, and X4 Pro is the one worth
   running it on. **[primary]**
3. **The `CONFIG_PM_ENABLE` smoke test -- go/no-go for S2.** One branch with the
   seven-option set above and an `esp_pm_configure()` of max 160 / min 80 /
   light sleep enabled. Bench it on `tools/blefakephone.py` with a **control run
   on the last known-good build**, per the 2026-08-16 rule. Watch: does the link
   hold, do the SPI panel, the ADC ladder and the SD card survive APB movement,
   does a phone write land inside the latency budget. **Also report residency** --
   what fraction of wall time was actually spent in light sleep at the current 100 Hz
   loop -- because that single number decides how much of "S2's missing half" has to
   be built. **Expect USB serial to die when sleep engages** -- that is the feature working, not a fault; evidence has
   to come from `power.csv` and BLE. Refutes: any driver that breaks under DFS
   plus light sleep names the next work item and, if it is the panel, may kill S2
   as configured.
4. **Slow-advertising discovery latency, phone side.** Advertise at an
   Apple-sanctioned slow value and measure map-open to `X4 appeared`, plus
   reconnect from parked-disconnected. Instrument: logcat and a stopwatch, five
   trials. This is the unrun H2 measurement (`../../docs/ble-fix-plan.md`) and it
   prices the resume promise the rider actually asked for.
5. **Boot-to-map-with-a-fix after a timer wake.** Only needed if some
   duty-cycled variant is revived. Serial timestamps from reset to first
   rendered fix.
6. **The `RTC_SLOW` advertising-only variant** -- one config line
   (`CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y`), same bench as experiment 3, parked and
   never connected. This is the only path to a sub-milliamp parked floor left on X4,
   because the internal RC needs no crystal and no pins. It cannot hold a connection
   (`>500 ppm`), so a single binary cannot be both -- which is exactly why it is worth
   knowing the number before deciding whether that trade is interesting. Refutes: if
   it lands near 2.3 mA anyway, the crystal really was the only path and X4 stops at
   the main-XTAL column.

Order, revised 2026-08-19 once experiment 2 answered itself: **3, then 1**, with
experiment 6 riding along on 3's bench.
Experiment 3 is the only **unconditional** one -- without light sleep there is no
state below 80 MHz with a live radio at all -- and now that the crystal is ruled
out it is also the *only* remaining way to move the number on this board.
Experiment 1 comes second because it needs the meter, not because it matters
least: with the crystal gone, the board's own floor is the last unknown term in
the parked budget, so it is what says whether 2.3 mA plus the floor is good enough
to stop at.

**Deep sleep survives as an instrument even though it is rejected as a feature.**
Experiment 1 needs it: the board's own floor cannot be separated from the SoC's
draw except in a state where the SoC draws almost nothing, and deep sleep with the
latch **held** is the only such state. Cutting the latch, as today's sleep does,
disconnects the board and measures nothing. One use, for calibration, then never
again.

All of these run on the frozen baseline described in
[`power-plan.md`](power-plan.md), "The frozen baseline" -- so a run's numbers stay
comparable to the run before it.

## Open questions

- **What is the board's own floor with the latch held?** Bounds everything above.
  Experiment 1.
- ~~**Does the X4 have an external 32.768 kHz crystal?**~~ **Answered
  2026-08-19: no, and it cannot carry a working one** -- GPIO1 is the button ladder,
  GPIO0 is the battery divider, and on C3 those are the two crystal pins. Open on
  **X4 Pro**, an S3 whose crystal pins are GPIO15/GPIO16 and which uses digital
  buttons -- nothing *known* is in the way there, though its RE'd profile has an
  unlocated battery ADC and those two pins are ADC2 channels.
- **Does a 100 Hz loop destroy light-sleep residency, or only waste a little?**
  Decides how much of the parked-loop design is needed. Light-sleep entry/exit cost on
  C3 is unpublished for this case and unmeasured by us. Experiment 3 reports it as a
  side effect.
- **Can a ladder button press trigger a light-sleep GPIO level wake?** If yes, all the
  buttons stay responsive while parked and the ADC poll can be gated hard. If no, the
  power button is the only responsive way back. Speculation today; cheap to try.
- **What does the internal 136 kHz RC cost as the BLE sleep clock, parked and never
  connected?** Unmeasured anywhere, needs no hardware change, and it is the only
  remaining route to a sub-milliamp floor on X4. Experiment 6.
- **Does anything break under DFS plus light sleep** -- SPI panel, ADC ladder,
  SD, USB CDC? Experiment 3. Expect to find at least one.
- **What advertising address does NimBLE use here, and is it stable across sleep
  states?** Gates the iOS resume path.
- **Boot-to-advertising time from deep sleep.** Undocumented for C3 anywhere;
  only a stopwatch answers it. Needed only if S3 is ever revived.
- **What does the phone app do with a peripheral that parks on purpose?**
  `onDeviceDisappeared` does nothing today, and the app has no parked notion.
