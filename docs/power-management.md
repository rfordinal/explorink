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

## A windowed update costs the same panel time as a fast full refresh

**Derived from run 2's counters, 2026-08-16. Not directly measured per type --
see the limit at the end.**

Nearly every refresh on the map is a windowed one. Run 2, over 13.19 h:

| Type | Count | Share | Static 10:30-20:00 | Driving 20:00-23:50 |
|---|---|---|---|---|
| `ref_window` | 2948 | **95.0 %** | 1138 (98.1 %) | 1809 (93.2 %) |
| `ref_fast` | 153 | 4.9 % | 21 (1.8 %) | 132 (6.8 %) |
| `ref_half` | 0 | 0 % | 0 | 0 |
| `ref_full` | **1 all day** | 0.0 % | 1 | 0 |

`ref_window` is a marker move. `ref_fast` is a viewport reset -- the marker
left the keep-in area and the frame re-centred -- which is why it goes from
2/h standing still to **36/h driving** across tiles.

### The cost does not follow the area

`panel_busy_ms / total refreshes` comes out at **507 ms in every phase**, even
though the type mix differs sharply (98 % window standing still, 93 % driving).
If a windowed update were cheap, the static phase would average lower. It does
not.

Two phases with different mixes give two equations and two unknowns:

```
static:   1138*w +  21*f = 588 000 ms
driving:  1809*w + 132*f = 983 000 ms
```

Solving: **`ref_window` ~= 508 ms, `ref_fast` ~= 490 ms.** A partial update of
a marker patch and a fast refresh of the whole 800x480 panel cost the same
panel time.

So the driver's cost is set by the **waveform's frame count, not by how many
rows changed**. Area is free; the number of refreshes is what is paid for.

### What it means for power and for MapFollow

The panel was busy **119 s per hour** in run 2 (3.31 % of wall), and **95 % of
that time went on marker moves**. Not dominant next to the CPU, but not the
rounding error it looked like after run 1, which saw only 69 refreshes/h
against run 2's 235.

It also inverts a design assumption. "Prefer many small partial moves over one
viewport reset" only saves anything if partial moves are cheaper, and they are
not. `MapFollow::kMaxPartialMoves` and the keep-in margin should be tuned to
minimise the **count** of refreshes, not their size -- a reset that buys many
fixes without a redraw can beat a run of partial moves.

### The limit on this finding

Both equations come from one run, split by a wall-clock time the rider
reported, and both phases share whatever fixed overhead the driver has. The
numbers are consistent across three independent phase splits, which is why they
are quoted, but nothing measured a single refresh of each type directly.

**What would settle it:** the same route driven twice with deliberately
different keep-in margins, so the window/reset ratio changes while everything
else holds. Or a bench run that issues N windowed updates and N fast refreshes
and differences `panel_busy_ms` across each.

**Checked against the split point, 2026-08-17.** The derivation rests on a phase
boundary taken from the rider's report, not from the data, so it was re-solved
for every split between 8.5 h and 10.5 h. `ref_window` came out **507-511 ms**
across all of them -- it barely depends on where the cut goes, because window
updates dominate the counts on both sides. `ref_fast` is softer, 435-497 ms. So
the headline ("both cost about the same; area does not set the price") is
robust; the exact `ref_fast` figure is not.

## BLE modem sleep cuts the draw a third (measured 2026-08-16)

**Measured on hardware, two full-day runs.** `CONFIG_BT_CTRL_MODEM_SLEEP` was
off in this tree, so the BLE baseband stayed powered between connection events
for entire rides. Turning it on (mode 1, main XTAL) is the campaign's first
confirmed saving.

Compared over the **same voltage band both runs covered** (4178 -> 3866 mV) --
whole-run figures flatter the newer run because run 1 continued into the steep
tail below 3748 mV:

| | Run 1, no modem sleep | Run 2, modem sleep |
|---|---|---|
| Time for the same drop | 6.20 h | **9.48 h** |
| Slope | 50.3 mV/h | **32.9 mV/h** |
| Draw at 650 mAh spec | 35.6 mA | **24.0 mA** |
| Refreshes/h | 65 | 122 |
| `loop_busy_ms` | 1.50 % | 5.81 % |

**33 % less draw while doing about twice the panel work and four times the
loop work.** The saving at equal workload is larger than 33 %; these two runs
cannot separate the two effects, and only a static repeat would.

