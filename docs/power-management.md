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

> **The measurement campaign lives in [`power-plan.md`](power-plan.md)**
> (2026-08-11): the TODO list, the run methodology, the ideas backlog and the
> table of runs. This file stays what it is -- findings about how power
> behaves. The plan says what to do about them.

## The device measures itself now

**Added 2026-08-11.** Two instruments, one vocabulary:

- **`/trailink/power.csv` on the SD card**, one row per minute
  (`src/PowerLog.cpp`, `PowerLog::kIntervalMs`). Written on every screen, from
  boot, with no phone and no cable needed. This is the only instrument that
  works for the baseline that matters most -- the device on battery with
  nothing connected.

  Two format rules, both added 2026-08-16 after reading a real file:
  - **Every row carries `build`** (`TRAILINK_VERSION`) as its last column. The
    device appends across boots, so one file holds rows from several firmwares
    and nothing else in the row says which. Run 1's file has 61 boots in it and
    no way to tell their builds apart -- that is the gap this closes.
  - **The header is written once per boot**, not once per file. It is therefore
    also the boot marker, and a file stays readable across a column change
    because each boot's rows carry their own column list. **A reader must skip
    any line starting with `uptime_s`.**
- **`stats` on the map console** (`src/activities/map/MapCommandParser.h`, the
  grammar block; `MapConsoleState::writeStats()`), answered over BLE or USB.
  Same key names as the CSV columns, deliberately, so one script parses both.
  Wired by `MapActivity` and `TileSyncActivity` through
  `fillMapPowerStats()` (`src/activities/map/MapPowerStatsProvider.h`).

Both read `PowerTelemetry` (`lib/PowerTelemetry/PowerTelemetry.h`), which
counts what costs power at the places that spend it:

| Counted | Where it is counted |
|---|---|
| Panel refreshes by waveform, plus a windowed-update bucket | `lib/hal/HalDisplay.cpp`, `displayBuffer()` / `displayBufferAsync()` / `refreshDisplay()` / `displayWindow()` |
| Panel busy time (ms blocked in a refresh) | same, plus `waitRefreshComplete()` |
| Milliseconds at full clock vs. throttled | `lib/hal/HalPowerManager.cpp`, `setPowerSaving()` |
| Main-loop iterations, busy ms, worst iteration | `src/main.cpp`, end of `loop()` |

Counters are cumulative since boot. Differencing two rows is the analysis;
the device never writes a delta, because a delta lost with a failed row is
lost for good.

**Battery is reported in millivolts, not just percent**
(`HalPowerManager::getBatteryMillivolts()`, 8 averaged ADC reads). On X4 the
percentage is a third-order polynomial over exactly that number
(`BatteryMonitor::percentageFromMillivolts()`,
`freeink-sdk/libs/hardware/BatteryMonitor/src/BatteryMonitor.cpp:443-456`), and
one percent of a 650 mAh cell is 6.5 mAh -- around twenty minutes of riding at
the draws measured below. Two firmware builds cannot be told apart by it.

**Verified on hardware 2026-08-11** (build `f6372ea6`, X4): `stats` answers
over both channels -- USB serial and BLE -- with 16 lines each and plausible
values. `power.csv` writes without an error line on serial across a 2.5-minute
boot, but **nothing has read the file off the card yet**; that is the one part
of the instrument still unproven.

What the first two `stats` calls already settled, both previously read-only
claims:

- **The map screen never throttles.** Across 41 s on the map, `full_clock_ms`
  rose 29,769 -> 70,470 while `throttled_ms` stayed frozen at 74,452 -- every
  millisecond of it at full clock. The Home screen before it had done the
  opposite (`[PWR] Going to low-power mode` 3 s after boot). Confirms
  `power-management.md` item 3 and `optimization/07`'s reading of
  `preventAutoSleep()`.
- **The loop really does run at ~100 Hz on the map.** 4,008 iterations in
  41 s, against ~33/s on the Home screen. Confirms item 4's premise.

**`rssi()` returns 0 on a live link.** The `INFO rssi_dbm=` line was absent
from a `stats` answered over BLE, i.e. with a central definitely connected and
the handle set (`BlePositionServer.cpp:601-605` returns 0 when
`ble_gap_conn_rssi()` fails). So either that HCI path is unavailable on this
build or the call errors for another reason. Pre-existing -- `rssi()` had no
caller before `stats` -- and not yet chased. Open.

