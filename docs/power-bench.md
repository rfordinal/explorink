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
the board drifting under it. Eight controls across the 2026-09-15 sweep agreed to
**0.38 mA** over four hours, which is what makes a 2 mA answer readable.

**Agreeing controls are not a correctness proof.** The 2026-09-12 pair below had
three controls agreeing to 0.42 mA and still put BLE advertising at 11.7 mA,
against 30.9 mA for the same command on the same board three days later. Tight
controls bound the *drift*; they say nothing about a state being what its label
says.

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

**The pattern is part of the measurement, not a detail.** On this board no mode
is differential in the waveform -- `LgfxEpdDriver::display` ignores `prev`
entirely -- but `Panel_EPD` arms a pixel only where the target changed, in every
mode. So `none` asks what
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
| **BLE advertising** `[withdrawn]` | 105.5 mA | 553 mW | **+11.7 mA, +61 mW** |
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

**The 11.7 mA advertising figure is withdrawn.** The same command on the same
board read **30.9 mA** on 2026-09-15, 2.6x higher, with 17,500 samples against
this pair's 25 s. `lib/BlePositionServer/` and `env:t5s3pro` are identical
between the two branches (`git log t251-fixes..develop` on that path is empty),
so the firmware is not the difference, and the advertising phase is not either:
both runs used `CMD:BLE ON` on the home screen, where the fast phase never ends.
**Why the two disagree is `[open]`**, and it is a larger discrepancy than most of
what the sweep reports.

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
campaign's `serial.log` carries one line per rung. The width is 8 bits and the frequency
1 kHz, both from source rather than from this run: `BoardConfig.h:1201` is
`{11, 5000, 8, true}` and `main.cpp` overrides the 5 kHz to 1 kHz for the
`PT4103B23F`'s own ceiling (T-247). (The `level`
field in that line is `_brightnessLevel`, which is set but unused here --
`_useLevel` is false, so the duty comes from `perceptualDuty()`.)

One straight line fits all ten points:

```
+mA at VBUS = 0.90 + 70.85 x duty      worst residual 0.16 mA
+mW at VBUS = 2.60 + 374.0 x duty      worst residual 1.4 mW
```

**The intercept is the finding, not the slope**, and what it *is* stays `[open]`.
0.90 mA is a third of the light's whole cost at 10 % and one per cent of it at
100 %, and it is why the table's last column falls from 108 to 72 -- a fixed term
divided by a growing duty, not the LED string behaving differently. (That column
is mA per unit *duty*, not per UI per cent.) Two mechanisms produce the same
intercept and this run separates neither: a fixed enable cost of the
`PT4103B23F`, or a fixed cost **per PWM cycle**. They come apart in one run,
because a per-cycle cost scales with the PWM frequency and an enable cost does
not, and that frequency is a one-line change. No datasheet figure for the part's
quiescent current is cited here either.

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

**Advertising at the *fast* interval costs 30.9 mA, and this run did not measure
anything else about BLE.** `BlePositionServer` advertises fast (NimBLE's 30-60 ms
default) for `kFastAdvertisingMs`, then drops to 200-300 ms -- but the switch is
made by `serviceAdvertising()`, which only the map and sync screens call.
`CMD:BLE ON` on the home screen is this bench's own instruction, so **every
advertising block here stayed in the fast phase for its whole length** and the
slow phase a parked device actually holds is unmeasured. Three further blocks are labelled "connected" in
`marks.jsonl` and they are not: `tools/blefakephone.py` died at import in every
one of them -- a fresh parent worktree has no `mapbuilder/tilegen` checkout and
the tool imports `tiles` at module level -- so nothing ever connected and all
three were advertising under another name. They read +30.70, +30.56 and
+30.60 mA, which now says only that the same state measures the same three
times. **The harness could not see it**: it checked every command's reply and
the charger at both ends, and never that the helper process it started was
still alive. It does now, and a block whose helper dies inside five seconds is
marked suspect.

**The repeat ran the same afternoon** and is below.

### BLE connected, measured

`--groups link`, 2026-09-15, three minutes a state, same board and build, run
directory `../../docs/power-runs/2026-09-15-t5s3pro-link-rail/`. The device
logged `[BLEPOS] connected: interval 12 units (15 ms), latency 0, timeout 2000`
once per connected block, so the link is attested by the device and not only by
the helper still being alive.

| state | +mA at VBUS | against advertising |
|---|---|---|
| advertising, nothing connected | +30.54 | -- |
| connected at 15 ms, silent | **+26.93** | **-3.61** |
| connected at 15 ms, a position every second | **+27.45** | **-3.09** |