Run 2's second half was a car drive (526 refreshes/h, 21.8 % loop busy) and
still cost 29.9 mA -- less than run 1's *static* 35.6 mA.

Config lives in `platformio.ini`'s `custom_sdkconfig` block. Verify it compiled
in rather than merely requested: `CONFIG_BT_CTRL_SLEEP_MODE_EFF` and
`CONFIG_BT_CTRL_SLEEP_CLOCK_EFF` must read `1` in the generated
`sdkconfig.default`. Those are the values
`BT_CONTROLLER_INIT_CONFIG_DEFAULT` hands the controller.

**What it does not touch:** the CPU. `throttled_ms` was 0.02 % of run 2, so the
map held 160 MHz all day. The CPU is now the largest remaining component, and
an 80 MHz floor is the next thing to try (see "Why 10 MHz breaks BLE").

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

## A connected BLE link does NOT survive 10 MHz (corrected 2026-08-16)

**This section previously claimed the opposite. It was wrong, and the wrong
version is what justified two flashes that both hung the device.** The
correction is kept in place of the claim rather than deleted, because the bad
inference is the lesson.

**Measured on hardware 2026-08-16, with a control run.** Two runs, same rig
(`tools/blefakephone.py` sending a fixed position, serial captured throughout):

| Build | Result |
|---|---|
| Throttle split (`158a2bc4`) | 1 fix received, then the link died. `[PWR] Going to low-power mode` at 12856 ms, then **total serial silence** for the rest of the run. Never recovered, not even after the 20 s supervision timeout should have dropped the link and restored full clock. Hung. |
| Logging build (`40cc5087`), control | **22 fixes** over ~2 minutes, link held throughout, device still logging after the central left. |

The control is what makes this conclusive: the rig is sound, so the difference
is the firmware. **Throttling to 10 MHz while a central is connected kills the
link and hangs the device.**

### What the old evidence actually was

The refuted claim came from reading `power.csv`: one boot logged 466 of 513
minute rows with `cpu_mhz=10` and `ble=2` together, ~7.8 hours, and `ble=2`
means `connIntervalMs() > 0` (`src/PowerLog.cpp:25-27`).

That is not evidence of a healthy connected link at 10 MHz. `connIntervalUnits_`
is cleared in `onCentralDisconnect()` (`BlePositionServer.cpp:709`), which runs
from NimBLE's disconnect callback -- so if the link dies in a way that callback
never services, the field stays non-zero and `ble` keeps reading 2 with nothing
connected. Which is exactly the state the 2026-08-16 runs produced.

**The lesson, not the number:** a counter derived from a cached field is only
as good as the path that clears it. `ble=2` proves the device *believes* a
central is connected, and nothing more. Anything built on that belief needs a
second, independent signal -- traffic arriving, in this case.

## (superseded) A connected BLE link survives 10 MHz

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

## Letting the map screen throttle hung the device solid (2026-08-16, reverted)

**Measured on hardware. The attempt is reverted; the finding is not.**

`preventAutoSleep()` answers two questions with one bool -- "do not sleep me"
and "do not slow my clock". Run 1 priced the second: the map held 160 MHz for
98.5 % of an 11.4 h day while the loop did real work 1.3 % of the time. The
obvious fix is to split the bool so the map can decline the clock pin and keep
the sleep guard (`optimization/07-power-and-lifecycle.md`, step 2).

That was built (`b8b8f307`), flashed, and **hung the X4 solid on first use** --
screen frozen, no recovery but the ROM bootloader.

**The cause was already written down in this file, one section below.**
`NimBLEDevice::init()` hangs in low-power mode. `main.cpp` guards the two
`CMD:` entry points into the map for exactly that reason and says so in a
comment. The split let the map screen throttle *while it was up*, so any path
that touches NimBLE at 10 MHz -- entering the map, an advertising restart, the
handover to `TileSyncActivity` -- meets the documented hang. The attempt put
`kickFullClock()` on the render entry points and left every BLE path
unguarded.

So the finding is not "the split cannot work". It is:

- ~~**The clock must be at full speed across anything that touches NimBLE**,
  not just anything that draws.~~ Too narrow. The rule is about the **bus**,
  not the call site -- see "Why 10 MHz breaks BLE" below.
- ~~**The steady-state link is fine at 10 MHz**~~ -- **refuted 2026-08-16 by
  attempt 2 and its control run.** The ~7.8 h of `cpu_mhz=10` with `ble=2` was
  a device believing it was connected, not a healthy link. See the corrected
  section above.
