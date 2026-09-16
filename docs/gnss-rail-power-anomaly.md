# Powering the GNSS rail makes the board draw less. Nobody knows why yet

**The claim under investigation.** On the LilyGo T5 S3 Pro, switching the
`LORA_GPS_EN` rail **on** lowers the current drawn at VBUS by about 2.7 mA.
Switching it off raises it again. A receiver in acquisition should cost tens of
milliamps, so this is the wrong sign and the wrong magnitude at once.

**Status: measured, reproduced, unexplained, and not yet trusted.** The
maintainer's own reaction on 2026-09-16 was that it does not look right, which
is the correct reaction. This file exists so the next session can attack it
without re-deriving anything.

Related: [`power-bench.md`](power-bench.md) is the method and the rest of the
campaign. [`gnss.md`](gnss.md) is the receiver. `docs/t5s3-power-path.md` in the
parent repo is the charger and the meter's own limits.

## What was measured

**Instrument.** Joy-IT JT-UM120 (FNIRSI FNB58) inline on VBUS, ~66 samples a
second, read by `tools/usbmeter_read.py`. Charging disabled with
`CMD:CHARGE OFF` and `chg=0` read back at both ends of every block. Board
`a2853ee1`, `env:t5s3pro`, home screen, indoors, no sky.

**Run 1, 2026-09-15, five minutes a state, eight controls over four hours
agreeing to 0.38 mA** (`docs/power-runs/2026-09-15-t5s3pro-spectrum/`):

| state | absolute | against nearest controls |
|---|---|---|
| `gnss-off` | 91.29 mA | -0.01 |
| `gnss-searching` (`CMD:GNSS ON`) | 89.25 | **-2.05** |
| `gnss-raw` (same, sentences to console) | 89.28 | **-2.02** |
| `gnss-off-post` | 91.16 | -0.14 |

**Run 2, the same evening, three minutes a state, four controls agreeing to
0.15 mA** (`docs/power-runs/2026-09-15-t5s3pro-link-rail/`). This one moves the
rail with `CMD:SDBUS RAIL`, which never opens the UART, and reads the bit back
in every block:

| state | rail bit | UART | absolute | against controls |
|---|---|---|---|---|
| `rail-control-pre` | 0 | closed | 91.295 mA | -- |
| `rail-up-uart-closed` | 1 | closed | 88.536 | **-2.72** |
| `rail-up-receiver-on` (`CMD:GNSS ON`) | 1 | open | 89.35 | **-1.91** |
| `rail-control-post` | 0 | closed | 91.225 | -- |

Per-block standard errors on a 30 s-block basis are 0.012 to 0.057 mA, so the
step is roughly **-2.72 +/- 0.06 mA**. The two controls bracket it and agree to
0.07 mA, so it is not drift.

**The saving belongs to the rail, not to the receiver.** Running the receiver
and parsing its sentences on top of a powered rail gives back 0.81 mA -- the
UART peripheral and the parse. Forwarding every sentence to the console on top
of that costs a further 0.03 mA, which is nothing.

**So `CMD:GNSS OFF` does not make this board cheaper.** Against `CMD:GNSS ON` it
is 1.9 mA dearer. The 2.7 mA figure belongs to a state only `CMD:SDBUS` reaches.

## What the rail actually is

**One expander pin powers two radios.** `PCA9535_IO00_LORA_GPS_EN` is bit 0 of
port 0 on the PCA9535 (`freeink-sdk/.../BoardT5S3Pins.h:70`), and the name is
not decoration: it is the GNSS receiver **and** the SX1262 together. Both
`Gnss`'s `powerEnable` hook (`src/main.cpp:463`) and `CMD:SDBUS RAIL` write that
one pin.

With the rail down, the ESP32 keeps driving into two unpowered parts, not one:

| line | pin | driven during a rail-down block |
|---|---|---|
| GNSS UART TX | see `gnss.md` | yes, the UART is closed but the pad keeps its level |
| `LORA_CS` | GPIO 46 (`BoardT5S3Pins.h:36`) | yes, held at 1 |
| `LORA_RST` | GPIO 1 (`BoardT5S3Pins.h:38`) | yes, held at 0 |

