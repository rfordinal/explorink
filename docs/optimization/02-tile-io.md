# Plan 02 — tile I/O

How many bytes one viewport reset pulls off the card, and why it is several
times more than the map on screen.

`MapTileSource::bytesRead()` already counts real file bytes
(`MapTileSource.h:98-103`), so every item here is measurable today with no new
instrumentation. That counter is the gate.

## The read amplification

**read.** `MapRenderer::render()` calls a `begin*()` pass seven times per frame:

| Pass | Call site | Layer |
|---|---|---|
| landuse, built-up | `MapRenderer.cpp:86` → `drawLanduseClass` → `:59` | Landuse |
| landuse, forest | `MapRenderer.cpp:87` → `drawLanduseClass` → `:59` | Landuse |
| buildings | `MapRenderer.cpp:90` | Buildings |
| water | `MapRenderer.cpp:102` | Water |
| roads, black strokes | `MapRenderer.cpp:134` | Roads |
| roads, white fills | `MapRenderer.cpp:147` | Roads |
| places | `MapRenderer.cpp:160` | Places |

Every one of those calls `MapTileSource::startPass()`, which rewinds to tile 0
(`MapTileSource.cpp:49-56`) and re-walks the whole range. Per tile, per pass,
`advanceToNextTile()` (`MapTileSource.cpp:58-107`) does:

1. `reader_.open()` — reads the 36-byte header plus the layer directory and
   CRC32s both (`MapTileReader.cpp:68-133`).
2. `hasLayer()` — free, directory is in RAM.
3. `beginLayer()` → `validateLayerCrc32()` — **reads the entire layer** to
   check its CRC (`MapTileReader.cpp:135-150`).
4. The record walk — **reads the entire layer again** through `refill()`
   (`MapTileReader.cpp:183-193`).

So for a 3×3 range:

- **63 file opens and 63 header CRC computations** per frame, where 9 would do.
  The file cannot change under us mid-frame.
- **Every drawn layer's bytes are read twice per pass** — once for its CRC,
  once for its records.
- **Roads and landuse are walked twice**, so those layers are read **four
  times** per frame.

The docs already record the raw scale: buildings alone were 277 KB of the
364 KB a four-tile viewport read (`docs/map-data-spec.md`, "RAM budget", quoted
at `IMapSource.h:73-76`). With buildings on, that is 277 KB × 2 = 554 KB of
card reads for one layer of one frame.

**open**: the actual current `bytesRead()` per rung. Read it off `info` (the
console pushes it, `MapActivity.cpp:1078`) before touching anything.

## Step 1 — parse each tile's header once per reset

The header and directory are 36 + 13×layers bytes, at most 114 bytes for
`kMaxLayers = 6` (`MapTileReader.h:32`). Nine of those is ~1 KB.

Change: `MapTileSource::begin()` gains an optional header cache — one
`{valid, originX, originY, LayerEntry[6], layerCount}` per tile index, filled
on first open, reused by every later pass. `MapTileReader` grows a way to be
handed an already-parsed header instead of re-reading it:
`openWithHeader(file, path, const Header&)`.

Cost: ~1 KB of `MapTileSource`, which is already ~5 KB and heap-allocated once
per session (`MapActivity.cpp:425`). Justified: it removes 54 file reads and 54
software CRC32 passes over 114 bytes each, per frame.

Keep the `open()`-parses-it path as well — the golden test and the native
preview open tiles standalone.

## Step 2 — validate each layer's CRC once per reset, not once per pass

Add a "this (tile, layer) pair already passed CRC in this reset" bitmap to
`MapTileSource`. Nine tiles × six layers = 54 bits, so one `uint64_t`. Cleared
in `begin()`, exactly like `unavailableMask_` (`MapTileSource.cpp:21`).

`beginLayer()` gains a `bool skipCrc` argument, or `MapTileSource` calls a new
`beginLayerUnchecked()` when the bit is already set.

This kills the second CRC pass over the roads layer and the second over
landuse. Combined with step 1, a frame's card reads drop from

```
7 passes x (header + layer CRC + layer data)
```

to

```
9 headers + 5 layer CRC passes + 7 layer data passes
```

Correctness note: the CRC's job is to catch a corrupt or truncated file, and a
file cannot become corrupt between two passes of the same frame. Skipping the
repeat check loses nothing. Say so in the comment — `MapTileReader.h:86-92`
currently promises a check on every `beginLayer()`, and that promise changes.

## Step 3 — use the ROM CRC32 on device

`MapTileReader`'s `crc32Update` is a bitwise loop: **8 iterations per byte**,
each with a shift, an AND and an XOR (`MapTileReader.cpp:28-37`).

The same repo already uses the hardware/ROM one three files away:
`esp_rom_crc32_le` in `MapTransferReceiver.cpp:388` (include at `:6`). It is
table-driven in ROM, same polynomial, same reflected convention. With
`crc = 0` seeded and no final XOR it matches `zlib.crc32`, which is what
`mapbuilder/tilegen/tiles.py` writes (`MapTileReader.cpp:24-27`).