The header status row consumed this value directly and, on a failure,
rendered it as full signal (0 passes every `bleBarsForRssi` threshold) --
fixed on the consumer side, 2026-08-13: `MapActivity::resolveBleBars()` now
holds the last bar count a non-zero reading produced instead of remapping a
0. See `docs/map-header-status.md`, "A failed rssi() read must not draw full
signal". This does not touch `ble_gap_conn_rssi()` itself -- if the HCI call
is failing every time on this build, the bars will sit at whatever they were
on the last connect and never move, which is still wrong, just a different
wrong. That underlying failure is still open, still here.

## Charging cannot be turned off in software (X4)

**Read off the board profile, 2026-08-11.** `XTEINK_X4` in
`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:685-715`:
`batteryAdc = 0` (GPIO0), `batteryChargeStatus = PIN_UNASSIGNED`, and
`NO_GAUGE` -- no fuel gauge, no charger IC on any bus, no charge-enable line.
The charger is autonomous. The only power-related pin the firmware can drive is
GPIO13, the battery MOSFET, and that cuts the whole device (`:709-715`).

Two consequences for every measurement:

- **A reading taken with USB plugged in is worthless.** The ADC on GPIO0 sees
  the charged rail, not a discharge curve.
- **Cutting VBUS also kills USB CDC**, because the ESP32-C3's USB-Serial-JTAG
  needs VBUS. So there is no serial channel during a real power run. That is
  why the instruments above are a card and a radio.

An inline USB meter measures the whole system *including* charging, so it
answers "what does the wall pay", not "what does the map cost". Only a
battery run answers the second.

## First real-draw numbers: two rides

**Measured on hardware, 2026-08-07, real rides, not a bench measurement.**
Both at the spec sheet's 650 mAh nominal capacity, map screen up and a phone
connected the whole time:

| Ride | Log | Duration | Battery drop | Implied avg draw |
|---|---|---|---|---|
| 1 | `docs/rides/trailink-gps-20260807-142303.jsonl` (parent repo) | 34 min | 4% = 26 mAh | ~46 mA |
| 2 | `docs/rides/trailink-gps-20260807-173058.jsonl` (parent repo) | 46.6 min | 2% = 13 mAh | ~17 mA |

Neither is a clean isolated sample of the plan's "Map screen, phone connected,
fix every 10 s" row (`optimization/07-power-and-lifecycle.md`, step 1): both
event logs show tile autosync running mid-ride (ride 1: `fetch_start`/
`fetch_end` x2, `xfer_in` x8; ride 2: same pattern plus three BLE
disconnect/reconnect cycles, `connect_timeout` x3), so part of each draw is
transfer and radio churn, not just the idle-between-fixes case the plan wants
isolated. The two numbers also disagree with each other (46 mA vs. ~17 mA) for
what should be a similar activity mix, which is itself evidence the
phone-reported percentage is too coarse to derive a precise mA number from --
treat both as ballpark, not a settled draw figure.

