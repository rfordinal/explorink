# Map memory cost, measured on hardware

What the map screen costs in RAM, measured on a real X4 on 2026-08-10, plus
where the cost actually sits. `docs/optimization/06-memory-and-flash.md` asked
for exactly one number ("what is the free heap on the map screen with BLE up")
and left it open. This doc answers it.

Confidence labels, same as the optimization plans: **measured** (a number off
this device or this machine), **read** (read off the code at the cited line),
**open** (not established; the doc says what settles it).

## How the numbers were taken

The firmware already prints the whole heap picture every 10 s whenever serial is
attached (`src/main.cpp:520-528`):

```
[INF] [MEM] Free: 49460 bytes, Total: 246260 bytes, Min Free: 37764 bytes, MaxAlloc: 42996 bytes
```

`Min Free` is `ESP.getMinFreeHeap()` — the high-water mark since boot. So plan
06's step 1 ("add `ESP.getMinFreeHeap()` to the reset log line") needs no
firmware change: the line exists, it is global, and it only needs a serial
capture with the map screen up. Capture used `/dev/ttyACM0` at 115200, with
`CMD:GOTO_MAP` (`src/main.cpp:686-712`) driving the screen.

Device: X4, panel 800x480, `EINK_DISPLAY_SINGLE_BUFFER_MODE=1`.

## Measured: the heap on the map screen

| State | Free | Min free (since boot) | MaxAlloc |
|---|---|---|---|
| boot idle, no map, no BLE (10 s uptime) | 124,564 | 124,480 | 114,676 |
| map screen, BLE up, one central connected, idle | 49,460 | 37,764 | 42,996 |

Both rows are the build as it was before the BLE config trim below. After the
trim the same map screen reports 58,540 free — the numbers in this section are
kept as the baseline every later measurement is compared against.

**Total heap is 246,260 bytes**, not 380 KB. 380 KB is the SRAM the chip has;
static DRAM, IRAM and the framebuffer come off it before the allocator sees
anything.

So the map screen costs **75,104 bytes — 60 % of the free heap the device had
before it opened** (measured, two runs 12 minutes apart: 49,460 and 49,468).

**The floor is 37,764 bytes.** The project's own testing checklist wants
`ESP.getFreeHeap()` above 50 KB (firmware `CLAUDE.md`). The map screen idles
below that gate and its high-water mark is 12 KB below it. That is the finding —
not flash, not static DRAM.

Two more measured facts from the same capture:

- **A transient ~11.7 KB sits under the resident figure.** Free stayed flat at
  49,460 for every 10 s sample while min free was 37,764, so something spent
  ~11.7 KB and gave it back between samples. Not attributed — **open**.
- **Fragmentation is ~4.4 KB.** 49,460 free but the largest block is 42,996–45,044,
  so a single allocation above ~43 KB fails on a screen that reports 49 KB free.

## The T5 S3 Pro is not the X4's heap

Every number above is an **X4** number (ESP32-C3). Measured on a **LilyGo T5 S3
Pro** (ESP32-S3) 2026-09-07, over USB serial, map screen up, one BLE central
subscribed, the CONFIRM menu opened:

| | X4 (C3) | T5 S3 Pro (S3) |
|---|---|---|
| Total heap | 246,260 | 305,468 |
| Free, map screen | 49,460 | 120,784 |
| Min free since boot | 37,764 | 120,720 |
| MaxAlloc (largest block) | 42,996 | 77,812 |