**A connection is cheaper than fast-phase advertising here, by 3.6 mA.** Why is
`[open]`: an advertising event carries three packets plus a scan-request listen
window and a connection event one exchange, but the two intervals differ
(30-60 ms against 15 ms) and neither was varied, so the ordering cannot be
attributed yet. The result matters because the same ordering appeared in run 4 on
2026-08-21 and was written off as counting noise; on this instrument, with
controls at 0.15 mA, it is real.

**A position a second costs 0.5 mA** over an idle link -- small, and above the
control spread, so it is a number rather than a nothing.

That closes T-250's BLE line, which guessed **10-20 mA for connected** and had
never measured it. Connected at this firmware's 15 ms interval is **27 mA**.

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

Control spread over the run 0.38 mA, so -2.05 is five times the noise floor.
`gnss-searching` and `gnss-raw` are two states rather than a repeat; the genuine
repeat is `rail-up-receiver-on` in the second run, 0.14 mA away at -1.91.
**Powering the GNSS rail lowers the board's draw.**

Checked before believing it:

- `Gnss::end()` does cut the rail -- `config_.powerEnable(false)` after closing
  the UART (`Gnss::end()`, `lib/Gnss/src/Gnss.cpp:192`), so the two states differ by the
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

### The rail experiment: the saving is the rail, not the receiver

`--groups rail`, the same afternoon, three minutes a state, four controls
agreeing to **0.15 mA**. `CMD:SDBUS RAIL` moves the rail without opening the
UART, and every block reads the bit back (`SDBUS:cs=1 rst=0 rail=<n>`).

| state | rail bit | UART | +mA at VBUS |
|---|---|---|---|
| control | 0 | closed | -- |
| rail powered, nothing using it | 1 | closed | **-2.72** |
| rail powered, receiver parsed | 1 | open | **-1.91** |

**The whole saving belongs to the rail.** Powering it with nothing on the other
end is already -2.72 mA; running the receiver and parsing its sentences on top
gives back 0.81 mA, which is the UART peripheral and the parse, not a radio.

**So this experiment did not isolate the receiver's own draw, and says why.**
A multi-GNSS receiver in acquisition is tens of milliamps, and neither rail
state shows anything of that size. Two readings survive and both stay `[open]`:

- the receiver draws far less here than any datasheet figure would suggest, or
- the rail-*down* state is paying for an unpowered receiver -- the ESP32 keeps
  driving lines into a part with no supply and back-feeds it through the input
  protection -- and that cost happens to exceed what the receiver costs when it
  is properly powered.

Either way there is a product-level consequence worth stating plainly:
**`CMD:GNSS OFF` does not make the board cheaper.** Against `CMD:GNSS ON`, the
state a user or an activity can actually reach, it is **1.9 mA dearer**. The
2.7 mA figure is the rail alone with the UART shut, which nothing but
`CMD:SDBUS` reaches.

What would settle it, and neither fits in a bench afternoon: `CMD:GNSS PROBE`
on a **real power-on boot** (it answers whether the board holds the rail on by
itself -- see [`gnss.md`](gnss.md)), and a state with the rail down and the
UART pins put to high-Z, which is the control that tells back-feed from
everything else.

### The map screen: not a clean comparison

| state | +mA at VBUS |
|---|---|
| map open, idle | -0.55 |
| back on the home screen | +0.01 |

**Do not read that as "the map is free".** The map started the GNSS receiver on
entry. `MapActivity::onEnter()` picks one position source and brings up only
that one, and the run's `serial.log` says which, 2.3 s before the block opened:

```
[MAP] ble: not started, position comes from the receiver
[GNSS] aiding sent: ...
[MAP] gnss: started, rx ring 8192 bytes
```

A powered GNSS rail is worth **-1.9 to -2.7 mA** on this board (below), so a map
screen carrying one against a home screen without one is not a like-for-like
pair. Adding the credit back puts the map screen at roughly **+1.4 mA** over
home -- three times the run's control spread. **The clean number needs a map
entered with the receiver forced down, and this run does not have it.**

**The evidence first published here was a check that could not fail.** An
earlier version of this section said no radio came up, because the `map-idle`
window carried no `BlePositionServer.begin() returned` and no
`[GNSS] aiding sent`. Those are one-shot entry lines and the window starts 30 s
after entry, so they can never be inside any window, for any state. A test whose
negative result is guaranteed is not evidence.

Two further caveats on the row, from the same log: the viewport was empty
(`1 missing` tile at that position) and the freshness check ran every 30 s with
nothing to do.