**Not verified**: the phone's percentage step size (a 2% read could be
anywhere in 1.5-2.49%, which alone accounts for real spread at this
resolution), the X4's actual capacity vs. the 650 mAh spec number, and
whether the battery's discharge curve is linear enough at this state of
charge for the mAh math above to hold. A bench measurement with an inline
meter (the plan's step 1) is still the number to trust over either of these.

## The state-3 baseline: ~45 mA over 11.5 hours

**Measured on hardware, 2026-08-15.** The first run long enough to be worth
quoting. Map screen up and BLE linked the whole time, hike mode, rung 0, mostly
stationary: 100 % at 10:30, 21 % at 22:00.

`power.csv` was pulled off the device the same evening, so these are
millivolt-derived, not percent-derived:

| | 99 % -> 21 % | full discharge |
|---|---|---|
| Duration | **11.42 h** | 12.94 h (to 5 %) |
| Voltage | 4220 -> 3547 mV | 4220 -> 3380 mV |
| Slope | **58.9 mV/h** | 64.9 mV/h |
| Implied draw at 650 mAh | **44.4 mA** | 47.2 mA |
| Extrapolated full cycle | **14.6 h** | 13.8 h |

This is the working baseline for the plan's state 3 ("map screen, phone
connected"). It agrees with ride 1 above (~46 mA) by a route with completely
different contamination, which is why it is trusted where either ride alone was
not.

### What the counters say the money went on

Deltas across the 11.42 h window. This is the part that was previously inferred
from the code and is now measured:

| Counter | Over the run | Meaning |
|---|---|---|
| `throttled_ms` | 1.55 % of wall | CPU held 160 MHz for **98.5 %** of the run |
| `loops` | 97.4 Hz | ~100 Hz loop, as read |
| `loop_busy_ms` | 1.34 % of wall | the loop does work 1.3 % of the time |
| `panel_busy_ms` | 0.89 % of wall | 62 refreshes/h, `ref_full` = **0**, nearly all `ref_window` |
| `ble` | 2 in 765 of 777 rows | link connected all day |

**Real work took ~2.2 % of the day.** The rest went on idling at 160 MHz in
`delay(10)`, plus the radio. Not one full panel refresh in 13 hours -- the panel
is not where the power goes on a stationary day.

**Stationary makes 14.6 h a ceiling**, not a trail expectation: a walking hiker
pays for viewport resets this run did not.

The three-day (72 h) endurance target, the 9.0 mA budget it implies, and the
routes to it live in [`power-plan.md`](power-plan.md). Run 1's full record is in
that file's Runs section.

## A connected BLE link survives 10 MHz

**Measured on hardware, found 2026-08-15** in `power.csv` data that was already
on the card -- no run was made for it.

One boot logged **466 of 513 minute rows with `cpu_mhz=10` and `ble=2` at the
same time**, about 7.8 hours. `ble=2` means `connIntervalMs() > 0`
(`src/PowerLog.cpp:25-27`), so that is a live connection, not advertising.

This does **not** contradict the `NimBLEDevice::init()` finding below. That one
is about *entering* low-power mode before init; this is about steady state with
a connection already up. Both hold.

Two limits on the same boot, so it is not over-read: it was **not the map
screen** (different heap profile, and `MapActivity::preventAutoSleep()` would
have pinned the clock), and it contains **two charging jumps**, so its apparent
3.0 mA draw is contaminated and is not a throttled-draw figure. Only the
link-survives-10-MHz claim comes out of it.

## Where the power goes on the map screen -- read off the code

**Read, 2026-08-11. None of this is measured**; it is the list a measurement
run should try to confirm or kill, ordered by how much it is suspected to cost.
`power-plan.md` carries the actions.

**1. The BLE controller never sleeps its radio.**
`sdkconfig.default:947` -- `# CONFIG_BT_CTRL_MODEM_SLEEP is not set`, with
`CONFIG_BT_CTRL_SLEEP_MODE_EFF=0` and `CONFIG_BT_CTRL_SLEEP_CLOCK_EFF=0`
(`:951-952`). Without controller modem sleep the BT baseband stays powered
between connection events instead of powering down and waking for each one.
This is a `custom_sdkconfig` entry (`platformio.ini:85`), not a code change.
Open: whether the X4 carries an external 32.768 kHz crystal, which decides
which low-power clock mode is available.

**2. No ESP-IDF power management at all.** `sdkconfig.default:1634` --
`# CONFIG_PM_ENABLE is not set`. So there is no dynamic frequency scaling and
no tickless idle; the CPU runs at whatever `HalPowerManager` last set, and the
idle task waits at that clock.

**3. The map screen held full clock and the 10 ms loop for the whole ride.**
~~`MapActivity::preventAutoSleep()` returns `BlePositionServer::isRunning()`,
and `main.cpp` resets `lastActivityTime` and calls `setPowerSaving(false)` on
it every iteration, so the `delay(50)` + `setPowerSaving(true)` branch is
unreachable while the map is up.~~

**Confirmed, then fixed.** `throttled_ms` was named here as the counter that
would prove or refute it, and run 1 did: **1.55 % of an 11.4 h day**, i.e. the
map held 160 MHz for 98.5 % of it (see the state-3 baseline above). The split
described in "The throttle and the sleep guard are two questions" below
landed 2026-08-16.

**4. ~100 wakeups a second, each with two ADC reads.**
`InputManager::update()` samples both button-ladder pins every call
(`freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:137,144`), and
`main.cpp` calls it once per iteration. In hike mode a fix arrives every 10 s,
so that is roughly a thousand ladder samples per useful event.

**5. Advertising parameters are left at the NimBLE default.**
`BlePositionServer::begin()` sets a service UUID, a scan response and a name
(`lib/BlePositionServer/src/BlePositionServer.cpp:301-317`) but never
`setMinInterval`/`setMaxInterval`, and never a TX power. A device sitting with
the map up and no phone connected therefore advertises at the default rate
indefinitely. The exact default interval is unverified.

**6. Panel refresh cost is unknown.** A full refresh is the single most
expensive event the device has and nothing has measured it in mA or in joules.
`MapFollow::kMaxPartialMoves = 12` decides how often one happens
(`src/activities/map/MapFollow.h:41-45`) and is still untuned, as its own
comment says. `PowerTelemetry`'s `ref_*` and `panel_busy_ms` counters now make
the *frequency* half of this measurable per ride.

## Connection parameter requests while connected -- shipped as an improvement, measured as a regression (T6.2, 2026-08-13 -> fixed 2026-08-14)

Item 5 above is about advertising with nobody connected; this is its cousin
for the connected-idle case docs/ble-review-2026-08.md's "Power" bullet
flagged: "device never requests connection parameters... connected-idle runs
at the phone's 30 ms interval to carry 1 write/s -- ~33 radio events/s".
`serviceAdvertising()` (`lib/BlePositionServer/include/BlePositionServer.h:154`)
takes a `transferActive` bool and calls `serviceConnParams()`
(`lib/BlePositionServer/src/BlePositionServer.cpp:756`, T6.2) whenever a phone
is connected, requesting one of two parameter sets. **Read off the code**, T6.2
shipped:

- **Idle set** -- 24-40 units (30-50 ms), latency 9, timeout 600 units (6 s).
  Requested 5 s after connect, or 5 s after a transfer ends, whichever the
  code is timing (`connParamsQuietSinceMs_`).
- **Fast set** -- 12-24 units (15-30 ms), latency 0, timeout 400 units (4 s).
  Requested the tick a file transfer begins (`MapTransferReceiver`'s
  `active_` flag, read via `Status::active`).

T6.2's own justification for the 4/6 s timeouts was `(1 + latency) *
maxInterval * 2` -- 60 ms and 1000 ms worst case respectively -- against the
requested timeout. That arithmetic is correct for *missed connection events*
and says nothing about the peripheral being unavailable for **seconds**, which
is the failure mode that actually showed up.

**Measured on hardware, 2026-08-14, from the maintainer's live ride** (session
log pulled off the phone,
`/sdcard/Android/data/org.explorink.gpsbridge/files/explorink-gps-20260814-145110.jsonl`):

- **57 disconnects, 57/57 `gatt_status: 8`** -- Android's `GATT_CONN_TIMEOUT`,
  HCI reason `0x08`, supervision timeout. Not one disconnect from any other
  cause.
- **Zero `gatt_stack_dead`, zero `link_dead`** -- the app's own 37 s dead-link
  verdict never fired, so this was not a software timeout; the link died at
  the link layer.
- `gatt_timeout` 119 and `gatt_late` 119, exactly equal: every timed-out
  operation eventually got its late callback, 5.9-7.2 s after being issued.
  The device always answered, just late -- overrunning the 3 s command
  budget, not the 10 s transfer budget.
- **The 4 s request was being applied by the phone.** Parameter sets reported
  by `onConnectionUpdated`: `interval=24 latency=0 timeout=400` 34x,
  `interval=24 latency=9 timeout=600` 28x, plus the phone's own
  `interval=12 latency=0 timeout=500` 173x.
- **29 of the 57 disconnects happened during an open fetch; 28 happened with
  no fetch in flight at all** (i.e. against the idle set) -- not only a
  render-collision story; a 4 s supervision timeout is simply too short for a
  moving vehicle.

**The fix (this section, 2026-08-14):** both timeouts raised to 2000 units
(20 s), and `kConnParamsIdleLatency` lowered from 9 to 4
(`lib/BlePositionServer/include/BlePositionServer.h`, see the comments on
`kConnParamsIdleTimeoutUnits`, `kConnParamsFastTimeoutUnits` and
`kConnParamsIdleLatency` for the full arithmetic). 20 s clears the
maintainer's stated worst case -- a map render plus panel refresh must be
assumed to reach 10 s -- with a 2x margin (2.7x against the slowest render
measured on the bench, a 7463 ms rung-0 frame: parent repo,
`docs/render-gate/stream-crc/manifest.json`, `"renderMs": 7463`, and
`docs/street-labels-plan.md:82`. That is a render-gate figure, not a number
from the 2026-08-14 ride, and it is not recorded in this submodule's
`docs/place-labels.md`); stays inside
the BLE supervision-timeout range (0x000A-0x0C80, 100 ms-32 s) with 12 s to spare;
and sits 10 s below Android's own 30 s ATT transaction timeout, past which
Android tears the bearer down itself regardless of what this device asks for.
Latency was lowered because its power saving was never measured (the H8 power
soak, `docs/ble-fix-plan.md`, never ran) while its cost -- more
legitimately-skipped listens for a marginal-RF hiccup to stack on top of -- is
real; it was not, on this evidence, the mechanism of the outage (latency 9's
own worst-case gap was 500 ms, nowhere near even the old 6 s timeout).

**Cost of the fix:** a larger supervision timeout means the device believes a
departed phone is still present for longer before `onCentralDisconnect` fires
and `resetAdvertisingPhase()` restarts the two-phase advertising clock
(T6.1, `kFastAdvertisingMs = 30000` at
`lib/BlePositionServer/include/BlePositionServer.h:443`). Worst case, that is
now up to 20 s of "still connected" instead of 4-6 s -- 14-16 s later into the
30 s fast-advertising window than before this change, ~50 s total from actual
departure to falling back to slow advertising instead of ~36 s.

Both are requests, not commands: `onConnParamsUpdate()`
(`BlePositionServer.cpp:164-169`) logs whatever interval/latency/timeout the
phone actually granted, which may match neither set -- the applied-params
evidence above (400 applied 34x, but the phone also kept its own 500 173x)
shows a central is not obliged to honour every request. **Open, needs a ride
with the Android app:** whether the new 20 s timeout actually stops the
disconnects (this fix is unverified on hardware as of this write-up), and what
connected-idle current draw looks like with latency 4 instead of 9 -- exactly
the numbers `power.csv`/`stats` above can catch once one is run with this code
on.

## The throttle and the sleep guard are two questions

**Changed 2026-08-16.** Built clean; **not yet verified on hardware.**

`preventAutoSleep()` used to answer two questions with one bool: "do not power
the device down behind my back" and "do not slow the CPU down behind my back".
The map screen needs the first for a whole ride and does not need the second,
so asking for one bought the other -- and run 1 priced it at 98.5 % of an
11.4 h day pinned to 160 MHz while the loop did real work 1.3 % of the time.

Now there are two:

| Method | Question | Map screen |
|---|---|---|
| `preventAutoSleep()` | do not deep-sleep me | **true** while the BLE server runs |
| `preventThrottle()` | do not slow my clock | **false**, except while work is queued |

`Activity::preventThrottle()` defaults to `preventAutoSleep()`
(`src/activities/Activity.h`), so every activity that has not thought about it
behaves exactly as before the split. Only `MapActivity` overrides it.

`main.cpp` keeps two deadlines instead of one -- `lastActivityTime` for the
auto-sleep timeout and `lastFullClockTime` for the throttle. Real user input
still resets both.

`MapActivity::preventThrottle()` returns true only while `redrawDueMs_`,
`arrivalRedrawDueMs_` or an active transfer says work is queued. It is
deliberately **not** true merely because a fix arrived: the phone sends at
1 Hz and run 1 drew 62 times an hour, so un-throttling per fix would reset the
3-second idle timer forever and the split would buy nothing. A fix that moves
the marker less than the follow decision cares about is processed at the low
clock and costs nothing.

**The drawing does not rely on that flag.** Every render entry point --
`showBusy()`, `renderWaiting()`, `renderLoadingTiles()`, `renderCurrent()`,
`renderRouteOverview()` -- calls `MapActivity::kickFullClock()` first, because
a fix can arrive and force a redraw inside the same `loop()` iteration whose
`preventThrottle()` was already polled. A viewport reset is close to two
seconds at 160 MHz and a large share of it is software floating point, so
drawing at 10 MHz would put tens of seconds between a fix and the picture. The
main loop drops the clock again on its own 3 seconds later.

**Open, and the reason this needs a supervised check before a long run:**
nothing has yet confirmed on hardware that the map still redraws promptly, that
the BLE link holds across the clock changes, or that `throttled_ms` actually
rises on the map screen. The last one is the pass/fail signal --
`throttled_ms` near 0 over a map-screen hour means the split did not take.

## Power-saving mode drops CPU frequency after idle

`main.cpp` calls `powerManager.setPowerSaving(true)` after the device has
been idle for a while (no button press, no touch, no tilt, nothing holding
`activityManager.preventThrottle()` true -- that used to read
`preventAutoSleep()`, see the section below). This lowers CPU frequency to save
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
