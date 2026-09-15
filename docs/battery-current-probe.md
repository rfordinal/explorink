# CMD:BATT -- the gauge's numbers on demand

`CMD:BATT` prints the BQ27220's voltage, state of charge and **average current**,
plus the BQ25896's charge status, in one line, when the host asks.

```
CMD:BATT  ->  BATT:mv=4102 pct=100 curr_ma=-38 chg=1 gauge=0x55 charger=0x6b
```

A field that could not be read prints `?`. `chg` is `CHRG_STAT`, the charger's
two bits: 0 not charging, 1 pre-charge, 2 fast charge, 3 done.

`CMD:BATT DM <addr>` reads the gauge's data memory instead -- see "The data
memory, and why it reads nothing useful on this board" below. `CMD:BATT SCAN`
sweeps the whole gauge bus -- see "The I2C sweep, and what it found on X3"
below.

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

- **`-DENABLE_BATT_CMD=1` in `env:t5s3pro` and `env:default`** (2026-09-15,
  `platformio.ini`), and in no release env. `env:default` is the C3 binary that
  covers both X3 and X4 -- the widening this doc predicted.
- **X4 has no gauge.** `gaugeAddr == 0` there, and the reply is
  `BATT_ERR:no gauge on this board`.
- **Enabling `env:default` needed `Wire.h` moved off an `ENABLE_CHARGE_CMD`-only
  guard** (`main.cpp:38`, was `#ifdef ENABLE_CHARGE_CMD`, now
  `#if defined(ENABLE_BATT_CMD) || defined(ENABLE_CHARGE_CMD)`). `env:t5s3pro`
  always carried both flags together, so this latent gap never showed until a
  BATT-only build tried to compile.
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

## The data memory, and why it reads nothing useful on this board

`CMD:BATT DM <addr>` reads one 32-byte data-memory block through
ManufacturerAccessControl:

```
CMD:BATT DM 0x91DE  ->  BATT_DM:addr=0x91DE len=24 sum=bad echo=bad sec=3 opstat=0x00A6 u8=0 u16=0x0001 data=00 01 05 ...
```

**Why it was needed.** Two gauge defaults decide whether a small current means
anything at all. `Deadband` (`0x91DE`, U1, TI default 5 mA) makes `Current()`
report a **hard zero** below it -- a board drawing 4 mA reads exactly 0, which
looks like a measurement and is not. `Operation Config A` (`0x9206`, H2, TI
default `0x0484`) has the SLEEP bit, and in SLEEP the gauge measures every 20 s
instead of every second. Both are quoted from SLUUBD4A's data-memory table.
Whether LilyGo changed either was unreadable, so every small number the gauge
reported was unfalsifiable.

**Measured 2026-09-12: this gauge is SEALED, so it is still unreadable.**
`OperationStatus()` (0x3A) reads `0x00A6`, whose `SEC[1:0]` bits (2:1) are `11` =
sealed. Three different addresses -- `0x91DE`, `0x9206`, `0x929F` -- all return
the same 20 bytes, with the address echo wrong and the checksum wrong. TI's own
default is UNSEALED, so something sealed this part: the factory firmware, or TI's
line.

Unsealing is a write to the gauge (`Control()` with `0x8000` twice), so it is a
change to a device's persistent state and **is not done without asking**. Until
then the honest reading of a `curr_ma=0` on this board is "0 or anything below
the deadband, and the deadband is unknown but probably TI's 5 mA".

**What the reply checks, and why every field of it is there.** A sealed or
unresponsive gauge answers a data-memory read with *something* -- the bytes above
look exactly like a real block. So the reply carries three independent ways to
catch that: `echo` is whether the address read back matches the one requested,
`sum` is `MACDataSum()` against the bytes themselves (255 minus the 8-bit sum of
the address plus `MACDataLen()` - 4 data bytes), and `sec` names the security
state outright. A reply with `sum=ok echo=ok sec=2` is believable; anything else
is not a measurement.

**One implementation trap, paid for on the bench.** The first version read
0x3E, 0x3F, 0x40..0x5F, 0x60 and 0x61 as separate addressed reads and got the
same stale block for every address. The block transfer has to be read as **one
incremental read starting at 0x3E** (SLUUBD4A 2.2's own worked example does
exactly that); an addressed read of each byte re-arms it and never delivers it.
0x3E..0x61 is contiguous: two address bytes, 32 data bytes, `MACDataSum()`,
`MACDataLen()`.

## The I2C sweep, and what it found on X3

`CMD:BATT SCAN` tries a zero-length write against every 7-bit address 0x01-0x7E
and reports the ones that ACK. It exists because X3's `chargerAddr` is 0
(`BoardConfig.h:913`) -- unlike the T5 S3 Pro's BQ25896 -- so whether X3 carries
any charger IC at all was open, and there is no vendor schematic to answer it
from (Xteink does not publish one, unlike LilyGo's dev board).

```
CMD:BATT SCAN  ->  BATT_SCAN:55 68 6B 7E
```

**Measured on the X3 that arrived 2026-09-09, 2026-09-15, build
`x3-i2c-scan` `a1f055ff`:**

- `0x55`, `0x68`, `0x6B` are all accounted for -- the BQ27220 gauge, the DS3231
  RTC and the QMI8658 IMU, all three already in `BoardConfig.h:915`.
- **`0x7E` is not in `BoardConfig.h` anywhere on this bus.** A real fourth
  device, or a bus artifact -- 0x7E sits inside the I2C spec's reserved block
  (0x78-0x7F, UM10204 s3.1.11), which real parts sometimes use anyway. An
  ACK-only sweep cannot tell a real chip from a reserved-address quirk; only a
  register read that comes back sane can.

  **`CMD:BATT PROBE 0x7E` (`BATT_PROBE:addr=0x7E ok=0/16 regs=?? ?? ...`),
  measured on the same X3, 2026-09-15, build `x3-i2c-scan` `8716699e`.** Every
  one of 16 register-addressed reads NACKed. `BATT_SCAN` ACKs a bare
  zero-length write; `BATT_PROBE` ACKs the address the same way but then fails
  the repeated-start read that follows -- so whatever answers at 0x7E does not
  behave like an addressable register device. **Read as: not a real
  register-mapped chip, most likely the reserved-address artifact the spec
  predicts** -- but this is inference from one probe pattern, not a datasheet,
  so it stays short of certain. X3's charger IC (if it has an I2C-visible one
  at all) is still unidentified.
