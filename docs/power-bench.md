# Pricing a feature on the bench: what it costs to have the light on

**What this file is.** The method for measuring what one feature costs on the
T5 S3 Pro in minutes rather than in a 13-hour discharge run, the commands it
needs, and every number it has produced so far.

The one rule that makes it work: **read the difference between two states, never
the absolute number.** A USB meter on VBUS reads the board through the charger's
buck converter, so an absolute reading carries an unknown efficiency (roughly
0.8x at a 4.1 V system rail, `[open]` below 50 mA) and up to 3 mA of the
charger's own draw. Both of those are identical in two states taken minutes
apart, so both cancel in a subtraction. What is left is the feature.

An absolute number still needs a discharge run -- runtime to empty, or the
gauge's own current logged to the card (T-250, T-282 in the parent repo). This
bench says *what to fix*; a discharge run says *what the rider gets*.

## The run

```
CMD:CHARGE OFF          # the charger stops adding its own current to the reading
CMD:LIGHT 0             # state A
CMD:BLE OFF
                        # meter 30 s
CMD:BLE ON              # state B -- exactly one thing changed
                        # meter 30 s
CMD:BLE OFF             # state A again, and this is not optional
                        # meter 30 s
CMD:CHARGE ON           # leave the board charging
```

Read the meter with `python3 tools/usbmeter_read.py --seconds N` in the parent
repo -- the only reader that does not hang the JT-UM120.

**Always A-B-A.** The device does things on its own -- a refresh, a sync check, a
CPU frequency change -- and a single A-B pair cannot tell a feature's cost from
the board drifting under it. Three controls 90 s apart agreed to **0.42 mA** on
the run below, which is what makes a 11.65 mA answer believable.

**Check `chg=0` again at the end.** If the watchdog were ever re-armed, charging
comes back 40 s in and half the run is a different measurement
([`charge-control.md`](charge-control.md)).

**Compare watts, not milliamps, when the states differ a lot.** VBUS sags under
load -- 5.249 V at idle against 5.233 V with the frontlight at 100 % -- so a
milliamp difference quietly mixes in a voltage difference. At these loads it
changes the answer by under 1 %, and at a panel refresh it would not.

## CMD:BLE

```
CMD:BLE       ->  BLE:running=0
CMD:BLE ON    ->  BLE_OK:on running=1
CMD:BLE OFF   ->  BLE_OK:off running=0
```

It exists because until 2026-09-12 the radio came up only as a side effect of
entering the map, so the only measurable pair was "home screen" against "map with
BLE" -- a difference that is tiles, a renderer and a panel refresh as much as it
is a radio.

**Use it on the home screen, not on the map.** Nothing asks who owns the radio:
`begin()` is idempotent and `end()` is unconditional, so an `OFF` issued while
the map is open takes the map's own channel down and the map will not notice
until it is left and re-entered.

Devel builds only (`-DENABLE_BLE_CMD=1`, `env:t5s3pro`): `ON` starts an
unauthenticated command channel with nothing on the screen to say so (T-222 in
the parent repo).

## CMD:WIFI

```
CMD:WIFI                 ->  WIFI:mode=0 conn=0 rssi=0 ip=0.0.0.0 clients=-1
CMD:WIFI OFF             ->  WIFI_OK:off
CMD:WIFI STA             ->  WIFI_OK:sta mode=1          (radio up, associated to nothing)
CMD:WIFI AP              ->  WIFI_OK:ap ssid=<x> ip=<y>
CMD:WIFI SCAN            ->  WIFI_OK:scan n=<x> ms=<y>
CMD:WIFI CONNECT [ssid]  ->  WIFI_OK:connect ssid=<x> rssi=<y> ip=<z> ms=<t>
```

Same reason `CMD:BLE` exists, for the other radio. Every WiFi state this
firmware has is reached through a screen that also repaints, scans or serves a
page -- the sync screen, the web server, OTA, the font download -- so none of
them is one thing changing.

`CONNECT` with no argument uses the network the menu last joined. It reads the
credential store and never writes it, so a bench run cannot change which
network the device prefers.

**A WiFi state is never a clean CPU state.** `HalPowerManager::setPowerSaving()`
forces power saving *off* whenever `WiFi.getMode()` is not `WIFI_MODE_NULL`
(`HalPowerManager.cpp:59-64`). So every WiFi reading carries a full-speed CPU as
well as a radio, and the pair "WiFi off" / "WiFi on" is two changes, not one.
Price the clock with `CMD:CPU` and subtract it, or the radio gets billed for
both.

## CMD:CPU

