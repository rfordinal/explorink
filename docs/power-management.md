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
the handle set (`BlePositionServer.cpp:516-520` returns 0 when
`ble_gap_conn_rssi()` fails). So either that HCI path is unavailable on this
build or the call errors for another reason. Pre-existing -- `rssi()` had no
caller before `stats` -- and not yet chased. Open.

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

**3. The map screen holds full clock and the 10 ms loop for the whole ride.**
`MapActivity::preventAutoSleep()` returns `BlePositionServer::isRunning()`
(`src/activities/map/MapActivity.cpp:768`), and `src/main.cpp:719-723` resets
`lastActivityTime` and calls `setPowerSaving(false)` on it every iteration.
The end of `loop()` then always takes the `delay(10)` branch -- the
`delay(50)` + `setPowerSaving(true)` branch is unreachable while the map is
up. `PowerTelemetry`'s `throttled_ms` is the counter that proves or refutes
this on hardware: it should read 0 for a whole ride.

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

## Connection parameter requests while connected -- fix, unmeasured (T6.2, 2026-08-13)

**Code change, not yet measured on hardware.** Item 5 above is about
advertising with nobody connected; this is its cousin for the connected-idle
case docs/ble-review-2026-08.md's "Power" bullet flagged: "device never
requests connection parameters... connected-idle runs at the phone's 30 ms
interval to carry 1 write/s -- ~33 radio events/s". `serviceAdvertising()`
(`lib/BlePositionServer/include/BlePositionServer.h:154`) now takes a
`transferActive` bool and calls `serviceConnParams()`
(`lib/BlePositionServer/src/BlePositionServer.cpp:734`) whenever a phone is
connected:

- **Idle set** -- 24-40 units (30-50 ms), latency 9, timeout 600 units (6 s).
  Requested 5 s after connect, or 5 s after a transfer ends, whichever the
  code is timing (`connParamsQuietSinceMs_`).
- **Fast set** -- 12-24 units (15-30 ms), latency 0, timeout 400 units (4 s).
  Requested the tick a file transfer begins (`MapTransferReceiver`'s
  `active_` flag, read via `Status::active`).

Both are requests, not commands: `onConnParamsUpdate()`
(`BlePositionServer.cpp:164-169`) logs whatever interval/latency/timeout the
phone actually granted, which may match neither set. **Open, needs a ride
with the Android app:** whether either request is honoured at all, and what
connected-idle current draw looks like before/after -- exactly the number
`power.csv`/`stats` above can catch once one is run with this code on.

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