Source: `main.cpp`'s `MEM` line, `[20042] [INF] [MEM] Free: 120784 bytes, Total:
305468 bytes, Min Free: 120720 bytes, MaxAlloc: 77812 bytes`. The menu backdrop
in the same capture logged `menu backdrop 19100 bytes (382x388), free heap
119388`.

So the S3 board has **2.4x the free heap and 1.8x the largest block**, on a
panel whose framebuffer is *bigger* (540x960 = 64,800 bytes against the X4's
48,000). A window refresh that `windowRefreshAffordable()` would refuse on an X4
can be affordable here, and a buffer sized against 45 kB leaves this board idle.

The rule that follows: **anything quoting "the largest block" or "free heap on
the map screen" names the board** (`CLAUDE.md`, measurements name the device).
One C3 binary drives X4 and X3 and a separate S3 binary drives this one, so a
build string does not say which hardware produced a number either.

Not measured on the S3: what the map session itself allocates, the BLE share, or
whether the ~11.7 KB transient below has a counterpart here. Only the totals
above were read.

## Measured: what one map session allocates

From `MapActivity::onEnter()`'s own before/after log (`MapActivity.cpp:1197-1219`):

```
[MAP] heap: 57260 before source alloc, 49564 after, delta 7696 (sizeof MapTileSource = 6696)
```

- `MapTileSource` 6,696 + marker patch 720 (`MapActivity.cpp:1211`) + allocator
  headers = **7,696 bytes, once per session**.
- **Per tile load: 60–68 bytes** (`heap: 49528 before tile load, 49460 after`).
  The streaming claim holds — a 603 KB, 4-tile, 2,646-way reset moves ~64 bytes
  of heap.

That leaves **67,304 bytes** between "boot idle" and "before source alloc". Of it,
`MapActivity` itself is 2,272 bytes (measured, below). The rest is
`BlePositionServer::begin()`.

## Measured: BLE is 86 % of the map screen's heap cost

`begin()` and `end()` now log a heap bracket
(`lib/BlePositionServer/src/BlePositionServer.cpp`), so the split is a number and
not a subtraction. Flashed and measured 2026-08-10:

```
[BLEPOS] heap: 121796 before begin, 57256 after, delta 64544
[BLEPOS] heap: 47140 before end, 105944 after, returned 58804
```

**`begin()` costs 64,544 bytes** (three runs: 64,888 / 64,540 / 64,544 — the
spread is whether a central is connected). Against 7,696 for the whole tile
streaming path, that is **86 % of the map screen's heap cost in the BLE stack,
14 % in the map.**

**open — `end()` returns 58,804 of it, 5,740 bytes short.** Free heap does come
back fully by the next `begin()` (121,796 vs 124,576 at boot, and the 2,780
difference is the live `MapActivity` plus its file source), so this is not a leak
across sessions. What is unaccounted for is *when* those 5,740 bytes come back —
`NimBLEDevice::deinit(true)` does not appear to return everything inside the
bracket. Settle it by sampling the heap again a second after `end()`.

## Measured: a real transfer costs no heap

Pushed a 42,681-byte tile with `tools/blepush.py` while the map screen was up
(`MAPXFER begin` -> `MAPXFER done`, crc verified, 2.6 KB/s):

- **Free heap flat at 49,136 through the whole transfer**, min free unchanged at
  47,032. The receiver streams to the card; it does not buffer the file.
- NimBLE host task high-water: **2,152 of 4,096 bytes free** at completion
  (logged by `MAPXFER done`).

So plan 06's worst case — map + BLE + a central + a transfer in flight — floors
at **47,032 bytes**, and the 37,764 seen before this build was an older session,
not this path.

## Measured: struct sizes, riscv32

From DWARF in `.pio/build/default/firmware.elf` (`riscv32`, so pointers are 4
bytes — a host `sizeof` would lie):

| Bytes | Type | Lifetime |
|---|---|---|
| 6,696 | `MapTileSource` | per map session, heap |
| 5,136 | `MapTileReader` (inside the above) | — |
| 2,272 | `MapActivity` | per map session, heap |
| 1,760 | `MapRouteSource` | only when a route was picked |
| 1,568 | `TileSyncActivity` | per sync-screen visit |
| 1,152 | `MapRouteReader` (inside `MapRouteSource`) | — |
| 588 | `StaleTilesList` | member of `MapActivity` |
| 424 | `MapRouteFit` | — |
| 404 | `MapTransferReceiver` | member of `MapActivity` |
| 312 | `BlePositionServer` | singleton |
| 216 | `MapStyle` | — |

`MapTileSource`'s 6,696 bytes break down as (DWARF member offsets):

| Bytes | Member | Note |
|---|---|---|
| 4,096 | `MapTileReader::streamBuffer_` | the tile read buffer (`MapTileReader.h`) |
| 780 | `MapTileReader::cells_` | per-layer cell index |
| 512 + 512 | `xs_`, `ys_` | 256 projected points, `int16_t` |
| 160 + 64 | `path_`, `name_` | fixed char buffers |
| 144 | `layers_` | 12 `LayerEntry` slots |
| 128 | `contentIds_` | 32 tiles x `uint32_t` |
| ~400 | counters and stats | ~30 `uint32_t` telemetry fields |

Plan 06 quoted `sizeof(MapTileSource)` as "≈ 5.5 KB". It is 6,696 today —
corrected there in the same pass as this doc.

**Static DRAM has no map in it.** Largest `.bss`/`.data` symbols on this build
are `g_cnxMgr` (WiFi, 3,880), `xIsrStack` (2,096), `BidiUtils` shaping buffers
(1,536 each), `s_wifi_nvs` (1,308). Everything above 16 KB in the symbol table
is flash-mapped font and hyphenation data at `0x3c...`, not DRAM.

## Verified not the problem

Worth stating, because each one is a plausible 48 KB suspect and each one is
clean:

- **The 48 KB async shadow is never allocated.** `FreeInkDisplay` lazily mallocs
  a framebuffer-sized baseline for async refresh
  (`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:571-584`).
  No firmware code calls `displayBufferAsync`, `triggerDisplayAsync` or
  `displayBufferAsyncNoShadow` (grep over `src/` and `lib/`: no hits), so
  single-buffer mode really is one buffer. **read.**
- **The 48 KB grayscale store never runs on the map.** `storeBwBuffer()`
  (`lib/GfxRenderer/GfxRenderer.cpp:2115-2140`) copies the whole framebuffer in
  chunks. Nothing under `src/activities/map/` mentions `GrayscaleFrame`,
  `storeBwBuffer` or grayscale at all. **read.**
- **Flash is 58 % used with 2.7 MB spare**, and static DRAM is 57.8 KB of the
  budget (plan 06, measured 2026-08-06).
- **Tile content does not grow the heap.** 60–68 bytes per reset, measured above.

## The BLE config is sized for a device this is not

**read**, from the Arduino precompiled config
(`~/.platformio/packages/framework-arduinoespressif32-libs/esp32c3/sdkconfig`).
This firmware is a BLE peripheral that serves exactly one phone and never scans:

| Setting | Value | What this device needs |
|---|---|---|
| `BT_NIMBLE_MAX_CONNECTIONS` | 3 | 1 |
| `BT_NIMBLE_ROLE_CENTRAL`, `..._OBSERVER` | y | no `NimBLEClient` / `NimBLEScan` anywhere in `src/` or `lib/` |
| `BT_CTRL_BLE_MAX_ACT` | 6 | 1 advertising + 1 connection |
| `BT_CTRL_SCAN_DUPL_CACHE_SIZE` | 100 | never scans |
| `BT_NIMBLE_ACL_BUF_COUNT` x `ACL_BUF_SIZE` | 24 x 255 = 6.1 KB | MTU is 256 (`BLEPOS MTU now 256`) |
| `BT_NIMBLE_MSYS_1/2` | 12x256 + 24x320 = 10.8 KB | one connection's worth |
| `BT_NIMBLE_HOST_TASK_STACK_SIZE` | 5,120 | high-water left 2,076 of 4,096 free in a real transfer (`docs/PROGRESS.md`) |
| `BT_NIMBLE_MAX_BONDS` | 3 | 1 |
| `BT_NIMBLE_WHITELIST_SIZE` | 12 | 0 |

The mechanism to change them already exists in this repo: `custom_sdkconfig` in
`platformio.ini`, which rebuilds the core libs and already reclaims ~32–37 KB by
right-sizing timer task stacks and moving WiFi out of IRAM (MEMFIX-PORT).

## Measured: trimming that config returns 9 KB

Landed in `platformio.ini`'s `custom_sdkconfig` and verified on the device
2026-08-10 — one connection, no central, no observer, one bond, 12 ACL buffers,
smaller msys pools, a 4,096-byte host task stack, `BT_CTRL_BLE_MAX_ACT=2`:

| | Before | After | Change |
|---|---|---|---|
| `begin()` cost | 64,544 | **56,972** | −7,572 |
| Free heap, map screen | 49,472 | **58,540** | +9,068 |
| Min free, map screen | 49,376 | **58,444** | +9,068 |
| Largest block | 45,044 | **55,284** | +10,240 |
| Total heap | 246,260 | 247,156 | +896 |
| Static DRAM (`.data` + `.bss`) | 57,777 | 57,081 | −696 |

**The map screen is now above the project's own 50 KB gate**, at 58.5 KB idle and
57.9 KB with a transfer in flight (min free 57,932 during a verified 42,681-byte
push).

Verified working after the trim, not just building: map renders on enter, a
central connects at MTU 256, and a 42,681-byte tile push completes with a
matching crc at the same 2.6 KB/s. **Watch the host task stack** — its high-water
now leaves 1,124 of 4,096 bytes free during a transfer (was 2,152 of 5,120), so
~2,970 bytes is the real usage and 4,096 is the smallest safe setting. Do not cut
it further without re-reading that number off `MAPXFER done`.

**Untested after the trim: a real phone sending positions.** A laptop central is
no substitute — BlueZ negotiates a 420 ms supervision timeout, and a 2 s viewport
render blocks past it, so the link drops on every redraw
(`tools/blefakephone.py` died on the first position write for exactly that
reason). The phone app negotiates 500 units and survives. Needs one ride with
the app.

## Levers, largest first

| Lever | Size | Confidence | Cost |
|---|---|---|---|
| ~~Trim NimBLE + controller config~~ | **9,068 bytes, done** | measured | landed 2026-08-10 |
| ~~Gate the chrome snapshots on a digitizer~~ | **8,448 bytes on a C3, done** | measured | landed 2026-09-13, below |
| Cut BLE further: `BT_CTRL_BLE_MAX_ACT=1`, smaller ACL pool, MTU below 256 | unknown | open | each one risks a transfer stall; verify with a push every time |
| Right-size `ActivityManagerRender` task stack (8,192, `ActivityManager.cpp:36`) | up to a few KB | open | measure `uxTaskGetStackHighWaterMark` on the map's deepest render |
| `MapTileReader::streamBuffer_` 4,096 -> 2,048 | 2 KB | read | more SD reads per layer; gate on the reset time already logged |
| Account for the 5,740 bytes `end()` does not hand back | up to 5.7 KB | open | sample the heap a second after `end()` |

## Measured and settled: the chrome snapshots cost the C3 8.4 KB

Taken 2026-09-12 on an **Xteink X3** (ESP32-C3), flashing two builds back to
back and reading the same instrument in the same state: map screen up, a phone
connected (`mtu=256`), `pos 48.4363 17.0206`, `zoom 4`, `tiles_ok=0`. The
numbers come from `stats` (`INFO heap`, `INFO min_heap`), not from a log line,
so both readings are the same code path.

| | `develop` 85af8066 | `develop` + both release branches, ad5311de | delta |
|---|---|---|---|
| free heap | 33,644 B | 25,188 B | **-8,456 B** |
| min free since boot | 21,988 B | 12,936 B | **-9,052 B** |
| largest free block | 31,732 B | 22,516 B | -9,216 B |

**It is not a leak.** Six zoom ladders and 90 s of idle moved the free heap by
at most 72 bytes and never moved `min_heap` at all. It is not the tile cache
either: both readings were taken over a viewport with no tiles at all.

**It is not static.** `riscv32-esp-elf-size -A` on the two ELFs: `.dram0.data`
+40 B, `.dram0.bss` +56 B. The heap pool itself is the same size either way
(245,220 B vs 245,124 B). So something allocates ~8.5 KB at run time on the
merged build that `develop` does not.

**It is not the GNSS ring.** That was the first guess, because `lib/Gnss`
arrived with this promotion and `platformio.ini` sizes its ring at 8,192 bytes
-- almost exactly the delta. But `GNSS_RX_BUFFER_BYTES` and the whole of
`gnssStart()` sit behind `ENABLE_GNSS_CMD`, which `platformio.ini` defines in
`[env:t5s3pro]` and nowhere else, so a C3 build compiles none of it. The
matching number is a coincidence and a good reminder to check the guard rather
than the arithmetic.

**It is the two chrome snapshots.** `MapActivity` keeps a framebuffer copy of
the bottom chrome band and of the side hint band, taken at the end of every full
render, so that a touch lock or unlock can put the map back with two windowed
refreshes instead of re-reading tiles off the card. Neither existed before the
promotion.

The arithmetic names the byte, which is why no `[MEM]` diff was needed in the
end. `getRegionByteSize()` maps the logical rect through the panel rotation and
widens it to byte boundaries, and an X3 panel is 792x528 physical behind a
528x792 logical screen:

| snapshot | logical rect | bytes/row | rows | bytes |
|---|---|---|---|---|
| `chromeFront_` | `0,752 528x40` (`chromeBandHeight()` = 40) | 5 | 528 | 2,640 |
| `chromeSide_` | `0,155 528x80` (`sideButtonHintsRect()`, X3 branch) | 11 | 528 | 5,808 |
| | | | | **8,448** |

Plus one 4-byte heap block header each: **8,456 B**, the whole measured delta.

The side band is the expensive half because `BaseTheme::sideButtonHintsRect()`
returns the **full screen width** on an X3 -- that board puts one box on the
left edge and one on the right, so the rect spans the dead middle as well.

**Why it is pure waste on a C3.** The swap has exactly one trigger: the touch
lock going on or off, which trades the hint boxes for the padlock. Both halves
of that are `panelPresent()`-gated (`TouchPolicy::locked()`,
`TouchPolicy::lockIndicator()`) and `hintsVisible()` is unconditionally true
without a digitizer. An X3 and an X4 have none, so the chrome the snapshots
protect cannot change, and `swapChrome()` would restore a picture identical to
what is already on the glass.

Fixed by gating both captures on `TouchPolicy::panelPresent()`.
`swapChrome()` already falls back to a full render when there is no snapshot, so
nothing changes on those boards except the heap. The S3 boards have PSRAM and
the touch this guards on, and keep the optimisation.

**Corroborated on hardware 2026-09-13, X3 -- but this is not the controlled
A/B.** The baseline column is the 2026-09-12 reading on the merged build at
`tiles_ok=0`; the gated column is a 2026-09-13 reading at `tiles_ok=4`, on a
card that had gained eighteen tiles in between, and the two builds were never
flashed back to back by the same hand. Read it as "the memory came back", not
as a measurement of how much.

| | merged `develop`, 2026-09-12 | gated, 2026-09-13 | |
|---|---|---|---|
| free heap | 25,188 B | 33,828 B | +8,640 |
| largest free block | 22,516 B | 29,684 B | +7,168 |
| min free since boot | 12,936 B | 20,604 B | +7,668 |

**The controlled evidence is the other two.** The 2026-09-12 pass flashed both
builds back to back in one session and measured the delta at exactly 8,456 B,
and the arithmetic above accounts for 8,448 of it plus one 4-byte block header
each. A cross-session comparison measures everything that changed between the
sessions; the arithmetic measures the thing.

**Why it matters on this board and not the others.** The same promotion left
171,948 B free on an X4 Pro and 173,520 B on a T5 S3 Pro, both read through the
same command in the same state as the X3 numbers above. Both have PSRAM. The C3
does not, and 12.9 KB is now the floor it reaches during boot. Nothing crashed
and nothing failed to render in this pass, but the margin that was there is not
there any more.

(The 187,556 B this paragraph used to quote for the T5 was its `[MEM]` line at
boot on the home screen, not the map state -- a number from a different screen
at a different moment, put next to two that were not.)

**Print the same fields on both sides.** The first pass at this comparison
grepped `heap|tile_fmt` on one build and the full `info` on the other,
concluded from the missing `mtu=` line that no phone was connected to the
baseline, and nearly blamed the whole delta on a BLE connection. A narrowed
grep manufactures a difference. Both readings here are the full `info` and
`stats` output.

## Measured: what the map screen actually costs on a C3, item by item

Taken 2026-09-13 on an X3 with the chrome snapshots gated off. Struct sizes are
read out of the build's own DWARF, not estimated:

```
riscv32-esp-elf-gdb -batch -ex "print sizeof(MapLabelScratch)" firmware.elf
```

| allocation | bytes | share |
|---|---|---|
| `BlePositionServer::begin()` -- the whole NimBLE host + controller | 57,384 | 83 % |
| `MapTileSource` (holds `MapTileReader` 5,356, of which 4,096 is its stream buffer) | 6,992 | 10 % |
| `MapLabelScratch` | 4,096 | 6 % |
| `markerPatch_` (`kMarkerBoxSize` 64) | 720 | 1 % |
| `HalFileSource` | 20 | |
| `MapPointSource` + its file source, only when the points layer is on | 1,348 | |
| `MapRouteSource` + its file source, only when a route was chosen | 1,780 | |

The boot log's own bracket agrees: `MAP heap: 47320 before source alloc, 35064
after, delta 12256` is `MapTileSource` + `MapLabelScratch` + `markerPatch_` +
`HalFileSource` + their block headers.

**NimBLE is 83 % of it, and everything else together is 13 KB.** Any further
work on this board that is not about the radio is competing for the last sixth
of the budget.

## Measured: a coarse rung costs time and card reads, not heap

The worry that rung 6 is memory-hungry does not survive the instrument. Taken
2026-09-13 on an X3 over Prague, the densest data this project has rendered:

| position | tiles | ways | bytes read | free heap | min free |
|---|---|---|---|---|---|
| 50.0640 14.4141 | 12 | 23,898 | 1,682,567 | 32,824 | 20,564 |
| 50.0000 14.3500 | 12 | 23,673 | 1,738,096 | 32,776 | 20,564 |
| 49.9600 14.4800 | 12 | 27,050 | 1,784,445 | 32,824 | 20,564 |

Free heap moved by 48 bytes across the set and `min_heap` did not move at all.
The render's own bracket says why: `MAP heap: 33408 before tile load, 33312
after, delta 96` for a 23,898-way frame. **The renderer streams.** Its whole
working set is the fixed `MapTileReader` buffers, which are allocated at map
entry whatever rung is showing.

What a coarse rung does cost is **time and card**: 7.6 s of render and 1.7 MB
read for one frame, against 3.3 s in the card. That is a panel-latency and a
battery question, not a heap one.

The comparison point on the same card: Bratislava at rung 6 is 12 tiles,
15,258 ways, 968 KB. The two Prague tiles that produced 16,501 ways on their
own came from the **CDN**, and the CDN's z11 build does not carry the
`pedestrian_keep=waymarked_or_named` filter a local build applies -- which
keeps 6.0 % of 158,128 pedestrian ways in this area. So part of that gap is the
rule set and not the city, and the honest claim is narrower: **a z11 tile built
without the pedestrian filter carries roughly half again the ways of one built
with it**, 157 KB against 104 KB for the same ground.

## Measured: the floor is a BLE transfer, not a render

The lowest the C3 gets is not the coarsest rung. From a clean boot, map up, no
phone:

| moment | free | min free since boot |
|---|---|---|
| map up, Prague rung 6, 12 tiles rendered | 33,312 | 33,096 |
| after one 100 KB tile pushed in over BLE | 32,980 | **28,204** |
| a phone connected at `mtu=256`, autosync running | 32,396 | **20,100** |

A single file push costs about 4.9 KB of transient heap, and that one is
isolated: clean boot, one 100 KB tile, nothing else running. **The 20,100 is
not.** It was read with a phone connected and the autosync exchange running,
but no transfer was confirmed in flight at that moment -- the tool's own log
was buffered and showed nothing. Treat it as the lowest thing observed rather
than as the cost of autosync. Settling it needs one push against a `min_heap`
read on either side, the way the 28,204 was taken.

Either way the floor belongs to the radio and the transfer path rather than to
the map, and that is what any future trim has to protect.

**What is on that X3's card, for whoever reads a stale-tile report from it.**
The Prague measurements above needed tiles nobody had. Sixteen z11 tiles (cols
1104-1107, rows 692-695) were built into a scratch directory on 2026-09-13 and
pushed over BLE, and two more (`11/1106/693`, `11/1106/694`) came straight off
the CDN. The scratch build is **not** in `mapbuilder/builds.json`, so nothing
can replay it and those sixteen `content_id`s belong to a build that no longer
exists anywhere. The two CDN ones carry different rules again. A freshness check
against that board will call all eighteen stale the first time the real CDN
serves the same ground, and that is correct rather than a bug.

## What is still unmeasured

- **A real phone sending positions on the trimmed build** (above).
- **The 5,740 bytes `end()` does not return inside its own bracket** (above).
- **An advertising restart after a central disconnects.** Seen twice in these
  captures: after a laptop central dropped, the device stopped advertising and
  `blefakephone.py` reported "no device advertising the map service" until the
  map screen was re-entered. Not a memory issue, and there is already a branch
  for it (`trailink-worktrees/ble-advertising-restart`) — recorded here because
  it is what made the worst-case capture take three attempts.
