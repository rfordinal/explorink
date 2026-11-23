# Which refresh mode, and why never FULL

Three modes reach the panel: `FAST_REFRESH`, `HALF_REFRESH`, `FULL_REFRESH`
(`FreeInkDisplay.h:31`). The names suggest a thoroughness ladder. They are not
one. Picking `FULL` because a frame needs a proper clean is the wrong call on
this hardware, and it cost the map screen a visible multi-flash on every entry
until 2026-08-17 (`map-follow.md`, "The entry frame is HALF, not FULL").

## What each one selects on the X4

The driver maps the mode to a controller update sequence, per board
(`Ssd1677Driver.cpp:53-65`, the X4 config):

| mode | X4 sequence | waveform | differential? |
|---|---|---|---|
| `FAST_REFRESH` | `0xFC` | stock partial/DU | **yes** -- drives only pixels differing from the RED plane |
| `HALF_REFRESH` | `0xD7` | stock warmed full-clean, single pass | no -- both planes rewritten |
| `FULL_REFRESH` | `0xF7` | OTP full, **multi-flash** | no -- both planes rewritten |

`HALF` and `FULL` both clear the panel absolutely and both reseed the
differential baseline. The difference is that `0xF7` inverts the panel several
times on the way, and `0xD7` does not. Stock X4 firmware never runs `0xF7` in
normal operation -- its only clean primitive is the single-pass `0xD7`
(`Ssd1677Driver.cpp:360`, and the sleep screen leans on the same fact,
`sleep-screen.md`).

So: **`FULL` does not clean better than `HALF`. It cleans the same and flashes.**

## The rule

- A frame that must clear whatever is physically on the panel -- first paint of
  an activation, coming back from another screen, erasing ghosting -- asks for
  **`HALF_REFRESH`**.
- Everything else asks for `FAST_REFRESH` and lets the driver promote if it must.
- **`FULL_REFRESH` needs a reason in a comment.** One caller has one today:
  `CMD:SHOWIMAGE` (`main.cpp:688-690`), where a host-pushed framebuffer is being
  judged for dither quality and any ghosting at all would be judged with it.

## Handing the clean forward: `requestCleanNextFrame()`

The rule above says the frame *arriving* from another screen asks for `HALF`. But
every menu screen paints `FAST` unconditionally -- `HomeActivity.cpp:141` (default
argument), `TileSyncActivity.cpp:1101`, `RouteSelectActivity.cpp:141` -- and they
got away with it because the screen *leaving* cleaned up first.

`MapActivity::onExit()` used to do that cleaning the expensive way:

```cpp
renderer.clearScreen();
renderer.displayBuffer(HalDisplay::HALF_REFRESH);   // removed 2026-08-19
```

A 1,684 ms whole-panel refresh painting white, which the arriving screen
overwrote with a 500 ms `FAST` -- two refreshes, ~2,184 ms, and a white flash in
between that the rider sees on every exit from the map.

**Derived, not measured**: 1,684 + 500 from the two figures below. The pre-fix
build was never run under the gate -- the two-refresh pattern is what the code
says it did, and the gate's pre-fix expectation was only ever tested against a
synthetic log. `docs/firmware-builds/2026-08-17-319a8c5f-map-entry-half-refresh.bin`
(parent repo) predates the fix and would settle it in one map-to-Home exit.

`GfxRenderer::requestCleanNextFrame()` (`GfxRenderer.h`) replaces it with a
one-shot request. The next whole-panel `displayBuffer()` or
`displayBufferAsync()` runs it through `takeCleanRefreshMode()`
(`GfxRenderer.cpp:1557-1565`): a `FAST` becomes a `HALF`, a `HALF`/`FULL` passes
through, and either way the request is spent. So the map's exit costs one
refresh instead of two, and what appears on the glass is the menu rather than
white.

`displayBufferWindow()` deliberately does **not** spend the request: a windowed
differential update addresses only its rectangle and cannot clear the rest of
the glass, so a pending clean has to survive it.

