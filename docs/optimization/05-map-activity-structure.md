# Plan 05 — `MapActivity`'s shape

Reviewed against `412e0ed9`, which moved the fetch screen out into
`TileSyncActivity` and took 244 lines of `MapActivity.cpp` with it. The
remaining shape is smaller than it was but still the largest thing this fork
wrote.

`MapActivity.cpp` is 1118 lines, `MapActivity.h` is 295. `loop()` is 124 lines.

The code is not tangled — every non-obvious decision carries a comment saying
why. Four separate jobs still share one class, and the class is the only thing
holding them together.

Do this **after** plans 01 and 02, so the refactor moves settled code.

## What is in there

**read**, grouped by job:

| Job | Members | Methods | Lines |
|---|---|---|---|
| Screen furniture | `busyShown_`, compass + badge + readout geometry | `drawCompass` (`:298`), `drawDebugLine` (`:363`), `showBusy` (`:279`), `drawBusyBadge` (`:256`), `busyRect` (`:247`) | ~230 |
| Marker + follow | `markerPatch_`, `markerDrawn*`, `partialMoves_`, `anchorHeading_`, `viewportDrawn_` | `drawPositionMarker` (`:166`), `moveMarker` (`:805`), `saveMarkerPatch` (`:798`), `markerRect` (`:791`), `applyFix` (`:866`) | ~250 |
| Viewport | `proj_`, `source_`, `file_`, `last*`, `showingPersistedFix_` | `renderViewport` (`:917`), `renderCurrent` (`:783`), `renderWaiting` (`:770`) | ~260 |
| Ladders + settings | `zoomStep_[]`, `markerStep_[]`, `mode_`, `redrawDueMs_`, `saveDueMs_` | `stepZoom` (`:672`), `stepMarker` (`:685`), `armRedraw` (`:696`), `armSave` (`:703`), `publishLadders` (`:705`), `syncLaddersFromConsole` (`:707`), `saveLaddersIfChanged` (`:728`), `switchMode` (`:659`) | ~190 |

Plus `loop()` (`:490-613`) arbitrating between all four, the popup, two consoles
and two deadlines.

The problems are narrow and concrete:

- **Every field is reachable from every method.** The subtlest invariants in this
  file are exactly about that boundary: the marker patch must be saved after
  everything else is drawn (`:1063-1068`), and any frame that is not a real map
  frame must clear `viewportDrawn_` and `markerPatchValid_` by hand
  (`renderWaiting` at `:780-781`). Nothing enforces either. They are held by
  comments.
- **`busyShown_` is cleared in three places** that each have to remember
  (`:781` in `renderWaiting`, `:1116` at the end of `renderViewport`, and via
  `renderCurrent()` on the popup-close path at `:513`). One missed assignment
  means a stale badge or a wasted refresh.
- **The header is where the design lives.** 295 lines of it, most of it load
  bearing, none of it findable from the code it explains.

## The target

```
MapActivity          coordinator + lifecycle + loop() arbitration
  MapChrome          compass, debug readout, busy badge, button hints
  MapMarkerLayer     patch save/restore, marker draw, move-vs-reanchor
  MapLadders         zoom/marker/mode state + the debounced settings save
  (existing)         MapTileSource, MapProjection, MapConsoleState, MapFollow
```

All three new pieces are plain classes, not `Activity` subclasses. Nothing here
is a screen; each is a part of one screen. Target: `MapActivity.cpp` around 400
lines.

## Extraction order

Least coupled first, so each commit is reviewable alone.

### 1. `MapChrome`

Takes `GfxRenderer&` and `MappedInputManager&`. Owns the compass constants
(`:88-107`), the busy geometry (`:69-73`), the readout line positions
(`:57-64`), and `busyShown_`.

```cpp
void drawCompass(uint8_t headingStep);
void drawDebugLine(int y, char* text);   // keeps the trim-to-width behaviour
void drawButtonHints(const MappedInputManager::Labels&);
bool showBusy();                          // false if the window was rejected
void onFullFrame();                       // clears the busy latch
```

`busyShown_` moving out is the point. It is a latch about the panel, not about
the map, and `onFullFrame()` replaces three assignments that must not be
forgotten with one call each full-frame path already wants to make.

`drawDebugLine`'s trim loop must move verbatim, including the reason:
`GfxRenderer::drawText` does not clip and `drawPixel` answers every off-panel
pixel with a `LOG_ERR`, so one overlong line is hundreds of error lines over USB
CDC (`:363-372`, confirmed by `GfxRenderer.cpp:497-500`).

