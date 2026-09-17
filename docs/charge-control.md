# CMD:CHARGE -- switching the charger off, and what the chip actually does

`CMD:CHARGE` drives the **BQ25896** on the LilyGo T5 E-Paper S3 Pro from the USB
serial console: charging on and off, the I2C watchdog, the BATFET, the charger's
own ADC, and a full register dump.

It exists for one reason. A USB inline meter on VBUS reads **the board plus
whatever the charger is doing**, so no VBUS number is board draw while a cell
charges behind it. Charging off takes the charge current out of the path, with no
device opened and no bare board -- the only measurement route this project's
hardware policy allows. Every per-refresh and per-state power number this project
owes starts here. Tracked as T-251 in the parent repo.

Devel builds only (`-DENABLE_CHARGE_CMD=1`, `env:t5s3pro`). See "Why it is not in
a release build" at the end.

## The commands

```
CMD:CHARGE            ->  CHARGE:chg=1 wd_s=0 batfet_dis=0 vbus_stat=2 chrg_stat=2 pg=1 wd_fault=0 curr_ma=344
CMD:CHARGE OFF        ->  CHARGE_OK:off      then the status line
CMD:CHARGE ON         ->  CHARGE_OK:on       then the status line
CMD:CHARGE WD 0|40|80|160
CMD:CHARGE BATFET <dwell_ms>
CMD:CHARGE ADC        ->  CHARGE_ADC:vbat_mv=4204 sys_mv=4224 vbus_mv=5100 ichg_ma=300 done=1 cont=1
CMD:CHARGE REG        ->  CHARGE_REG:3F 06 51 16 ... (REG00..REG14, hex)
```

Status fields: `chg` is `CHG_CONFIG` (REG03 bit 4), `wd_s` the watchdog period in
seconds, `batfet_dis` REG09 bit 5, `vbus_stat`/`chrg_stat`/`pg` the REG0B status
bits, `wd_fault` REG0C bit 7, and `curr_ma` the BQ27220's `Current()` at the same
instant, signed positive into the cell.

**`wd_fault` clears when it is read.** The first status line after a boot showed
`wd_fault=1` and every later one showed 0. That is the register latching, not the
fault going away.

## Two rules the code enforces

**Every write is a read-modify-write of named bits, and there is no generic
register write.** The parent repo's `docs/t5s3-power-path.md` carries a "never write these" table -- REG14 bit 7 `REG_RST` resets every
register including `BATFET_DIS`, REG03 bit 5 `OTG_CONFIG` drives 5 V back onto
VBUS, REG00 bit 7 `EN_HIZ` drops the board onto the cell. A `CMD:CHARGE REG 0x14
0x80` would put all of them one typo away, so no path here writes a byte a host
chose. Four named bits are reachable; the rest are not reachable at all.

**Every write is read back before it is reported.** A bench number taken against
a state nobody confirmed is exactly the failure this work exists to stop.

## The 40 s trap, and why `OFF` disables the watchdog first

The chip is in default mode until the first write. That write puts it in host
mode and starts the I2C watchdog; on expiry the chip **restores defaults, and
`CHG_CONFIG` is not on the exception list** (SLUSC76C p.31). So charging switches
itself back on 40 seconds into a measurement while the run still says it is off.

`OFF` therefore writes `REG07` bits 5:4 = `00` *first* and only then clears
`CHG_CONFIG`. **Measured 2026-09-12**: charging held off across a 100 s wait with
`wd_fault` still 0.

`ON` puts charging back and **deliberately leaves the watchdog where it is**. On
this board the watchdog was already disabled before anything of ours wrote to the
chip, so re-arming it would not be a restore -- see the next section.

## What this board's charger looked like before we ever wrote to it

**Measured 2026-09-12**, first bench run, `CMD:CHARGE REG` on a board that had
only ever run our firmware's read-only `BatteryMonitor`:

```
REG00..REG14: 3F 06 51 16 10 03 5E 8D 03 44 73 5E 00 15 5A 5D 4A 9A 00 09 06
```

Three of those are **not** the datasheet's reset values:

| register | read | reset | what it means |
|---|---|---|---|
| `REG03` | `0x16` | `0x1A` | `SYS_MIN` = 011 (3.3 V), not 101 (3.5 V) |
| `REG07` | `0x8D` | `0x9D` | `WATCHDOG` = 00, disabled, not 01 (40 s) |
| `REG02` | `0x51` | `0x00` | `CONV_RATE` = 1, the ADC is in 1 s continuous mode |

`REG00` is `0x3F`: `EN_ILIM` clear, which is the vendor's
`disableCurrentLimitPin()`.

**So the charger is configured by LilyGo's factory firmware and keeps it.** The
charger's registers survive an ESP32 reflash and a reboot -- charging stayed
disabled across a full `pio run -t upload` cycle during this bench. This
contradicts what the parent repo's power-path doc assumed ("the chip sits in
default mode today ... our firmware never writes it"), and it has a practical
consequence: **"restore the datasheet default" is not the same as "restore this
board"**, which is why `ON` leaves the watchdog alone. Re-arming a 40 s watchdog
here would make the chip periodically reset `SYS_MIN` from the vendor's 3.3 V to
3.5 V.