**The two "map with a position stream" rows measured nothing** -- the fake phone
died at import, as above. `map-position-1s` agrees with `map-idle` to 0.03 mA.
`map-position-7s` reads **0.97 mA lower**, which is 2.5x the control spread and
is not explained.

### Panel refreshes

90 frames a block, one every 2,000 ms, whole-panel each time. `flip` alternates
an all-black and an all-white fill, `light` moves a tenth of the bytes, `none`
leaves the buffer alone.

> **The energy columns below are withdrawn, 2026-09-15.** `CMD:REFRESH` blocks
> `loop()` for the whole block, so `HalPowerManager`'s idle throttle never runs
> and the board sits at **240 MHz from `REFRESH_BEGIN` to `REFRESH_END`** -- while
> every control it is differenced against sits at 80 MHz. Each row therefore
> carries the whole 80->240 MHz step (+9.48 mA) on top of the panel, and
> `mJ per frame` charges the panel for ~2 s of 240 MHz idle per frame.
>
> Read off the run's own data, not inferred: all seven blocks enter on
> `[PWR] Restoring normal CPU frequency` with the only change inside the window
> being the return to 80 MHz at the end, and the tenth percentile of
> `refresh-half-flip`'s one-second means is **100.5 mA** -- `cpu-240`'s 100.90,
> not the control's 91.3.
>
> The frame times are unaffected and stand. The energies need re-reporting
> against each block's own inter-frame level, which is in the committed
> `meter.log` and needs no device; the clean fix is `CMD:CPU HOLD 240` on the
> refresh blocks *and* on a matched control, the same way the WiFi group's clock
> is handled.

| mode, pattern | frame ms | +mA at VBUS `[withdrawn]` | mJ per frame `[withdrawn]` | mW while busy `[withdrawn]` |
|---|---|---|---|---|
| `FAST`, flip | 597 | +19.28 | 261 | 437 |
| `FAST`, light | 508 | +17.31 | 230 | 453 |
| `HALF`, flip | 1461 | +28.68 | 515 | 353 |
| `HALF`, light | 1162 | +24.07 | 393 | 339 |
| `FULL`, flip | 1461 | +29.06 | 523 | 358 |
| `FULL`, light | 1162 | +24.21 | 399 | 344 |
| `FAST`, nothing changed | 260 | +13.98 | 165 | 635 |

What survives, because it rests on the frame times rather than the energies.

**`FULL` and `HALF` are the same call on this board**, and that is read off the
driver rather than measured: `LgfxEpdDriver.cpp:110-114` maps both
`RefreshMode::Full` and `RefreshMode::Half` to `lgfx::epd_mode::epd_text`. The
bench agrees, and the strongest form of that is not the energy: `total_ms`,
`mean_ms`, `min_ms` and `max_ms` come back **byte-identical** between the two
modes (131511 / 1461 / 1459 / 1464 on `flip`; 104535 / 1161 / 1161 / 1162 on
`light`). That is a consistency check on the instrument, not a finding. **It says nothing about
[`refresh-modes.md`](refresh-modes.md)'s rule**, which is about the X4's SSD1677
where `HALF` is `0xD7` and `FULL` is `0xF7`, two genuinely different controller
sequences. This board never issues a distinct `FULL` waveform, so it can neither
confirm nor refute that.

**`FAST` is two-fifths of the time**, 597 ms against 1,461 for the same
all-pixels-move content. The energy ratio is withdrawn with the table.

**A `FAST` call with nothing to move still costs 260 ms**, 44 % of the time of a
`FAST` frame where every pixel changes. (The energy share is withdrawn with the
table; it was inflated by the clock and the true share is smaller.) The CPU
still converts
and pushes all 518,400 pixels and the scan still clocks every row, which is the
whole-panel data path [`t5s3-partial-refresh.md`](t5s3-partial-refresh.md)
describes from the driver side, now measured from the supply side. What the 96 mJ
between `none` and `flip` buys is `Panel_EPD`'s per-pixel arming -- **not a
differential waveform**: `LgfxEpdDriver::display` takes `prev` and opens with
`(void)prev`. It is also why the `mW while busy` column is *highest* for this
row: a short window doing CPU and bus work rather than a long one moving ink.

**`epd_text` is content-dependent too**, which is why `light` runs 1,162 ms
against `flip`'s 1,461 ms. An earlier version of this section filed that as an
anomaly on the grounds that `HALF` and `FULL` are "non-differential" -- a
property of the X4's SSD1677, wrongly carried to this board. Here
`Panel_EPD::task_update()` arms a pixel only when its target changed, in every
mode ([`t5s3-partial-refresh.md`](t5s3-partial-refresh.md)).

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