```
CMD:CPU              ->  CPU:mhz=80 hold=0 held=0
CMD:CPU HOLD 240     ->  CPU_OK:hold mhz=240 held=1
CMD:CPU AUTO         ->  CPU_OK:auto mhz=240
```

The loop throttles to `HalPowerManager::LOW_POWER_FREQ` three seconds after the
last input, which on this board is 80 MHz. So an untouched bench state is at
80 MHz, a state holding WiFi up is at full speed, and any pair spanning those
two differs by a clock as well as by whatever was being measured. This is what
separates them.

**80, 160 and 240 only.** Below 80 the CPU leaves the PLL for the crystal, which
drags APB down with it and takes flash and PSRAM timing with it on an S3. Both
are correctness bounds `HalPowerManager.h` already documents (`LOW_POWER_FREQ`,
`BLE_SAFE_FREQ`); this command must not be the one place that walks past them.

The hold is a `HalPowerManager::Lock`, and the manager allows exactly one at a
time. **`held=0` in the reply means something else holds it** and the next
throttle will throw the clock away -- a run taken against `held=0` does not
measure what its label says.

## CMD:REFRESH

```
CMD:REFRESH                        ->  REFRESH:modes=fast,half,full patterns=flip,light,none
CMD:REFRESH fast 30                ->  30 refreshes, no gap, whole-panel flip
CMD:REFRESH half 20 5000           ->  20 refreshes, 5 s apart
CMD:REFRESH fast 30 2000 light     ->  a tenth of the bytes change between frames
CMD:REFRESH fast 30 2000 none      ->  nothing changes between frames
```

Each frame prints `REFRESH:i=<n> ms=<t>`, and the run ends with
`REFRESH_END:... total_ms mean_ms min_ms max_ms span_ms`.

**Why a run and not one refresh.** The meter samples about 66 times a second, so
a single ~1,100 ms whole-panel frame on this board is roughly 70 samples with a
state change at each end. One frame is a spike to be integrated, not a level to
be read. A run of them at a fixed cadence *is* a level: (block mean - control
mean) x block length / count is the energy one refresh costs.

**The pattern is part of the measurement, not a detail.** `FAST` is differential
on both panels this firmware drives ([`refresh-modes.md`](refresh-modes.md)): it
moves only the pixels that differ from the previous plane. So `none` asks what
the *call* costs with nothing to do, `flip` asks what the panel costs when every
pixel moves, and `light` sits nearer a map redraw. Quoting one of them as "the
cost of a refresh" without saying which is how a number becomes folklore.

It writes straight into the framebuffer the panel already owns, exactly as
`CMD:SHOWIMAGE` does, under a `RenderLock` held for the whole run. Whatever was
on screen is destroyed and the next activity repaint cleans it up.

## Driving the whole thing unattended

`tools/power_campaign.py` in the parent repo runs a list of states end to end,
holds each one for a fixed block, keeps the meter logging across all of them,
and writes one `marks.jsonl` line per block with the wall-clock window and the
`chg` read-back at both ends. `--report` reads the log back against those marks
and prints each state against the controls either side. Thirty states by hand is
thirty chances to mistype a state, skip a control, or lose the offset a window
gets cut at -- and the device is the thing being measured, so anything typed at
it during a block is part of the reading.

## What has been priced

**2026-09-12, T5 S3 Pro, `t251-charge-off`, `env:t5s3pro`, JT-UM120 inline on
VBUS, home screen, charging disabled, nothing else touched between states.**
Each state 25 s of samples at ~66/s.

| state | VBUS | power | against baseline |
|---|---|---|---|
| idle, light off, BLE off | 93.6 mA at 5.25 V | 491 mW | -- |
| **BLE advertising** | 105.5 mA | 553 mW | **+11.7 mA, +61 mW** |
| **frontlight 40 %** | 123.1 mA | 645 mW | **+29.7 mA, +155 mW** |
| **frontlight 100 %** | 165.9 mA | 868 mW | **+72.4 mA, +377 mW** |

Controls: 93.85, 93.43, 93.58 mA, taken at the start, the middle and the end.

**The frontlight's current is linear in PWM duty, and the UI percent is not the
duty.** Measured across all ten rungs on 2026-09-15 (below). The reading of this
25 s pair as "40 % costs 41 % of 100 %, so the UI number behaves like a duty"
was right about this build and wrong as a general claim: `t251-charge-off` pins
`freeink-sdk` at `55a49587`, which has **no gamma correction in
`FrontlightManager`** at all, so `CMD:LIGHT 40` really was 40 % duty there. The
gamma table landed in the SDK on 2026-08-18 (`a4976be`, gamma 1.6554) and
`955b2530` -- what `develop` and `release/lilygo-t5-s3-pro` carry -- has it. On
that pin `CMD:LIGHT 40` is **22 %** duty and costs 23 % of what 100 % costs.