Pure move. Screenshot must be byte-identical.

### 2. `MapLadders`

Owns `zoomStep_[]`, `markerStep_[]`, `mode_`, `saveDueMs_`, and the settings
read/write. Depends on nothing but a clock.

```cpp
bool step(Axis axis, int delta);   // false if a ladder end was hit
bool setMode(MapRideMode);         // false if already current
void tick(uint32_t nowMs);         // performs the debounced save
void flush();                      // onExit()
uint8_t zoomStep() const; uint8_t markerStep() const; MapRideMode mode() const;
```

Two reasons it earns its own class:

- The "in memory, not read back from settings on a mode switch" rule
  (`MapActivity.h:217-224`) is a real invariant with a real failure mode: switch
  mode and back inside the four-second save window and the step you just chose is
  gone. It belongs next to the state it protects.
- It becomes host-testable with a fake clock: ladder ends, mode switching, and
  the value-change guard in `saveLaddersIfChanged()` (`:728-766`) are all pure
  logic. `MapFollow` set this precedent and it paid off (`test/map_follow`).

The last-fix persistence rides in the same save (`:740-759`) — keep it there,
same debounce for the same reason — but `showingPersistedFix_` belongs to the
viewport, so pass it in rather than moving it.

### 3. `MapMarkerLayer`

Largest, and last, because it holds the invariant that is easiest to break:
**the patch is saved after everything else is drawn, and the marker goes down
last** (`:1063-1068`). Draw it earlier and a marker low on the screen comes back
with the button hints painted through it.

```cpp
void anchor(int16_t x, int16_t y, uint8_t headingStep);   // after a full frame
Action apply(int16_t fixX, int16_t fixY, uint8_t heading); // move, or ask for a reset
bool valid() const;
void invalidate();                                        // non-map frames
```

`MapFollow` stays exactly as it is — pure, host-tested, no renderer, no
projection, no HAL (`MapFollow.h:16-20`). This class holds the patch buffer and
talks to the renderer; `MapFollow` keeps deciding.

Carry the comment at `:1063-1068` across verbatim. It was learned the hard way.

## `loop()` afterwards

Same order, same early returns, one line each:

```
1. popup owns input     -> return
2. BLE fix              -> marker_.apply() or renderViewport()
3. console lines        -> ladders_.sync() + redraw
4. buttons              -> ladders_.step() + chrome_.showBusy()
5. deadlines            -> redraw, missing-tiles flush, ladders_.tick()
6. Back                 -> home
```

Every step's position has a reason in the current comments. Move the comments
with the code — especially the Back-release swallowing after a popup close
(`:498-515`), which is two edges of one physical press and reads as a bug
without its explanation.

## While in here: two loose ends from `412e0ed9`

Both are plan 04 items; naming them here because this is the plan that touches
the same file.

- `MapActivity` still owns and attaches a `MapTransferReceiver`
  (`MapActivity.h:294`, `.cpp:385`) but no longer drains landed tiles from
  `MissingTilesStore`. Decide whether the map screen accepts pushes at all; if
  not, `transfer_` and its readout line (`:1051`) come out and this class gets
  smaller for free.
- `MissingTilesConsoleSource` is now a shared header (`:48`) used by both
  screens. Good. The drain logic is the other half of that pair and is currently
  in only one of them.

## What not to do

- **Do not route `MapActivity`'s drawing through `requestUpdate()` / the render
  task.** It has always drawn straight to the buffer on the main task, and the
  popup depends on it (`MapActivity.h:170-176`). Changing that is a separate
  decision with its own risks.
- **Do not merge the two consoles.** One assembler per channel, one shared
  state, deliberate, with a stated reason (`MapCommandConsole.h:269-276`).
- **Do not thin the header comments.** They are the design record. Move each one
  to the class that now owns it; do not summarise.
- **Do not make any of the three pieces an `Activity`.** They have no lifecycle
  of their own, and the map screen's `onEnter`/`onExit` pairing with
  `BlePositionServer` is what would break (`MapActivity.cpp:379-386`, `:471-476`).

## Gate

Every commit: `CMD:SCREENSHOT` byte-identical before and after, on the waiting
screen, a rendered viewport, and the popup. A decomposition that changes a pixel
changed behaviour.

Host tests keep passing, and `MapLadders` arrives with its own.