Same shape as the driver's own `_needsInitialFull`
(`Ssd1677Driver.cpp:364-373`) -- a one-shot "the next frame must clear"
promotion -- lifted to where an activity can ask for it. Read off the source; the
panel-time claim is the 500/1,684 ms pair measured below.

**Measured on the X4 2026-08-19**, build `0.1.0-dev-map-exit-clean-8a5851cc`,
off the device's own refresh log (`tools/quick_resume_gate.py` in the parent
repo):

```
[547736] Exiting activity: Map
[547801] Entering activity: Home
[549537]   Wait complete: refresh (1683 ms)     <- one refresh, promoted
```

One `HALF` where the pre-fix build logged a `HALF` and then a `FAST`. The
maintainer confirmed no map ghost under the menu on the same run. The promotion
is the load-bearing part and it does happen: a promotion that silently did not
would read 500 ms here.

## What the driver promotes behind your back

Three cases, all in `Ssd1677Driver::displayImpl()` (`Ssd1677Driver.cpp:355-394`):

- **First paint after boot or wake** (`_needsInitialFull`): a `FAST` request is
  promoted, because a differential refresh cannot clear the boot or sleep image
  it never saw. A `HALF` or `FULL` the caller already asked for is honoured as
  is, so the boot logo does not pay an extra flash on top of its own frame.
- **Cold panel** (`!_isScreenOn`) on boards with no `fullSeqOverride`: promoted to
  `HALF`. The X4 sets that override, so this branch is not the X4's path.
- **Leaving grayscale** (`_inGrayscaleMode`): a `FAST` is promoted to `HALF`,
  because RED still holds the grey MSB plane and cannot serve as a differential
  baseline. Stock has no revert waveform either.

`FreeInkDisplay::displayBuffer()` promotes a `FAST` to `HALF` once more when an
inversion is pending (`FreeInkDisplay.cpp:505`).

Consequence worth remembering: **asking for `FAST` is not the same as getting
it**, so a timing measured on the first frame after boot is not a `FAST` number.

## What it costs

Measured on the X4 2026-08-17, build `319a8c5f`, off the driver's own
`Wait complete: refresh (N ms)` line (`EpdBus.cpp:220`):

| mode | waveform |
|---|---|
| `FAST_REFRESH`, whole panel or windowed | 500 ms |
| `HALF_REFRESH`, whole panel | 1,684 ms |
| `FULL_REFRESH` | **never timed** |

Two things that line does **not** include: the controller RAM writes, which
happen before the wait (`Ssd1677Driver.cpp:398-407`) and cost a non-differential
mode roughly 68 ms more than a `FAST` (derived, `map-follow.md`); and the
framebuffer work, which on a map frame dwarfs both (~1.6 s of tile reads and
rendering, `map-follow.md`, "The refresh").

A windowed `FAST` costs the same 500 ms as a whole-panel one -- **the waveform is
a fixed cost and area does not enter into it** (measured over two ride replays,
`map-follow.md`). So the thing to minimise is the number of refreshes, never
their size.

### The T5 S3 Pro is not the X4, and the number is twice as big

**T5 S3 Pro, windowed refresh: ~1,081 ms** `[measured, 2026-09-05]`. Every
figure above is the X4's.

Instrument: the device's own `PowerTelemetry` counters over one 4 h 36 min
walk, read out of `power.csv` afterwards -- `panel_busy_ms` 2,944,634 over
`ref_window` 2,608 window refreshes plus 29 whole-panel ones, so the whole-panel
share is under 1 % of the total. Per-refresh cost held between **1,049 and
1,101 ms** across seven segments of that walk, so it does not drift with time,
with battery state or with the evening temperature.

The X4's 500 ms figure stays as measured. These are different panels and the
cost is device-specific -- do not carry either number to a third board without
measuring it there.

#### And on this board a window is dearer than the whole panel

**T5 S3 Pro, whole-panel `FAST`: ~545 ms, against the window's ~1,030 ms**
`[measured, 2026-09-07]`. **A window costs about twice a full frame here**,
which is the reverse of the X4, where the two are the same 500 ms and area does
not enter into the waveform at all.

