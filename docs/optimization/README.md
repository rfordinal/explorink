# Firmware review, 2026-08-06

Full read of the map/BLE/tile code this fork owns, plus the inherited paths it
leans on. One plan per area. Each plan is a change list, not a rewrite.

Scope: the code TrailInk added (`src/activities/map/`, `lib/BlePositionServer/`,
`src/MissingTiles*`) and the inherited code the map path actually runs through
(`lib/GfxRenderer/`, `lib/hal/`, `src/main.cpp`). Inherited code is only touched
where the map needs it, per the fork rule in `CLAUDE.md`.

Reviewed against `develop` at `412e0ed9`. `1eabf58a` was HEAD when the read
started and `412e0ed9` landed part-way through, so plans 04 and 05 were rewritten
against the new shape and every `MapActivity.cpp` citation was re-checked. If a
line number is off by a few, the surrounding function name is the anchor.

## The plans

| Plan | Area | Biggest single item |
|---|---|---|
| [01-render-pipeline.md](01-render-pipeline.md) | projection, area fill, stroke, canvas | ESP32-C3 has no FPU; the per-point projection is software `double` |
| [02-tile-io.md](02-tile-io.md) | `.tib` reading, CRC, pass structure | every drawn layer is read off the card twice per pass |
| [03-ble-link.md](03-ble-link.md) | `BlePositionServer`, transfer channel | the negotiated MTU is never requested and never logged |
| [04-tile-sync.md](04-tile-sync.md) | missing list, `TileSyncActivity` | a sync that goes quiet never ends; arrivals on the map screen no longer clear the list |
| [05-map-activity-structure.md](05-map-activity-structure.md) | `MapActivity` shape | 1118 + 295 lines, four responsibilities in one class |
| [06-memory-and-flash.md](06-memory-and-flash.md) | RAM and flash budget | measured: flash 58% used, static DRAM 58 KB — the constraint is elsewhere |
| [07-power-and-lifecycle.md](07-power-and-lifecycle.md) | CPU scaling, sleep, BLE lifetime | the map screen pins the CPU at 160 MHz forever |
| [08-verification.md](08-verification.md) | tests, gates, instrumentation | CI never runs the 20 host test suites |

## Confidence labels

Every claim in every plan carries one:

- **read** — read off the code at the cited `file:line`. Not measured.
- **measured** — a number taken from this machine or from hardware, with how.
- **open** — not established. The plan says what measurement settles it.

No plan asks for a change on an **open** claim alone. Where a change depends on a
measurement, the measurement is step 1 of that plan.

## Ordering

Dependency order, not importance order.

1. **08's first item — CI runs `ctest`.** One workflow file. Twenty host suites
   exist and nothing enforces them, so every refactor below is currently
   unprotected.
2. **02 tile I/O**, starting with its stats fix — the counters that the rest of
   the plans use as a gate currently report one pass instead of the frame.
3. **01 render pipeline** — same target as 02 (the cost of one viewport reset),
   measurable with instrumentation that already exists.
4. **03 BLE link** — independent of the render path, and it decides whether the
   fetch feature is usable at all once a real phone is on the other end.
5. **04 tile sync** — three short fixes, one of which is a regression from
   `412e0ed9`.
6. **05 structure** — after 01/02 land, so the refactor moves settled code.
7. **06 memory** and **07 power** — both start with a measurement, and 07's is a
   hardware measurement that needs the user.

## Two things found while reviewing

### The working tree does not build

**measured**, this machine, 2026-08-06:

```
gen_mapstyle.py: layers.water.rules[0]: water rules must match on `class`
(['lake', 'river', 'stream', 'unknown']), not on OSM tags -- the tile carries
the class, not the tags
========================== [FAILED] Took 5.09 seconds ==========================
```

Cause is the uncommitted `data/mapstyle.json` in `firmware/trailink`, not the
committed tree. The working-tree copy keys water rules on OSM tags
(`match: {waterway: [river]}`); `develop`'s committed copy keys them on `class`
(`match: {class: [river]}`), which the generator has required since the water
class byte landed. A clean checkout of `develop` builds — verified, 8m49s from
scratch, `SUCCESS`.

So the working copy is a pre-migration style file, not a new edit. Left alone on
purpose: another session owns that file. Whoever owns it either re-does the edit
against the current schema or discards it.

### Arrivals on the map screen no longer clear the missing list

**read**, and new in `412e0ed9`. `MapActivity` still owns and attaches a
`MapTransferReceiver` (`MapActivity.h:294`, `.cpp:385`), but
`drainTransferredTiles()` moved to `TileSyncActivity` and was not left behind. So
a tile pushed while the map screen is up lands on the card and stays on the
missing list, and the next sync asks the phone for it again.

Full write-up and both fix options in [04-tile-sync.md](04-tile-sync.md), item 1.

## What the review did not find

Worth stating, because it changes what these plans are for.

- **No memory-safety bug in the map path.** Every multi-byte decode goes through
  `memcpy` (`MapTileReader.cpp:79-131`, `BlePositionServer.cpp:282-290`), the way
  point count is bounded in the reader rather than in each caller
  (`MapTileReader.cpp:225`), and the transfer path validator rejects `..`,
  absolute paths and non-ASCII (`MapTransferReceiver.cpp:338-373`).
- **No unbounded allocation on the render path.** `MapTileSource` allocates
  nothing after construction (`MapTileSource.h:17-24`), and the per-session heap
  pair is allocated in `onEnter()` and logged as a before/after delta
  (`MapActivity.cpp:420-440`).
- **No two-writer race on `MissingTilesStore`.** The NimBLE host task publishes a
  coordinate and an activity task does the removal
  (`MapTransferReceiver.h:98-115`, `TileSyncActivity.cpp:172-184`).
- **No `std::function` and no `std::string` on any hot path.** Hooks are plain
  function pointers with a `ctx` (`BlePositionServer.h:121-128`).

The code quality is high and the comments carry the reasoning. These plans are
about cost, shape and coverage — they are not a bug list.
