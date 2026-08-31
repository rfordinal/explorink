# Plan: the map reads position from the internal GNSS

Goal: on a board with its own receiver, the map's dot comes from that receiver
instead of from the phone. Thesis `V4` and `V47` -- position from the device is
the target, the phone is a state being removed.

**This file is the tracker.** Each step below is written so a session with no
prior context can pick exactly one of them up, finish it, and stop. Update the
status table when you finish a step, in the same commit as the work.

Read [`gnss.md`](gnss.md) first. It is what exists today and what is still open;
this file is only what comes next.

## Status

| step | what | needs | state |
|---|---|---|---|
| 1 | UART survives a blocking render | device | **open, do this first** |
| 2 | Power floor of the GNSS + LoRa pair | device, meter, maintainer | open (T-579) |
| 3 | GNSS as a third `applyFix()` caller | device | blocked on 1 |
| 4 | Heading from course, on-device | device, product decision | blocked on 3 |
| 5 | Priority when both sources are live | numbers from 2, product decision | blocked on 2 and 4 |

## How sessions share this

**One worktree, `.worktrees/firmware/t5-gnss`, branch `feat/t5s3-gnss`, and
sessions continue in it chronologically.** Maintainer's decision, 2026-08-31.

**One session at a time in it.** The steps are independent in *content*, not in
*checkout*. Two sessions editing this worktree at once is the exact failure
`CLAUDE.md` describes under "Every change goes in a worktree": an edit sitting in
a shared checkout belongs to whoever commits next, and it cost seven hours on
2026-08-19. If a second session wants to work while this one is open, it branches
its own worktree off `feat/t5s3-gnss` and merges back here.

Step 2 is the one exception worth knowing: it is a measurement and a doc, it
touches no source file, so it can safely run in its own worktree in parallel.

Start of every session in here:

```
cd .worktrees/firmware/t5-gnss
git status --short                       # expect clean
git log --oneline -3
git -C . submodule update --init --recursive   # only if freeink-sdk is missing
pio run -e t5s3pro                       # expect SUCCESS, zero warnings
```

## The merge gate

`feat/t5s3-gnss` merges into `release/lilygo-t5-s3-pro` when **the GNSS
implementation itself is known good and mapped with no surprises left** --
maintainer's wording, 2026-08-31. Concretely, all four:

- **Step 1 done and verified on hardware.** A blocking map entry loses no
  sentences.
- **Step 3 done and verified on hardware.** The map draws from the receiver with
  no phone connected.
- **The stuck-byte drain verified.** It is the one thing in the current tree that
  the tree's own docs call unverified ([`gnss.md`](gnss.md), "whether that drain
  clears the wedge"). Reproduce the wedge, watch a `CMD:` succeed within seconds
  of the drain line.
- **T-576's two open items either closed or explicitly deferred by the
  maintainer**, not silently dropped: the SPI-contention test that has never run,
  and a TTFF measured from the receiver's own power-on.

Steps 4 and 5 are **not** in the gate. They are product decisions and they can
land after the merge.

## Step 1 -- the UART has to survive a blocking render

**Why this is first even though step 3 is the exciting one.** Wiring GNSS into
the map is small (step 3). A position source that silently loses seconds of fixes
is worse than the phone, because the map cannot tell. Measured 2026-08-31: a map
entry blocked the loop for **6.07 s** and cost **85 sentences**, of which **84
moved no counter at all**.

Measured inputs, all from the same run, all in [`gnss.md`](gnss.md):

| | |
|---|---|
| receiver output rate | **816 B/s, 14.9 sentences/s** (measured between two statuses with no screen work) |
| worst blocking window seen | **6.07 s** (`New max loop duration: 6074 ms`, the whole map `onEnter`) |
| its composition | 4,017 ms render, ~1.5 s panel refresh, ~0.5 s setup. **BLE begin is 48 ms**, do not blame it |
| driver RX buffer, stock | 256 B (`framework-arduinoespressif32` `HardwareSerial.cpp:148`, read not measured) |
| what this tree asks for | 1024 B (`GnssConfig::rxBufferBytes`), about 1.2 s of headroom |

**Do the measurement before choosing the fix.** The 6.07 s is one observation of
one map entry. What decides the design is the worst *real* window: a cold tile
load, a 16-grey refresh, a route loaded, the freshness check running. Drive them
from `CMD:` and read `New max loop duration`.

Three candidate fixes, and the number picks one:

1. **Bigger buffer.** Cheapest. 6.07 s at 816 B/s needs ~5 kB, so ~8 kB to have
   margin. Internal DRAM on this board is ~300 kB free, so it is affordable
   here and **not** on a C3 -- and `lib/Gnss/` is meant to go upstream, so the
   default stays modest and the board's env raises it.
2. **UART on its own task.** Cleanest, and the only one that survives an
   arbitrarily long block. Costs a lock between the task and the map's read of
   `Gnss::fix()`.
3. **Event-driven read.** The driver's own callback. Least code, most
   framework-specific, hardest to port.

**Done when**, and read the second half of this carefully:

- A blocking map entry leaves `rxfull` at 0 **and** the sentence count across the
  window matches the 14.9/s baseline. `rxfull=0` alone is a check that cannot
  fail -- it also reads 0 when nothing was measured. Both, or it is not done.
- The chosen fix and the number that chose it are written into
  [`gnss.md`](gnss.md), replacing "A blocking render starves the UART".
- The drain is verified in the same pass (see the merge gate).

**Do not**: change the parser, touch `MapActivity`, or raise the library's
default buffer for every board.

## Step 2 -- the power floor (T-579)

Independent of the rest, needs the maintainer's hand and a meter. Two halves,
and the first is nearly free.