Instrument: the same `PowerTelemetry` counters, off a 4 h 08 min walk's
`power.csv`, build `0.2.0-t5s3pro`. The two kinds were separated by keeping only
the one-minute intervals in which exactly one of the four refresh counters grew
-- `panel_busy_ms` is a single bucket for all four, so any mixed interval
charges its whole panel time to whichever counter you divide by, and the earlier
1,081 ms figure carried the whole-panel share inside it. Windowed: median
1,030 ms, n=374, p25 1,028, p75 1,055. Whole-panel `FAST`: median 545 ms, n=35,
472 to 668. Both timings are wall clock around the driver call at the same
boundary, `HalDisplay::displayWindow` (`lib/hal/HalDisplay.cpp:118`) and
`HalDisplay::displayBuffer` (`:78`), so they are comparable.

**Why a rectangle costs more than the whole frame is unexplained** `[open]`,
and after a source pass on 2026-09-09 it is unexplained in a stronger sense:
**the two numbers are the same operation.**

The driver question above is answered. The `t5s3pro` env links `LgfxEpdDriver`,
via `freeink-sdk/libs/hardware/BoardT5S3/src/LilyGoT5S3LgfxConfig.cpp`, and that
driver **overrides `displayWindow` not at all**. So
`PanelDriver::displayWindow`'s base implementation runs
`display(bus, fb, prev, RefreshMode::Fast, turnOff)`, the rectangle is dropped,
and `LgfxEpdDriver::display` -- which opens `(void)prev` -- does exactly
`fillCanvasBW(fb)` and `pushCanvas(epd_fast)`. A whole-panel `FAST` calls the
same function with the same mode. **Every one of the walk's 2,608 window
requests took the whole-panel path.**

So the earlier sentence here, "the extra second is inside that driver", was
wrong and is withdrawn: there is no second driver path for it to be inside. It
is worse than that for the comparison -- the whole-panel bracket runs strictly
*more* code than the window one (`resolveReleasedMode`, `consumePrevFrameFor`
and `swapBuffers` in `FreeInkDisplay::displayBuffer`, none of which
`displayWindow` runs), so every difference the source contains points the wrong
way.

The numbers themselves survived being re-checked. Recomputed by hand against
the card's whole history (`docs/power-runs/run8-2026-09-08.csv` in the parent
repo, a superset of run7): windowed **1,030 ms** at n=998, whole-panel `FAST`
**517 ms** at n=73, and the two distributions do not overlap. Not a small
sample, not an averaging artefact.

One mechanism in the code could produce a factor like this, and it is content
rather than path: an unchanged frame arms no pixel, so the panel task runs one
empty pass instead of eleven. Fitted to the two observed modes that is ~52 ms
per pass over a ~455 ms fixed cost `[derived]`. It is falsifiable without a
code change and `t5s3-partial-refresh.md` section 3 says how.

The consequence for the plan is the reverse of what this section used to
advise: **routing the marker path to a whole-panel `FAST` saves nothing**,
because that is already what it does. Only which counter increments would
change. T-277 in the parent repo carries the withdrawal; the blocking itself is
T-263 and its fix is the async path, not a change of waveform.

The consequence for the rider is in [`map-follow.md`](map-follow.md), "A
windowed refresh blocks the loop": at this cost the map's main loop spends up to
a quarter of its wall clock inside a blocking panel call.

**And that 1,081 ms is not a windowed refresh at all.** `LgfxEpdDriver` does not
override `PanelDriver::displayWindow`, so every window request on this board
falls through to a whole-panel push -- all 2,608 of the counted ones did. The
flat quotient of that walk is 1,117 ms; the 1,081 above is the mean of its seven
segments. A fast
frame there is **11** LovyanGFX LUT passes and a clean frame **37**, and most of
the cost is CPU and PSRAM traffic rather than panel time.
[`t5s3-partial-refresh.md`](t5s3-partial-refresh.md) has the chain, the cost
model, the power half and the plan.

## The map never asks for a clean after entry