It also explains a bug in the first version of `CMD:CHARGE ADC`: it wrote
`CONV_START` and verified the readback, and reported `CHARGE_ERR:adc start` on a
perfectly working chip, because `CONV_START` is read-only while `CONV_RATE` = 1
(Table 8). The command now starts a one-shot only when the chip is not already
converting, and reports `cont=1` when it is.

## What charging off actually costs, measured

**2026-09-12, T5 S3 Pro, build `0.2.1-t5s3pro` + this branch, JT-UM120 inline on
VBUS, board idle on its home screen.** The cell was full for the first pair, so
the interesting pair is the second, taken while the charger was in fast charge.

| state | VBUS | gauge `Current()` | charger `ICHGR` |
|---|---|---|---|
| charging (fast) | **370.2 mA** at 5.156 V | +344 mA | 300 mA |
| charging disabled | **101.9 mA** at 5.221 V | **0 mA** | 0 mA |

So **the gauge's current does fall to ~0 with charging disabled and USB
attached**, which is what SLUSC76C p.18 implies and nothing here had measured.
That was T-251's first open question.

The energy balance cross-checks the instrument: the VBUS delta is 1.377 W and the
charger's own `ICHGR` says 4.2 V x 300 mA = 1.26 W into the cell, so the buck ran
at about **91.5 %** at that load -- next to the datasheet's headline 92.5 % at
2 A. The gauge's 344 mA would make it 105 %, which is impossible, so at this
current the two instruments disagree by more than the `ICHGR` register's own
50 mA step. Neither is calibrated here; the campaign's conversion factor (T-220)
is still owed and this is not it.

`CMD:CHARGE ADC` also validated the meter: it read `vbus_mv=5100` and `5200`
while the JT-UM120 read 5.156 V and 5.221 V in the same two states, which is
inside the ADC's 100 mV step.

## `BATFET_DIS` is honoured while VBUS is present -- and it costs 8.6 mA

The second open question. The datasheet never says whether the bit is accepted
with an adapter attached; it specs `VSYS` for the BATFET-disabled case (p.8),
which only means anything with the converter running from VBUS, but it lists
"plug in adapter" as an *exit* event from ship mode and never says whether an
already-present adapter counts. The two TI E2E threads on exactly this are behind
a bot wall.

**Measured 2026-09-12, five pulses across two charge states**: the bit reads back
as 1, the board keeps running, and clearing it works.

```
CHARGE_BATFET:set=1 alive=1 curr_before=0 curr_during=0 curr_after=0 cleared=1 dwell_ms=5000
```

And the meter shows a step nobody predicted:

| dwell | before | `BATFET_DIS` = 1 | after |
|---|---|---|---|
| 3 x 5 s, cell full | 100.68 mA | **109.27 mA** | 101.6 mA |
| 1 x 9 s, cell at 4.12 V | 101.50 mA | **109.92 mA** | 101.75 mA |

**+8.6 mA, about 8.5 % of the board's whole idle draw, reproducible.** Where it
comes from is `[open]`. It is **not** the cell being taken out of a supplement
path: the gauge reads 0 both with the BATFET on and with it forced off, and ~10 mA
of cell discharge would be above its 5 mA deadband. The likelier explanation is
the converter's own light-load behaviour with no battery buffering it -- p.18
says the device switches to PFM at light load when charging is disabled.

**Consequence for the bench: do not use `BATFET_DIS` for measurements.**
`CHG_CONFIG = 0` is the recipe; forcing the BATFET off adds 8.6 mA of instrument
error to the very number the campaign wants. That was already the rule for safety
reasons, and now it has a measured one.

### The command is a pulse, never a latch

`CMD:CHARGE BATFET <dwell_ms>` sets the bit, reads it back, holds it for the
dwell, then clears it -- in one command, capped at 10 s. Two reasons it can never
be left set:

- **`BATFET_DIS` survives a watchdog expiry.** It is on p.31's exception list,
  unlike `CHG_CONFIG`. Exactly the wrong way round for us.
- **With no VBUS the bit is ship mode.** SYS goes to zero, I2C dies, and only the
  `S4` button or an adapter brings the board back (p.26). So the command refuses
  outright when `VBUS_STAT` is 000, and if the clear ever fails it logs a
  `LOG_ERR` telling whoever is at the desk not to unplug USB.

## Why it is not in a release build

Every other bench command on this board is devel-only because it has no UI behind
it. This one is devel-only because **it actuates the charger**: it can leave a
rider's device not charging, silently, and the device would look perfectly normal
until the battery ran out.

Both command channels -- USB serial (P3) and the BLE command characteristic (P5)
-- share one grammar and have no authentication at all, and BLE advertising runs
with no pairing and no bonding. In a release build this command would hand anyone
in radio range a way to flatten a device they can see but not touch. See T-222 in
the parent repo's `docs/TODO.md`.

## Related

- Parent repo `docs/t5s3-power-path.md` -- the datasheet research behind every
  register here, with page numbers, and the "never write these" table.
- Parent repo `docs/usb-power-meter.md` -- the meter, its four corrections, and
  why a VBUS milliamp is not a board milliamp.
- [`battery-current-probe.md`](battery-current-probe.md) -- `CMD:BATT`, the gauge
  side, including `CMD:BATT DM` and the sealed-gauge finding.
- [`power-bench.md`](power-bench.md) -- what this command is for: the A-B-A
  method, `CMD:BLE`, and what the frontlight and the radio actually cost.
