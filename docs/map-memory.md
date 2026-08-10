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
`MapActivity` itself is 2,272 bytes (measured, below). The rest — about 65 KB —
is `BlePositionServer::begin()`, which the log shows running immediately before
(`MapActivity.cpp` onEnter, `BLEPOS begin: calling NimBLEDevice::init`).

**open — NimBLE's exact share is not isolated.** What settles it: log
`ESP.getFreeHeap()` immediately before and after `begin()` and `end()` in
`lib/BlePositionServer/src/BlePositionServer.cpp`. Two log lines, one flash. The
`end()` comment already claims returning that RAM is the point of the full
deinit, and no number backs it.

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
`platformio.ini:88`, which rebuilds the core libs and already reclaims ~32–37 KB
by right-sizing timer task stacks and moving WiFi out of IRAM (MEMFIX-PORT).
Adding BLE keys there is the same one-line-per-key edit.

**open — how much this returns.** Nothing here is measured. The gate is the same
as plan 06's: `riscv32-esp-elf-size -A firmware.elf` before and after, plus the
`MEM` line from the map screen with a central connected. No saving may be claimed
without both.

## Levers, largest first

| Lever | Size | Confidence | Cost |
|---|---|---|---|
| Trim NimBLE + controller config to one peripheral connection | ~65 KB is in play; savings unknown | open | core rebuild, one flash, verify with `MEM` |
| Log heap around `begin()`/`end()` to split BLE from the rest | 0 (instrumentation) | — | two lines, one flash. Do this first |
| Right-size `ActivityManagerRender` task stack (8,192, `ActivityManager.cpp:36`) | up to a few KB | open | measure `uxTaskGetStackHighWaterMark` on the map's deepest render |
| `MapTileReader::streamBuffer_` 4,096 -> 2,048 | 2 KB | read | more SD reads per layer; gate on the reset time already logged |
| Attribute the 11.7 KB transient under min free | up to 11.7 KB | open | heap sample inside a transfer and inside a reset |

Order matters: the instrumentation line is first because every other item is
argued against a 65 KB block nobody has measured.

## What is still unmeasured

- **Worst case with a transfer in flight.** Plan 06 wanted map + BLE + a
  transfer + a viewport reset at once. Not captured: the X4 allows one BLE
  connection, and a real phone held it for the whole session, so
  `tools/blefakephone.py` could not attach ("no device advertising the map
  service"). Repeat with the phone's Bluetooth off.
- **NimBLE's own share** (above).
- **The 11.7 KB transient** (above).