Both runs measure the same law from different points, which is what makes it a
law rather than a fit: 0.400 duty gave 0.41 of full, 0.219 duty gave 0.231 of
full.

**BLE advertising at 11.7 mA is the first measurement of that radio on this
board at all.** T-250 guessed 10-20 mA for BLE *connected* at a 30 ms interval;
advertising is the cheaper half of that range's floor. **This figure did not
reproduce on 2026-09-15** -- see the sweep below.

## The full sweep, 2026-09-15

**T5 S3 Pro, `power-spectrum` at `a2853ee1`, `env:t5s3pro`, JT-UM120 inline on
VBUS, charging disabled the whole run, home screen unless the row says
otherwise.** Driven by `tools/power_campaign.py --block 300 --trim 30` in the
parent repo: five minutes a state, ~17,500 samples a block, controls between
groups. Run directory and its caveats:
`../../docs/power-runs/2026-09-15-t5s3pro-spectrum/`.

### The frontlight, every rung

Baseline 91.5 mA at 5.25 V, and the two controls bracketing this group agreed to
**0.00 mA**.

| UI % | PWM duty | duty fraction | +mA at VBUS | +mW at VBUS |
|---|---|---|---|---|
| 10 | 6 | 0.024 | +2.55 | +10.6 |
| 20 | 18 | 0.071 | +5.79 | +28.1 |
| 30 | 35 | 0.137 | +10.62 | +53.9 |
| 40 | 56 | 0.220 | +16.54 | +85.5 |
| 50 | 81 | 0.318 | +23.37 | +122.1 |
| 60 | 109 | 0.427 | +31.24 | +163.1 |
| 70 | 141 | 0.553 | +40.12 | +209.2 |
| 80 | 176 | 0.690 | +49.85 | +261.4 |
| 90 | 214 | 0.839 | +60.45 | +317.1 |
| 100 | 255 | 1.000 | +71.59 | +375.2 |

**The duty column is read off the device, not derived.** `FrontlightManager`
logs `apply: brightness=<pct> level=<n> totalDuty=<d>` on every change, and the
campaign's `serial.log` carries one line per rung. It also settles the PWM
width: `totalDuty=255` at 100 % means **8 bits** on this board. (The `level`
field in that line is `_brightnessLevel`, which is set but unused here --
`_useLevel` is false, so the duty comes from `perceptualDuty()`.)

One straight line fits all ten points:

```
+mA at VBUS = 0.90 + 70.85 x duty      worst residual 0.16 mA
+mW at VBUS = 2.60 + 374.0 x duty      worst residual 1.4 mW
```

**The intercept is the finding, not the slope.** 0.90 mA is what the
`PT4103B23F` boost driver costs for being switched on at all, before it lights
anything: a third of the whole cost of the light at 10 %, one per cent of it at
100 %. It is why the naive "mA per per cent" column falls from 108 to 72 across
the table -- that is the fixed term being divided by a growing duty, not the LED
string behaving differently.

So the cost of any rung is `0.90 + 70.85 x GAMMA_TABLE[pct] / 65535` mA at VBUS,
and the gamma table is
`freeink-sdk/libs/hardware/FrontlightManager/src/FrontlightManager.cpp`.

**This answers the half of T-250 that was still open.** That task asked whether
the UI's "40 %" is a PWM duty or a step index, and how the boost driver's draw
scales with duty. It is a duty, reached through a perceptual curve; the draw is
linear in it with a fixed offset.

### The CPU clock

Frontlight off, every radio down, the clock pinned with `CMD:CPU HOLD`.

| clock | +mA at VBUS | +mW |
|---|---|---|
| 80 MHz (the idle floor on this board) | -- | -- |
| 160 MHz | +4.31 | +24.0 |
| 240 MHz | +9.48 | +49.2 |
| left to the power manager | -0.20 | -0.4 |

About **0.058 mA/MHz**, straight to within 0.34 mA. The `AUTO` row lands on the
80 MHz row, which is the check that matters: it says the loop really does
throttle three seconds after the last input, and that pinning the clock is not
hiding a state the device would not otherwise be in.

**This row is the reason the WiFi rows below are readable at all.**

### BLE

| state | +mA at VBUS | +mW |
|---|---|---|
| radio down | +0.19 | -1.4 |
| advertising, nothing connected | **+30.86** | +159.0 |