`CMD:SDBUS` reports all three (`SDBUS:cs=1 rst=0 rail=0`) and every rail block in
run 2 carries that line.

## The four explanations, and none is excluded

**1. Back-feed through input protection.** With no supply, a driven pin
forward-biases the part's ESD clamp and current flows into its rail. Powering the
part properly stops it. This is the explanation the first write-up reached for,
and it now has **two** candidate victims rather than one, because of `LORA_CS`
and `LORA_RST` above.

*Against it:* it has to account for roughly 30 mA of saving, since the receiver's
own draw should be ~25-35 mA and the net is -2.7. That is a large leak.

**2. The converter, not the board.** The BQ25896 is a switching buck and the
project's own instrument doc marks its **efficiency at our loads `[open]`**
(`docs/usb-power-meter.md`, "Efficiency at our load is `[open]`"); the datasheet
says it runs PFM at light load with charging disabled. A 3 % efficiency change
across an 88-to-91 mA step is entirely inside that unmeasured band, and a VBUS
meter cannot tell it from a board-side saving. **This explanation was missing
from the first write-up and may be the cheapest one to be true.**

**3. The receiver draws far less than any datasheet says.** Possible, and it
would make the arithmetic work without a leak, but nothing here measures it and
the part is emitting a full sentence set every second.

**4. The rail was never actually off in the states called off.** T-244 records
the rail as a latch that survives a reboot, and `Gnss::end()` returns early when
it is not running, so `CMD:GNSS OFF` cannot lower a rail that `CMD:SDBUS` raised.
Run 2 reads the bit back in every block and rules this out **for run 2**. Run 1
never read it, so run 1's -2.05 rests on command replies alone.

## What would settle it, cheapest first

**a. Repeat the rail experiment at three base loads. 25 minutes, no new code.**

```
python3 tools/power_campaign.py --device t5s3pro --port <port> \
    --out docs/power-runs/<date>-t5s3pro-rail-loads --block 180 --probe \
    --groups rail
```

Run it three times with the frontlight held at 0, 50 and 100 % (`CMD:LIGHT`),
which adds 0, ~23 and ~72 mA and pushes the converter from PFM well into
continuous PWM. **If -2.7 mA survives at all three base loads it is board-side.
If it shrinks or flips, it is explanation 2 and the whole thing is an artefact of
the buck.** This is the one experiment to run first.

**b. Take the driven pins out of the picture.** With the rail down, put the GNSS
UART TX, `LORA_CS` (GPIO 46) and `LORA_RST` (GPIO 1) to high-Z and measure again.
A partial version already exists: `CMD:GNSS PROBE` sets the expander pin itself
to `INPUT` (`src/main.cpp:2106`). Doing the UART alone proves nothing now that the
rail is known to feed the SX1262 as well -- all three, or none.

**c. Reverse the order.** Every run so far is rail-off first. Run rail-on first
and confirm the sign does not follow the ordering. One block.

**d. Read the cell, not just the register.** `--probe` was off in run 1 and on in
run 2, and the gauge is **SEALED** on this board, so its `curr_ma=0` means
"anything between -5 and +5 mA" (`docs/usb-power-meter.md`, "The gauge's floor is
a lie"). A 2.7 mA result sits inside that band, so the cell could be sourcing or
sinking the difference and nothing here would see it. Any future run takes
`--probe` and rejects a block whose boundary `curr_ma` is non-zero.

**e. Settle what the receiver costs at all**, which none of the above does. That
needs a control that removes the receiver's supply while leaving the SX1262
alone, and one expander pin cannot do it.

## Why it matters beyond curiosity

T-250 in the parent repo is named for **~20-40 unexplained milliamps** on this
board. If explanation 1 is right, a rail-down device is paying a leak of that
order, and T-244's latch means two rides can differ by it with nothing in
`power.csv` saying which state the rail was in. If explanation 2 is right, then
every small VBUS difference this campaign published needs re-reading, because it
would mean deltas at this operating point are not additive -- and the campaign
already leans on subtracting one delta from another for the WiFi rows.

**Either way it is load-bearing, and either way it is `[open]`.**