Change: keep the software loop for the host build, use the ROM one on device.

```cpp
#if defined(ESP_PLATFORM)
#include <esp_rom_crc.h>
inline uint32_t tibCrc32(uint32_t crc, const uint8_t* d, size_t n) {
  return esp_rom_crc32_le(crc, d, n);   // crc seeded 0, no final xor
}
#else
inline uint32_t tibCrc32(uint32_t crc, const uint8_t* d, size_t n) { /* current loop */ }
#endif
```

Both forms must be seeded and finalised the same way. The current code seeds
`0xFFFFFFFF` and XORs at the end (`MapTileReader.cpp:118-121`); the ROM form
seeds `0` and does not. Convert the call sites, not the polynomial.

Gate: `test/map_tile_reader`'s golden test already round-trips real `.tib`
files built by `mapbuilder`. If the seeding is wrong, that test fails loudly —
which is the point of doing this behind it.

**open**: how many milliseconds this is worth. It is a pure win either way, but
the size of it depends on how much of the layer bytes survive steps 1-2.

## Step 4 — fix what the stats mean

Two counters lie, in a way that has already cost debugging time.

`startPass()` resets `tilesOpened_` and `tilesUnavailable_`
(`MapTileSource.cpp:52-54`), and `unavailableMask_` does **not** reset — it
accumulates until the next `begin()` (`MapTileSource.h:78-84`, deliberate).

So by the time `renderViewport()` reads them (`MapActivity.cpp:1036`,
`:1057`, pushed to the console at `:1078`), `tilesOpened` and
`tilesUnavailable` describe **the places pass only** — the last one that ran —
while `unavailableMask` describes the whole frame. The debug readout's `%lut`
and `info`'s `tiles_ok` / `tiles_missing` are therefore not the frame's numbers.

The header comment says this ("counted over the pass that is running or most
recently ran", `MapTileSource.h:69-71`) so it is documented, not hidden. It is
still the wrong number to put on screen.

Fix: keep the per-pass counters if anything wants them, and add frame-scoped
ones reset only in `begin()`: `tilesOpenedThisFrame_`,
`tilesUnavailableThisFrame_` — or simplest, derive both from
`unavailableMask_` and the range count, which are already frame-scoped.
`popcount(unavailableMask_)` is the missing count; `range.count() - that` is
the available count.

Same change should make `waysEmitted()` honest about `kRoadPasses`. It sums
across passes on purpose (`MapTileSource.h:86-90`), and the readout prints it
raw as `%luw` (`MapActivity.cpp:1036`), so the number on screen is roughly
double the ways in the picture. Either divide at the call site or label it.

This is a correctness-of-instrumentation change and it should land **first**, in
front of steps 1-3 — otherwise the gate for those steps is a lying counter.

## Step 5 — do not read a layer whose style draws nothing

Mostly already done. `MapRenderer` guards buildings, water and landuse behind
`style.*Enabled` (`MapRenderer.cpp:82`, `:90`, `:102`) and
`drawLanduseClass` bails before walking when the class has no tone, no hatch
and no outline (`MapRenderer.cpp:55-58`).

Two gaps left:

- **Places.** Guarded by `placeDotDiameterPx > 0` (`MapRenderer.cpp:160`),
  which is right, but the place **name** is read off the card for every place
  and thrown away — `readPlaceName` always consumes `nameLen` bytes
  (`MapTileReader.cpp:245-258`) and `nextPlace` stores it in `name_`
  (`MapTileSource.cpp:178`), and nothing draws a place label yet. That is
  correct as stream discipline (the bytes must be consumed to reach the next
  record) but it means the places layer costs its full size for a few dots.
  No change needed now. Worth a comment so it is not mistaken for a bug when
  labels land.
- **Water class widths.** A water class with `waterLinePx[c] == 0` is read,
  projected and then drawn at width 0 (`MapRenderer.cpp:111-120`,
  `MapAreaFill::outlineRing` returns immediately at `:177`). Cheap to skip the
  projection instead — that is plan 01's step 3 territory, and the same trick
  applies here.

## Step 6 — the pass-outer order stays

For the record, because it will be the first idea anyone has: **do not reorder
to tile-outer.** All black road strokes must precede all white road fills, or
every tile seam and every junction shows a broken casing (`MapRenderer.cpp:126-133`,
`docs/map-render-spec.md`). Tile-outer would let a later tile's white fill
punch through an earlier tile's black edge. The pass-outer order is a
correctness requirement, and steps 1-3 exist precisely because they cut the
cost of it without touching it.

## Commit sequence

1. `fix(map): frame-scoped tile counters` — step 4. Lands first; it is the gate.
2. `perf(map): parse a tile header once per viewport reset` — step 1.
3. `perf(map): validate a layer's crc once per reset` — step 2.
4. `perf(map): use the ROM crc32 on device` — step 3.
5. `docs(map): why the places layer is read in full` — step 5, comment only.

Record before/after `bytesRead()` and the readout's `%lums` for each, at the
same coordinate and rung, in `docs/PROGRESS.md`.