**2a. Is the rail on by board design, or by a latch nothing has cleared?** The
receiver is powered before any firmware asks -- that is measured. The mechanism is
not. Four vendor datasheets say the expander's configuration resets to
all-inputs, but the reset needs the rail below ~0.8 V against 1 uA of standby
draw, so a 21 s unplug did not necessarily get there.

The test self-certifies now: `CMD:GNSS PROBE` reports the chip's own reset cause
in the same line as the registers. **No `reset=POWERON`, no conclusion.** Two
traps that cost three attempts on 2026-08-31, both in [`gnss.md`](gnss.md): a
vanished USB device node means the USB peripheral went down, **not** that the
board lost power (twice it was deep sleep), and a silent wait lets the board's own
sleep timer end the run -- poke it with a harmless command every 15 s.

**2b. What does the pair draw?** Rail up and rail down, and **name the
instrument**. The BQ27220 is on board; a meter in series at the development
board's battery connector is allowed (`docs/hardware-policy.md` in the parent --
a bare board is not a device).

**Done when** the mechanism is settled with a citation and both draws are
recorded. Asking LilyGo for the schematic would settle 2a with no measurement at
all and the vendor thread is open.

## Step 3 -- GNSS as a third `applyFix()` caller

**Smaller than it sounds, because the funnel already exists.** Position is
already transport-agnostic from the moment it enters `applyFix()`:

```
src/activities/map/MapActivity.h:205    void applyFix(int32_t latE7, int32_t lonE7,
                                                      uint8_t headingStep, uint8_t seq)
src/activities/map/MapActivity.cpp:2473 BlePositionServer::getInstance().getLatest(update)
                                        -- the ONLY position read in the file
src/activities/map/MapActivity.cpp:2505 applyFix(...)   from BLE
src/activities/map/MapActivity.cpp:2544 applyFix(...)   from the serial console
```

So GNSS is a **third caller**, not an abstraction. No interface, no base class.
`docs/lora.md` in the parent argues for an abstraction over position *sources*;
`applyFix()` already is that seam, and it was found by reading rather than
assumed -- verified 2026-08-31, `getLatest` occurs exactly once in the file.

What the work is:

- **Convert the units.** `Gnss::fix().latitude` and `.longitude` are decimal
  degrees as `double`; `applyFix` wants `int32_t` in 1e-7 degrees.
- **Pass heading 0 and leave it there.** Mapping course is step 4 and doing it
  here will produce a compass that spins on a parked bike. Say so in a comment.
- **Gate it behind a setting**, off by default. `CrossPointSettings.h:273-275`
  holds the map's persisted fix fields and is the neighbourhood. Add the key to
  the `CMD:SETTING` allow-list (`src/main.cpp:955-961`) so a test needs no menu
  walk.
- **Do not touch the BLE path.** X4 and X4 Pro are the reference devices and have
  no receiver (`V5`, `V48`).

Two things that come free and should be noted rather than built on:

- `showingPersistedFix_` and the waiting banner clear on any real fix
  (`MapActivity.cpp:2480-2482`), so a GNSS fix ends the banner with no change.
- `PositionUpdate::accuracyM` is "wired and stored and nothing draws it"
  (`lib/BlePositionServer/include/BlePositionServer.h:45`). GNSS has HDOP, so
  this field could finally carry a real number. Nothing draws it, so it is not
  part of this step.

**Done when** the map draws from the receiver **on hardware** with no phone
connected, and the same build still draws from the phone with the setting off.

**A quantified bonus, not a goal.** `BlePositionServer::begin()` costs
**57,080 bytes** measured, and the map builds it on enter and tears it down on
exit (`MapActivity.cpp:2231`, `:2062`). A board with a receiver could skip it
entirely. On this S3 that is comfort; on a C3 it would be about a third of the
map screen's free heap. Do not do it in this step -- it changes what the map is
for, and the phone still brings tiles.

## Step 4 -- heading, and this is the real work

Not arithmetic. `PositionUpdate::heading` is **0 to 15**, steps of 22.5 degrees
(`drawCompass()`, `MapActivity.cpp:1470`; `drawPositionMarker()`, `:5049`), and
**nothing on the device derives it** -- the phone supplies it.

That is deliberate twice over: thesis `V35` puts the navigation head in the
phone, and the maintainer's standing instruction is not to add device-side
hysteresis for what the phone already stabilises.

GNSS gives `courseDegrees`, and **at rest it is noise**: measured
`speed=1.3 course=211.9` on a stationary desk, 2026-08-31. A naive mapping turns
the compass on a parked bike.

So this step builds a **minimal navigation head on the device**: a speed gate
below which heading is held rather than updated, plus hysteresis so it does not
flutter at the threshold. `V35` already anticipates the cost ("je to práca, ktorú
cieľ z V4 zaplatí").

**This is a product decision before it is code.** What the rider should see when
stopped -- last heading, north-up, or no arrow -- is the maintainer's call. Bring
the measured numbers, propose, do not choose.

## Step 5 -- priority when both sources are live

Phone connected **and** the receiver has a fix: which wins? The phone has the
stabilised heading, the receiver has independence. Needs step 2's numbers (a
duty-cycled receiver has a price) and step 4 (without it the receiver has no
usable heading). Product decision.

`power-management.md`'s T5S3 section already frames the duty-cycle question and
its G1-G7 measurement list; that is where the numbers land, not here.

## What this plan deliberately does not do

- **No abstraction layer.** `applyFix()` is the seam. Adding an interface over
  one function with three callers is cost with no buyer.
- **No routing, no turn-by-turn.** `V10`, `V12`, `V17`. GNSS does not change what
  the product answers.
- **No claim that the T5 is a product.** `V48`: it is a development board. Every
  number here is a T5 S3 Pro number and must not be quoted as an X4 Pro number.