- **A supervised bench check comes before any long run.** This hang appeared
  within minutes of the first real use; a day-long run would have been lost to
  it. Still true, and the only bullet here that survived.

Next attempt: raise the clock before `BlePositionServer::begin()`, before
advertising restarts, and across activity transitions that own the peripheral,
then prove on the bench that entering the map and restarting advertising both
survive at 10 MHz -- before anything runs for hours.

## The map throttles to 80 MHz (2026-08-17)

**Built and bench-verified on hardware.** The third attempt at the throttle
split, and the first that works. What changed is not the guard but the floor.

`preventAutoSleep()` used to answer two questions with one bool. Now there are
two, and **safety lives in neither of them**:

| Method | Question | Map screen |
|---|---|---|
| `preventAutoSleep()` | do not deep-sleep me | **true** while the BLE server runs |
| `preventThrottle()` | do not slow my clock | **false**, except while work is queued |

`Activity::preventThrottle()` defaults to `preventAutoSleep()`, so every other
activity is unchanged. `main.cpp` keeps two deadlines, `lastActivityTime` for
auto-sleep and `lastFullClockTime` for the throttle; user input resets both.

### The floor is the safety, not the caller

`HalPowerManager::lowPowerFloorMhz()` asks
`esp_bt_controller_get_status()` on **every** throttle and returns
`BLE_SAFE_FREQ` (80 MHz) whenever the controller is enabled, `LOW_POWER_FREQ`
(10 MHz) otherwise. No call site can put the radio in an unsupported state,
whatever it returns, because the HAL refuses.

That is deliberate. The two earlier attempts placed the duty at the call site
and keyed it on an application-level view of the link -- which went stale and
hung the device. The controller's own status cannot go stale.

`setPowerSaving()` also re-applies when the **floor moves under an existing
throttle**: the controller can come up while the device is already at 10 MHz,
which is exactly what entering the map from an idle Home screen does.
`MapActivity::onEnter()` and `TileSyncActivity::onEnter()` additionally raise
the clock before `BlePositionServer::begin()`, closing the window before the
controller is enabled and the floor starts applying.

### Drawing still runs at 160 MHz

`MapActivity::renderViewport()` calls `kickFullClock()` first. **That seam
matters and the first bench run proved it**: the guard was originally only on
`renderCurrent()` and its siblings, and a 10.6 s viewport reset slipped through
via one of `renderViewport()`'s three direct callers. Moving the guard into
`renderViewport()` itself took the identical scene (107,930 points projected)
from **10,576 ms back to 6,075 ms**.

### Bench results

Four runs with `tools/blefakephone.py`, serial captured throughout:

- Throttle engages with a central connected -- `[PWR] Going to low-power mode
  (80 MHz)` -- and the link survives it. **44, 45 and 33 fixes** across three
  runs, device still logging after the central left every time.
- `throttled_ms` **93.8 % of wall** over a 299 s run (`stats`), against 0.02 %
  in run 2. The throttle genuinely engages.
- The clock rises for a render and falls back afterwards, four transitions in
  one run.
- A fresh connect after the device had been running and throttling works.

**Transfers were not exercised on this build.** `preventThrottle()` returns
true while `transfer_.status().active`, so a transfer already under way holds
full clock. But the previous build had no split at all -- it ran at 160 MHz
throughout -- so its two verified transfers say nothing about the new path:
**a transfer that begins while the device is throttled at 80 MHz is untested on
any build.** The bench never produced one, because the test route already had
its tiles on the card.

**What would settle it:** clear a tile from the card, or drive the fake phone to
an area with none, and confirm a transfer starts and completes from a throttled
state.

**Not measured: the saving.** USB charges, so only a full unplugged run prices
it. Run 2's slope was 32.6 mV/h at 24.0 mA with the CPU at 160 MHz all day; the
CPU is the largest remaining component and this halves its clock for ~94 % of
the time.

**A note on the rig:** the laptop's BlueZ stack accumulated failures across
~15 connect cycles in one session, failing at `start_notify` with "Unlikely
Error" while the device log showed a clean connect, subscribe and MTU
negotiation. `bluetoothctl power off; power on` cleared it. Do not read a
host-side GATT error as a device fault without checking the device's own log.

