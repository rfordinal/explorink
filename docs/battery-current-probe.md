# CMD:BATT -- the gauge's numbers on demand

`CMD:BATT` prints the BQ27220's voltage, state of charge and **average current**,
plus the BQ25896's charge status, in one line, when the host asks.

```
CMD:BATT  ->  BATT:mv=4102 pct=100 curr_ma=-38 chg=1 gauge=0x55 charger=0x6b
```

A field that could not be read prints `?`. `chg` is `CHRG_STAT`, the charger's
two bits: 0 not charging, 1 pre-charge, 2 fast charge, 3 done.

**`curr_ma` is signed the way TI signs it**: positive is current *into* the cell,
negative is current *out* of it. A charging board therefore reports the opposite
sign to what "draw" suggests, and a reading taken on USB is mostly about the
charger rather than about the board.

## Why it exists

**A USB meter reads the board plus the charger.** The power campaign's only
external instrument now sits on VBUS (the parent repo's `docs/usb-power-meter.md`
and T-220 -- every unit on the bench is enclosed, so nothing meters the cell).
With a cell charging behind a BQ25896, a VBUS number is not board draw.

Subtracting the gauge's own current is one of the three ways round that, and it
only works if the two numbers come from the same moment. That means the host has
to be able to ask, at the instant it marks a window in the meter log. A row once
a minute in `power.csv` cannot do it.

## Why it re-reads the registers instead of extending BatteryMonitor

The SDK already reads all three registers -- `freeink-sdk`'s
`BatteryMonitor.cpp:211` reads `0x0C` -- and **throws the current away**, using it
only to decide the sign of `charging`. Its public `Status` carries percentage,
millivolts and a bool, and no current.

Adding a `currentMa` field there is the obvious fix and it is the wrong repo:
`freeink-sdk` is upstream's (`Free-Ink/freeink-sdk`), and our submodule pointer
stays on upstream `main`, never on a fork. So this handler reads the same
registers from our side, with the register numbers copied from the SDK's own
table so the two cannot drift apart silently.

`gnss-to-map-plan.md`'s step 2b asks for the SDK change; **this replaces that
half of it**. The measurement 2b wants is unchanged.

## Scope and gating

- **`-DENABLE_BATT_CMD=1` in `env:t5s3pro` only**, and in no release env. Devel
  builds of other boards do not carry it.
- **X3 would answer it too.** It carries a BQ27220 on the C3 binary, and the
  profile is picked at runtime, so widening the flag is a one-line change when
  an X3 measurement comes up. Until then a C3 build does not pay for the code.
- **X4 has no gauge.** `gaugeAddr == 0` there, and the reply is
  `BATT_ERR:no gauge on this board`.
- **It leaks nothing about the rider** -- no position, no route, no identity. It
  is devel-only because a command with no UI behind it does not belong in a build
  a stranger flashes, not because the reply is sensitive.

## What is verified

**Nothing on hardware yet.** Written 2026-09-02; it compiles clean in
`env:t5s3pro`. What a device pass has to check:

1. `CMD:BATT` answers at all, with plausible `mv` and `pct` against what
   `mapcmd.py stats` reports from the same gauge through the SDK's own path.
2. `curr_ma` moves the right way: negative (out of the cell) on battery, positive
   while charging, near zero at termination.
3. `chg` tracks plugging and unplugging USB.
4. The read does not disturb the SDK's own gauge reads -- the handler calls
   `Wire.begin()` with the same pins and clock the SDK uses, so it reconfigures
   the bus to what it already is, but that is read off the code and not observed.

**And a trap worth naming before the first run**: the serial console wedges until
a line arrives with a leading newline (`gnss.md`, "The BLE path still works with
the setting off"). `tools/mapcmd.py` does not send one yet, parent T-113. Send
`\nCMD:BATT\n` if the first attempt is silent.
