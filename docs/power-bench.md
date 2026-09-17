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

**The frontlight scales with the percentage.** 40 % costs 41 % of what 100 %
costs, so the UI's number behaves like a duty and the `PT4103B23F` boost
driver's draw follows it. That is half of what T-250 asks about the light.

**And it lands near the ride figure, from one binary.** Two rides put the light
at 40 % at "~43 mA at the cell" (`power-management.md`, "The frontlight is 43 mA,
not 8"), but they could not be proven to be the same build. This bench is one
binary and one screen: 155 mW at VBUS is about 142 mW at the board at the 91.5 %
efficiency measured the same day, which is **34 mA at the 4.15 V system rail**,
or **37 mA drawn from a 3.85 V cell**. Lower than 43 mA and far from the ~8 mA
this project assumed before 2026-09-04. The remaining gap is what a ride adds:
the map, the radio and a refresh every minute.

**BLE advertising at 11.7 mA is the first measurement of that radio on this
board at all.** T-250 guessed 10-20 mA for BLE *connected* at a 30 ms interval;
advertising is the cheaper half of that range's floor. Connected is not measured
yet and needs a phone in the loop.

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
