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
| 1 | UART survives a blocking render | device | **done 2026-09-01**, verified on hardware |
| 2a | Is the rail on by design or by an uncleared latch | device only, one I2C write | **done 2026-09-01**, verified on hardware: an uncleared latch |
| 2b | What the pair draws | device, a cell that is confirmed present (T-583), one small firmware change | open, **gated on T-583** |
| 3 | GNSS as a third `applyFix()` caller | device | **done, verified on hardware**: the map drew from the receiver on the ride 2026-09-01, and the BLE path with the setting off was re-checked 2026-09-02 |
| 4 | Heading from course, on-device | device | **built and ridden once, 2026-09-01** -- the gate held on 31 stationary rows; nobody watched the arrow on the panel |
| 5 | Priority when both sources are live, **and the duty cycle** | numbers from 2, product decision | **reframed 2026-09-02**: a ride took 10+ min to first fix, so the question is what it costs to never power the receiver down |

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
git fetch origin
git log --oneline origin/release/lilygo-t5-s3-pro ^HEAD   # MUST be empty
git log --oneline -3
git -C . submodule update --init --recursive   # only if freeink-sdk is missing
pio run -e t5s3pro                       # expect SUCCESS, zero warnings
```

**That fourth line is the one that bites.** This branch forks from
`release/lilygo-t5-s3-pro`, not from `develop`, so `CLAUDE.md`'s
"only flash a rebased branch" check has to name the release branch -- comparing
against `develop` here answers the wrong question. Anything listed means the
release branch moved (a develop sync, another device fix): **merge it in and
rebuild before flashing**, or the device gets a build that is missing work
somebody already verified. It happened on 2026-09-02: the release branch was five
commits ahead and nothing in this block would have said so.

## The merge gates -- two of them, and they are different deliveries

The original single gate bundled two claims that finish at different times:
**the GNSS implementation is known good** (steps 1 and 2a, the driver and the
rail) and **the map reads from it** (step 3 and T-576). One list made the first
unshippable until the second landed, which is how a gate becomes unmeetable and
the session after it routes around it instead. Split, 2026-09-01, on the
maintainer's call.

### Gate A -- the GNSS implementation itself

`feat/t5s3-gnss` merges into `release/lilygo-t5-s3-pro` on this one. **Met
2026-09-01.**

- **Step 1 done and verified on hardware.** A blocking map entry loses no
  sentences. Done 2026-09-01.
- **Step 2a done and verified on hardware.** The rail's mechanism is settled with
  a citation: an expander latch, not board design. Done 2026-09-01.
- **The stuck-byte drain carried open, with its label attached.** See below. It
  does not block this gate, and it is not closed by it.

### Gate B -- the map reading from the receiver

A later merge, from step 3's own session. **Met 2026-09-02**: both hardware
checks run, both remaining items deferred by the maintainer in writing, each with
its own reason.

- **Step 3 done and verified on hardware.** The map draws from the receiver with
  no phone connected. **Met 2026-09-01**, on the ride.
- **The BLE regression run.** The other half of step 3's own "done when": the
  same build with `mapGnssPosition 0`, a phone connected, the map drawing from
  the phone. **Done 2026-09-02**, on the build the ride ran, no reflash. The frame
  re-anchored on the coordinates the phone sent
  (`renderViewport start: lat=489250000 lon=174500000`), and the receiver stayed
  out of it throughout (`GNSS_OFF`, not one `gnss fix:` line -- a software gate,
  not a rail read). `gnss.md`, "The BLE path
  still works with the setting off". It matters most for the boards with no
  receiver: X4 and X4 Pro have only this path.
- **The stuck-byte drain** -- **deferred by the maintainer, 2026-09-02.** Reason:
  it cannot break anything. It discards **one byte per pass**, and only a
  non-`'C'` head byte that has sat unconsumed for five seconds
  (`src/main.cpp:817-841`) -- which the map's own console never produces, because
  it consumes within milliseconds. The worst case is one stale byte nothing was
  reading; a wedge it fails to clear leaves the device where it would have been
  without it. It ships labelled, and the label is the awkward kind of
  unverified -- it ran and the diagnostic never fired. **The 2026-09-02 bench run
  reproduced the wedge and the diagnostic stayed dark there too**, which is one
  more observation of the same shape, not a verification. See "The drain ships
  open".
- **T-576's SPI contention** -- **deferred by the maintainer, 2026-09-02.**
  Reason: the instrumentation to run it does not exist. Corrupt reads and absent
  tiles land on the same counter and draw the same hatch, so no ride and no
  screenshot can answer it. Both ways to build the instrument are in `gnss.md`,
  "The SPI check needs a counter the firmware does not carry out of the frame".
- **T-576's cold TTFF** -- still open, and unchanged by the ride. The 526 s is
  measured from boot, not from the receiver powering up, so it is an upper bound
  and not the figure this item asks for.

Steps 4 and 5 are in **neither** gate. They are product decisions and they can
land after either merge.

### The drain ships open, and the label is the point

**The stuck-byte drain is unverified, and the reason matters more than the
status.** It is not that nobody ran it. It is that **it ran and the diagnostic
never fired**: in the session where commands finally arrived there were zero
`serial head byte` lines, so the commands got through because the board was awake
with a clean buffer, not because anything was drained
([`gnss.md`](gnss.md), "whether that drain clears the wedge").

That distinction is what stops the next reader closing it on sight. "Never
exercised" invites one run to settle it. "Exercised, and the instrument stayed
dark" means a run that looks clean proves nothing, and the wedge has to be
**reproduced first** -- then a `CMD:` has to succeed within seconds of a drain
line that actually appears.

**So it enters `release/lilygo-t5-s3-pro` labelled, not quietly.** Anything that
reports on this branch says the drain is unverified and says which of the two
kinds of unverified it is.

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
- The drain is verified in the same pass -- **not met**, and it is carried
  open into the release branch rather than closed (see "The drain ships
  open").

**Do not**: change the parser, touch `MapActivity`, or raise the library's
default buffer for every board.

### Done, 2026-09-01. What was built and what it cost

`rxBufferBytes` = 8192 in `env:t5s3pro`, the library default left at 1024, plus
two driver-reported loss counters. Full account in [`gnss.md`](gnss.md), "The
fix: an 8 KB ring, and why not a task". The short version, for a session that
does not want to read it:

- **The first three measurement runs were void.** The device's SD card had no
  tile coverage where it stood, so every render drew nothing and every blocking
  window was about four times too short. Anyone re-measuring anything about map
  cost must confirm coverage first -- `CMD:GOTO_MAP` prints `N tiles ok, M
  missing`, and the map console's `tiles` lists them.
- **Neither the task nor the event-driven read was built**, and the reason is
  worth keeping: a position source needs the newest sentence, not the stream,
  and an undersized ring costs position *age* rather than a wrong position,
  because the driver refuses new bytes instead of evicting old ones.
- **8192 deliberately does not cover the reported 15 s redraw.** That case costs
  about a second of fix age, against a phone that sends a position at most once
  every 7 s.
- The three candidate fixes this plan listed are therefore resolved: candidate 1
  chosen, candidates 2 and 3 rejected with a reason rather than left open.

Not done here: the stuck-byte drain is still unverified, and it is unverified in
the awkward way -- it ran and the diagnostic never fired, so a clean-looking rerun
settles nothing. It sits in gate B, carried into the release branch with that
label rather than closed.

## Step 2 -- the power floor (T-579)

Independent of the rest. Two halves, and **neither needs an external meter** --
the first is answered by the receiver's own byte count, the second by the gauge
the board already carries.

**2a. Is the rail on by board design, or by a latch nothing has cleared?**
**Answered 2026-09-01: an uncleared latch.** `CMD:GNSS RELEASE` on the T5 S3 Pro,
twice. Setting `CONFIG0` bit 0 back to input stops the NMEA; restoring
output-high brings the receiver back. So nothing on the board holds
`LORA_GPS_EN` -- the expander's own latched output does, and it has survived every
reset because the expander has never lost power.

```
cfg0_base=0x00 cfg0_released=0x01 cfg0_restored=0x00 wrote=1 released=1 restored=1
run 1  base 1722 B / 42 sent (3 s)   released 308 B / 6 (5 s)   restored 1737 B / 43 (4 s)
run 2  base 1606 B / 38 sent (3 s)   released 467 B / 10 (5 s)  restored 1743 B / 43 (4 s)
```

The full reading, the coast-down that makes the released window non-zero, and
what it narrows in the earlier "powered without this firmware asking" claim are
in [`gnss.md`](gnss.md), "Settled 2026-09-01".

**No power cycle was run, and none is needed for this question.** The route
through unplugging the battery at connector `P2` was dropped before it was
tried: that part number is `[open]`, nobody here has identified it on the board,
and it was only ever a detour to the question above. `reset=POWERON` stays worth
having in the reply for when a genuine power-on happens by other means -- though
on this board the field has returned `UNKNOWN` on every run so far, including
straight after an esptool hard reset, so its absence proves nothing.

**The subcommand is separate because it writes device state**, and named so
nobody reaches for it while looking for a read. Devel-only with the rest of
`CMD:GNSS`, behind `ENABLE_GNSS_CMD`.

**2b. What does the pair draw?** Rail up and rail down, and **name the
instrument**.

**No external meter is needed, and the first version of this plan wrongly implied
one was.** The BQ27220 already reports average battery current: register `0x0C`,
signed mA, and this firmware **already reads it** --
`freeink-sdk/libs/hardware/BatteryMonitor/src/BatteryMonitor.cpp:211`. It throws
the number away, using it only to decide the sign of `charging`. The public
`Status` struct carries percentage, millivolts and `charging` and no current
(`BatteryMonitor.h`, the `Status` fields). So step 2b is: add `currentMa` plus a
`currentKnown` flag where `0x0C` is already read, then measure. That is a small
device-free change followed by a run.

**The battery is probably connected, and this is still not settled.**
`mapcmd.py stats` returned `batt_mv=4102` and `batt_pct=100` on 2026-09-01. That
rules out the two things it was quoted to rule out -- it is not the 3.3 V rail and
not 5 V VBUS -- but **it does not rule out no cell at all.** This board carries a
BQ25896 charger (`BatteryMonitor.cpp`, `readGaugeCharging`), and a charger with no
cell on it holds that node near its 4.2 V float voltage. 4102 mV fits a charged
cell and an empty connector equally well, and the gauge reads a node, not a
presence. `batt_pct=100` is not a second opinion: it is the same gauge computing
from the same voltage.

**One free test settles it: unplug USB and see whether the board stays up.** USB
is a connector and pulling it is ordinary use; the disputed act was ever only the
cell at `P2`. Until that runs, this claim is **read, not measured**, and 2b --
which must be read on the cell with USB out -- is gated on it rather than
unblocked. Tracked as T-583.

The history is why this is spelled out. This plan first said no cell was attached,
which was wrong and came from a slip in conversation rather than from the board.
The version after it said the opposite and was still only conversation. The
version after *that*, on 2026-09-01, called a voltage a measurement of presence.
Three documents rested on the claim before anything on the device addressed it,
and the device still has not.

One thing to get right when measuring: the gauge reports *battery* current, so
read it with **USB unplugged**, on the cell. On USB the charge path dominates and
the number is about charging, not about what the pair draws.

**There is no cross-check at the cell, corrected 2026-09-02.** An earlier version
of this step said a meter in series at the board's battery connector was allowed
here. It is not: our T5 S3 Pro **arrived in a closed shell**, which makes it a
device under `docs/hardware-policy.md` in the parent, and `P2` is inside it. The
claim came off the vendor schematic before the hardware landed.

So 2b has the gauge and nothing else on the cell side. The one external
instrument left is a **USB meter on VBUS**, which sees the board plus the charger
and therefore prices state-against-state differences rather than an absolute
draw (parent T-220). The gauge stays the primary path for this step, exactly as
written above.

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

### Built 2026-09-01, ridden 2026-09-01

The code is on `feat/t5s3-gnss` and it compiles clean in both envs (`t5s3pro`
and `default`, zero warnings) with 417/417 host tests passing. None of that was
evidence that it works -- the claim is about a dot on a panel, and **a ride the
same day settled it: the marker followed the rider, with no phone connected.**
The ride's own numbers (526 s to a first fix, three to five satellites on a car
dashboard, 21 s of stale dot across a render) are in
[`gnss.md`](gnss.md), "Ten minutes to a first fix outdoors" and the sections
under it.

The full account is in [`gnss.md`](gnss.md), "The map reads it": the files
touched, the three decisions inside `pollGnssFix()` and why each is the way it
is, the rail's lifecycle, and the both-sources-live question left to step 5.

The short version:

- `src/GnssAccess.h` is the whole seam. `applyFix()` gained a third caller and
  no abstraction was added, exactly as this plan predicted.
- `CrossPointSettings::mapGnssPosition`, off by default, reachable with
  `CMD:SETTING mapGnssPosition 1` and not present in the Settings screen.
- Heading is passed as 0 and stays 0 until step 4.
- The receiver's rail comes up in `MapActivity::onEnter()` and goes down in
  `onExit()`, and only if the map is what started it.

**Step 3's "done when" is met on both halves.** The map draws from the receiver
on hardware with no phone (the ride, 2026-09-01), and the same build still draws
from the phone with the setting off (a bench run, 2026-09-02). Of the five checks
`gnss.md` lists under "What a hardware pass has to check", three are done, one is
half done and one cannot be run with today's instrumentation.

**Board state, 2026-09-02: `mapGnssPosition` is 0 on the device.** It was set
during the BLE regression run and never set back. Powering the device off does
not clear it: `CMD:SETTING` calls `SETTINGS.saveToFile()`
(`src/main.cpp:1023`), the field is serialised into `settings.json`
(`src/CrossPointSettings.cpp:102`) and read back at boot with a default of 0
(`:224`). So the card holds a zero and the next boot will too.

**The first command of every future GNSS run is therefore `CMD:SETTING
mapGnssPosition 1`.** Without it the map runs off the phone, everything looks
correct, and the receiver takes no part in what is being judged. That is the
worst shape a test can have: a pass that measured the wrong code.

**T-576's SPI contention was exercised on that ride, and today's instrumentation
cannot say what happened.** This is the first code path that powers the LoRa rail
while the map streams tiles off the SD card, so the ride put the two together for
a whole trip. But the failure is a corrupt tile read, not a crash, and **nothing
the device records separates a corrupt read from a tile the card simply does not
have**: hatch is drawn for both, and `tilesUnavailable_` is incremented by both
(`MapTileSource.cpp:77` for a crc32 failure, `:123`/`:151`/`:183` for absence).
The one counter that does separate them, `corruptLayers_` (`:76`), is frame-
scoped and surfaces only as a serial `LOG_ERR`, so a ride carries none of it home.
**T-576 is untested because the instrument is missing, not because nobody
looked.** Two ways to get one -- carry that counter out of the frame and compare
the same viewport rail up against rail down, or bypass the map with a task doing
continuous CRC-checked card reads under a refresh loop -- are in `gnss.md`, "The
SPI check needs a counter the firmware does not carry out of the frame".

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