Everything above is about picking the right mode per frame. There is a separate
hole: **nothing promotes a `FAST` back to a `HALF` over time.** `MapActivity`
cleans on its entry frame and then runs DU forever
(`MapActivity.cpp:4511`/`:4553`), and `Ssd1677Driver` has no
`ghostClearInterval` counter, unlike the IT8951 and Murphy drivers
(`It8951Driver.cpp:66`). A long ride is thousands of DU refreshes with zero
cleans. Two field reports of a pale, half-driven panel, the light mechanism that
explains the shape of the failure (a refresh under a thumb came out white except
under the thumb), the heat story on top of it, and what would settle each:
`eink-refresh-degradation.md`.

## What each mode leaves the panel's rails doing

Separate from ghosting, and found 2026-08-22 while reading vendor guidance. The
`0x22` sequence byte carries the power-down bits in its low two (`0x03` =
ANALOG_OFF_PHASE | CLOCK_OFF), and the X4's three sequences differ:

| mode | X4 sequence | low bits | panel after the refresh |
|---|---|---|---|
| `FAST_REFRESH` | `0xFC` | `0x00` | **clock and analog left enabled** |
| `HALF_REFRESH` | `0xD7` | `0x03` | powered down by the sequence itself |
| `FULL_REFRESH` | `0xF7` | `0x03` | powered down by the sequence itself |

The driver tracks that in one line -- `_isScreenOn = (seqOverride & 0x03) ? false
: !turnOff;` (`Ssd1677Driver.cpp:288`). Note the comment above it says "the
sequence powered the panel down at the end", which is true for `0xD7`/`0xF7` and
**not** for `0xFC`.

`turnOffScreen` does not help here. It defaults to `false` on every public entry
point (`FreeInkDisplay.h:113`, `:164`, `:181`, `:217`, `:220`) and, on a board
with `fastSeqOverride` set, passing `true` only flips the flag -- the sequence
byte is sent as-is, so no power-down bits reach the panel. The only code that
actually drops the rails is `Ssd1677Driver::deepSleep()`
(`Ssd1677Driver.cpp:585-600`), reached from `display.deepSleep()` in the device's
own sleep path (`main.cpp:271`).

**Consequence: on the map screen the rails stay up for the whole session**, since
the entry frame is the last `HALF` and everything after it is `FAST`.

Two reasons that might matter, both **open**:

- **Vendor guidance says do not.** "If some e-Paper screens are powered on for a
  long time, irreversible screen damage may occur", and after each refresh the
  screen should be set to sleep or powered off
  (https://docs.waveshare.com/5inch_e-Paper/FAQ). GxEPD2's author gives the same
  advice for exactly this reason. Against that: stock X4 firmware also uses
  `0xFC` for its partial refresh, so the sequence itself is vendor-sanctioned for
  this panel -- what stock does *between* page turns is not known.
- **Current draw.** An enabled charge pump costs something, and the power
  campaign has never accounted for it (nothing in `power-management.md` or
  `power-idle-sleep.md` mentions the panel's analog rails).

**The fix, if a measurement wants one, is a rails-off call and not
`deepSleep()`.** Deep sleep discards controller RAM and re-arms
`_needsInitialFull`, so the next paint becomes a `HALF` -- 1,684 ms and a clean
nobody asked for. A plain power-down (`0x22 = 0x03` + `0x20`, the tail of what
`deepSleep()` already does) keeps RAM and the differential baseline, and `0xFC`
re-enables clock and analog at the start of the next refresh anyway. There is a
`powerOn()` (`Ssd1677Driver.cpp:326`) and no matching `powerOff()`; that is the
gap.

Tracked as BUG-023 and T-515 in the parent repo. **Nothing here is measured** --
the bits are read off the source, the risk is a vendor sentence, and the cost is
unknown until someone runs a slope with the rails dropped.

## Status

Sequence numbers, promotion rules and which caller uses what: **read off the
source**, cited above. The 500 ms and 1,684 ms figures: **measured on the X4**,
conditions above, one run each for `HALF` -- `FAST` has two ride replays behind
it. The T5 S3 Pro's 1,081 ms windowed figure: **measured on
that board**, one walk, 2,608 refreshes behind it, and it is an average over the
run rather than a single timed frame. `FULL`'s cost is **open**; timing it needs one deliberate `FULL` frame with
the log open, and nothing in the tree wants one.
