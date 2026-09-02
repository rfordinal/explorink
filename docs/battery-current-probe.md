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

**Run on the T5 S3 Pro, 2026-09-02**, on USB, cell full and charge terminated:

```
BATT:mv=4100 pct=100 curr_ma=0 chg=3 gauge=0x55 charger=0x6B
```

- **It answers, and repeatably** -- three reads two seconds apart gave the
  identical line, and three more during a map render gave it again.
- **`pct` agrees with the SDK's own path, measured against this build.**
  `CMD:BATT` returned `pct=100` and a `CMD:SCREENSHOT` seconds later shows `100%`
  in the header, which `GUI.drawHeader()` gets from `BatteryMonitor` -- two
  readers of one gauge, same firmware, same minute. The agreement is at percent
  resolution, which is all the header has. **`mv` is not cross-checked**: nothing
  on screen shows millivolts, so 4100 is plausible for a full cell and unverified
  against a second reader.

  An earlier version of this section claimed the agreement off a screenshot taken
  before this build was flashed. Two readings from two firmwares is not a
  cross-check, and it read like one.
- **`chg=3` is the charger reporting charge done**, which is what a full cell on
  USB should say, so the BQ25896 read at `0x0B` works.
- **No disturbance seen.** The map screen kept rendering and drawing its own
  battery figure with this handler re-beginning the bus underneath it. Observed,
  not proven -- a race would not show up in six reads.

**`curr_ma` is the one that is not verified, and 0 is exactly why.** On USB with
charge terminated the cell is neither charging nor discharging, so 0 is the right
answer -- and it is also what a broken read would print. The field says the I2C
read succeeded (a failure prints `?`) and nothing more. **A reading of 0 here is
a check that cannot fail.**

Two things still to run, and both need the cell to be doing something:

1. **Discharge, then read while charging.** Unplug USB for long enough to drop
   the cell off termination, plug back in, and `curr_ma` should go clearly
   positive (into the cell). That closes the sign convention and the magnitude in
   one go, and it costs one cable pull.
2. **Read on battery.** `curr_ma` should be negative and roughly the board's
   draw. This cannot be done over USB serial -- unplugging the cable ends the
   session -- so it needs either a BLE-reachable version of this command or a
   periodic row written to the card, like `GnssLog`. Neither exists.

**And a trap worth naming before the first run**: the serial console wedges until
a line arrives with a leading newline (`gnss.md`, "The BLE path still works with
the setting off"). `tools/mapcmd.py` does not send one yet, parent T-113. Send
`\nCMD:BATT\n` if the first attempt is silent.