**Verified again from a clean boot on the merged build** (`3c6644c3`):
throttles to 80 MHz at 11.5 s, the central connects **while throttled**, 30
fixes, the clock rises for a render and drops back. That is the state the
device was left in.

### Open: advertising stopped after a long test session

**Observed once, 2026-08-17, not explained.** After roughly 25
connect/disconnect cycles and several reflashes in one sitting, the device sat
on the map screen answering `stats` over serial but **not advertising** -- two
`blefakephone` runs in a row reported "no device advertising the map service".

It is not the laptop: a `BleakScanner.discover()` at that moment saw 44 other
BLE devices. It is not the build either, in any simple sense: a device reset
brought advertising straight back and everything worked.

So something in the advertising restart path does not recover from whatever
that sequence produced. The area already has explicit failure handling --
`advertisingDown_`, `retryAdvertising()`, `serviceAdvertising()` in
`lib/BlePositionServer/` -- so this is a data point against it, not a new
mechanism.

**What would settle it:** reproduce with the serial log running through the
whole cycle sequence and watch `onAdvertisingState()` and the retry path.
Nothing here says whether a rider could hit it -- a ride has far fewer
disconnects than a test session -- but it should not be filed as a rig
problem, because the rig was demonstrably working.

## Why 10 MHz breaks BLE: APB, and a lock that is compiled out

**Read off the code 2026-08-16 (ESP-IDF and Arduino core sources), explains
every measurement we have.** This is the mechanism behind both hangs.

1. `setPowerSaving(true)` calls `setCpuFrequencyMhz(10)`
   (`lib/hal/HalPowerManager.cpp:47`, `LOW_POWER_FREQ` at
   `lib/hal/HalPowerManager.h:31`).
2. At 10 MHz the CPU source becomes the crystal, and `rtc_clk_cpu_freq_to_xtal`
   ends by calling `rtc_clk_apb_freq_update(cpu_freq)` -- **APB drops with the
   CPU**. At 80 and 160 MHz the source is the PLL and **APB stays pinned at
   80 MHz**.
3. The ESP32-C3 BLE controller states its requirement as an
   `ESP_PM_APB_FREQ_MAX` lock: taken for as long as the controller is enabled,
   and re-taken before every modem-sleep wakeup.
4. **Every one of those lock sites is inside `#ifdef CONFIG_PM_ENABLE`, and
   `CONFIG_PM_ENABLE` is not set** (`sdkconfig.defaults:1680`). So the
   controller's only defence against a non-80 MHz APB compiles out of this
   firmware, and `setCpuFrequencyMhz()` will pull APB out from under a live
   controller with nothing objecting.

So the rule is not "raise the clock around NimBLE calls". It is:

> **Never take the CPU below 80 MHz while the BLE controller is enabled.**
> 80 and 160 MHz are both PLL-sourced and both leave APB at 80 MHz. 10 MHz is
> not a slower version of the same state -- it is a different bus
> configuration that Espressif never supports outside the PM-lock system.

That also collapses two findings into one. The verified
`NimBLEDevice::init()` hang and the 2026-08-16 steady-state hang are the same
violation; init was simply the first register conversation with the BT MAC, so
it was hit first, back in 2026-08-04.

**Open, and cheap to settle:** whether the failure leaves the CPU hung or only
the controller dead. `power.csv` from the attempt-2 bench boot decides it --
rows continuing past the throttle mean the CPU lived and only the radio died.
The card has not been read since.

**Consequence for the campaign:** the throttle split is not dead, it was aimed
at the wrong floor. An 80 MHz floor while the controller is up is safe by
construction and needs no PM machinery.

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
`NimBLEDevice::init()`, `lib/BlePositionServer/src/BlePositionServer.cpp:253-254`)
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

### And starves RX outright

**Measured 2026-08-16.** The TX half above is the known one. The receive side is
worse: at 10 MHz the device does not accept `CMD:` at all. A host can write
`CMD:GOTO_MAP` repeatedly for 45 s and see nothing but the 10 s heartbeat log,
while the same command lands instantly in the ~3 s full-clock window after a
reset.

So any tool that drives the device over serial must either send inside that boot
window, or wake the device another way first. `main.cpp`'s handlers call
`setPowerSaving(false)` *after* the line is read, which does not help a line that
never arrives.

With the 80 MHz floor this only applies where the floor does not: screens with
the BLE controller down, Home among them.

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