**Advertising costs 30.9 mA on this board, and this run did not measure
anything else about BLE.** Three further blocks are labelled "connected" in
`marks.jsonl` and they are not: `tools/blefakephone.py` died at import in every
one of them -- a fresh parent worktree has no `mapbuilder/tilegen` checkout and
the tool imports `tiles` at module level -- so nothing ever connected and all
three were advertising under another name. They read +30.70, +30.56 and
+30.60 mA, which now says only that the same state measures the same three
times. **The harness could not see it**: it checked every command's reply and
the charger at both ends, and never that the helper process it started was
still alive. It does now, and a block whose helper dies inside five seconds is
marked suspect.

So T-250's BLE line is still open on the half that matters: **connected, and at
a cadence, is unmeasured.** `--groups link` is the repeat.

**Why a flat cost is what this build should produce.** Read off this build's own
generated `sdkconfig.t5s3pro` (2026-09-15, `env:t5s3pro`) and the pinned
ESP-IDF 5.5.2.260206:

```
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y      modem sleep is on
CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y     its low-power clock is the main crystal
CONFIG_RTC_CLK_SRC_INT_RC=y              there is no external 32 kHz crystal
# CONFIG_PM_ENABLE is not set            no DFS and no light sleep
```

IDF's own Kconfig makes `BT_CTRL_LPCLK_SEL_EXT_32K_XTAL` **depend on**
`RTC_CLK_SRC_EXT_CRYS || RTC_CLK_SRC_EXT_OSC`, so with the internal RC
oscillator that option cannot even be selected. (The S3's
`components/bt/controller/esp32s3/Kconfig.in` is one line sourcing the C3's, so
the C3 text governs here.) Modem sleep therefore runs with the 40 MHz main
crystal as its low-power clock and no power-management framework to gate
anything -- which is exactly the shape of a cost that does not care about
airtime.

**That is read off the configuration and consistent with the measurement, not a
proven cause.** What would settle it is whether this board has a 32.768 kHz
crystal fitted, which is a question for the schematic.

### WiFi

| state | +mA at VBUS | +mW | radio alone, CPU subtracted |
|---|---|---|---|
| station mode, associated to nothing | +13.15 | +69.2 | ~3.7 mA |
| soft AP up, no client | **+84.84** | +441.1 | **~75.4 mA** |
| a full scan, back to back | +70.35 | +366.6 | ~60.9 mA |

**Every WiFi figure carries a 240 MHz CPU.** `HalPowerManager::setPowerSaving()`
forces power saving off whenever `WiFi.getMode()` is not `WIFI_MODE_NULL`
(`HalPowerManager.cpp:59-64`), so "WiFi off" against "WiFi on" is two changes.
Confirmed rather than assumed: all three blocks entered on
`[PWR] Restoring normal CPU frequency` with no further change inside the window,
while every control entered on `Going to low-power mode (80 MHz)`. The last
column subtracts the 240 MHz row above.

**The soft AP is the most expensive thing this board does** -- more than the
frontlight at 100 %, and it is what the web server screen runs on for as long as
it is open.

There is no "associated and idle" row. `CMD:WIFI CONNECT` needs a network in the
credential store and this board's card has none; putting one there is writing a
rider's device state and is the maintainer's call, not a bench tool's.

### GNSS: powering the receiver makes the board cheaper

| state | +mA at VBUS |
|---|---|
| receiver rail down | -0.01 |
| **rail powered, searching** | **-2.05** |
| same, plus every sentence forwarded to the console | **-2.02** |
| rail down again | -0.14 |

Control spread over the run 0.38 mA, so -2.05 is five times the noise floor and
it reproduced twice. **Powering the GNSS rail lowers the board's draw.**

Checked before believing it:

- `Gnss::end()` does cut the rail -- `config_.powerEnable(false)` after closing
  the UART (`lib/Gnss/src/Gnss.cpp:188`), so the two states really differ by the
  receiver's power.
- The receiver really runs: sentences every second in the raw block, and
  `$GPTXT,01,01,01,ANTENNA OK`.
- All four blocks were at 80 MHz.
- **Forwarding every sentence to the console costs 0.03 mA**, i.e. nothing. The
  UART and the log are not where the money is.

A receiver in acquisition should cost tens of milliamps, so a net of -2 means the
rail-down state is paying roughly that much for something. The likeliest
explanation is the ESP32 driving the UART line into an unpowered receiver and
back-feeding it through its input protection -- **a hypothesis, not a measured
cause**. `CMD:SDBUS RAIL` moves the rail without opening the UART, which is the
control that separates "the rail is powered" from "the receiver is running";
`tools/power_campaign.py --groups rail` is that experiment.

