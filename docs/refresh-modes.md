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

## Status

Sequence numbers, promotion rules and which caller uses what: **read off the
source**, cited above. The 500 ms and 1,684 ms figures: **measured on the X4**,
conditions above, one run each for `HALF` -- `FAST` has two ride replays behind
it. `FULL`'s cost is **open**; timing it needs one deliberate `FULL` frame with
the log open, and nothing in the tree wants one.