Worth noting where it points: T-250 is named for **~20-40 unexplained mA** on
this board, and T-244 is the rail being a latch that survives a reboot -- so two
rides can differ by this without anything in the log saying so.

### The map screen

| state | +mA at VBUS |
|---|---|
| map open, idle | -0.67 |
| back on the home screen | +0.01 |

**An idle map screen costs nothing over an idle home screen.** E-ink holds an
image with no current, and the map activity's loop adds nothing this bench can
see.

**And entering the map did not bring a radio up**, which is why this row is as
low as it is. `MapActivity::onEnter()` gates the radio on
`bleInUse_ = SETTINGS.mapGnssPosition == 0 || forcePhonePosition_` -- "one
position source per map session, and the other radio does not run" -- and this
device has the receiver as its source. Confirmed from the run's `serial.log`:
the `map-idle` window carries neither `BlePositionServer.begin() returned` nor
`[GNSS] aiding sent`. Two caveats on the row, both from that log: the viewport
was empty (`1 missing` tile at that position) and the freshness check ran every
30 s with nothing to do.

**The two "map with a position stream" rows measured nothing** -- the fake
phone died at import, as above. Read them as two more samples of "map idle",
which is what they agree with.

### Panel refreshes

90 frames a block, one every 2,000 ms, whole-panel each time. `flip` alternates
an all-black and an all-white fill, `light` moves a tenth of the bytes, `none`
leaves the buffer alone.

| mode, pattern | frame ms | +mA at VBUS | mJ per frame | mW while busy |
|---|---|---|---|---|
| `FAST`, flip | 597 | +19.28 | **261** | 437 |
| `FAST`, light | 508 | +17.31 | **230** | 453 |
| `HALF`, flip | 1461 | +28.68 | **515** | 353 |
| `HALF`, light | 1162 | +24.07 | **393** | 339 |
| `FULL`, flip | 1461 | +29.06 | **523** | 358 |
| `FULL`, light | 1162 | +24.21 | **399** | 344 |
| `FAST`, nothing changed | 260 | +13.98 | **165** | 635 |

Three things fall out.

**`FULL` and `HALF` are the same frame on this board, in energy as well as in
time.** 1,461 ms both, 523 against 515 mJ -- a 1.5 % gap, narrower than the
run's own control spread is against these numbers.
[`refresh-modes.md`](refresh-modes.md) already said `FULL` does not clean better
than `HALF` and costs a multi-flash for nothing; on the T5 S3 Pro it does not
even cost the flash. The rule stands and now has a price attached.

**`FAST` is half the energy and two-fifths of the time.** 261 against 515 mJ for
the same all-pixels-move content.

**A `FAST` call with nothing to move still costs 165 mJ and 260 ms** -- 63 % of
the energy of a `FAST` frame where every pixel changes. The differential
waveform saves the ink, not the trip: the frame is still converted and pushed
whole. That is the same whole-panel data path
[`t5s3-partial-refresh.md`](t5s3-partial-refresh.md) describes from the driver
side, now measured from the supply side. It is also why the `mW while busy`
column is *highest* for this row: a short window doing CPU and bus work rather
than a long one moving ink.

**One observation left alone.** `HALF` and `FULL` are non-differential and
should not care what is on the panel, yet `light` runs 1,162 ms against `flip`'s
1,461 ms and costs a quarter less. Something in the path is content-dependent in
a mode where it should not be. Not chased here.

## What this bench cannot do

- **Anything under about 5 mA.** The charger's own draw is 1.5 mA typical and
  3 mA maximum, so a sleep state disappears into it. The deep-sleep and ship-mode
  floors need a bare board metered at the cell (parent repo T-553, T-554).
- **Short spikes.** The meter samples ~66 times a second: a 545 ms panel refresh
  lands as about 36 samples, a radio burst lands as nothing. Measure those as
  energy over a repeated cycle instead of as a peak.
- **Anything the gauge could answer better.** With USB unplugged the BQ27220
  measures the cell directly, with no charger in the path -- but it reports a
  hard 0 below its deadband, it is sealed on this board so the deadband is
  unknown, and unplugging USB takes the console with it. The fix is a logged
  `batt_ma` column rather than a console command (T-250, T-282).

## Related

- [`charge-control.md`](charge-control.md) -- `CMD:CHARGE`, the register rules,
  and why `BATFET_DIS` costs 8.6 mA and stays out of every measurement.
- [`power-management.md`](power-management.md) -- the ride-scale picture, the
  frontlight correction, and the power campaign's own scoreboard.
- Parent repo `docs/usb-power-meter.md` -- the meter, and why a VBUS milliamp is
  not a board milliamp.
