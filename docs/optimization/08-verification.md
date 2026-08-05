# Plan 08 — verification

What is tested, what is not, and which gate each of the other plans needs. This
plan runs alongside the rest; it is not a phase.

## What exists

**Host tests**, GoogleTest via CMake (`test/CMakeLists.txt`), 20 suites. The
map-related ones:

| Suite | Covers |
|---|---|
| `test/map_tile_reader` | `.tib` header, CRC, layer directory, streaming, against real fixtures |
| `test/map_preview` | the whole render pipeline to a PPM, plus a heap probe |
| `test/map_area_fill` | scanline fill and hatch, including the half-open edge rule |
| `test/map_follow` | move-vs-reanchor policy |
| `test/map_stroke` | thick-line stack geometry |
| `test/map_command_parser` | the console grammar |
| `test/map_tile_path` | tile path parse/format |
| `test/missing_tile_priority` | fetch order policy |

That is a good set, and the design that makes it possible is deliberate:
`MapFollow`, `MapAreaFill`, `MapStroke`, `MapCommandParser` and
`MissingTilePriority` were each written with no HAL, no renderer and no Arduino
so they could be tested on the host. `MapCommandConsole` even keeps
`ArduinoJson` and `PersistableStore` out on purpose, with an adapter as the only
meeting point (`MissingTilesConsoleSource.h`).

**Device instrumentation**, all already in the tree:

- `CMD:SCREENSHOT` — 48 KB framebuffer over serial, 100/100 clean via
  `tools/screenshot_gate.py` (firmware `CLAUDE.md`).
- `CMD:SCREENSHOT_GRAY` — BW frame plus both grey planes, decoded by
  `tools/greyshot.py`.
- `CMD:GOTO_MAP` — enter the map screen with no button press.
- The map console over serial and BLE — `tools/mapcmd.py`, `tools/blepos.py`.
- `tools/blereplay.py` — replay a recorded ride's fixes over BLE.
- `tools/blepush.py`, `tools/upload_tiles.py` — push files over the transfer
  channel.
- `MapTileSource::bytesRead()` — real card bytes per reset.
- The map's debug readout — mode, zoom, marker, tiles, ways, milliseconds.
- The device-preview host binary — `MapRenderer` compiled for the laptop, ~2 s
  per style edit (`docs/device-preview.md`, parent repo).

The combination of a screenshot channel, a replay tool and a byte counter is
what makes every gate in plans 01-02 cheap. That is unusual and worth saying.

## The gap that matters

**`MapTransferReceiver` has no test at all.** It is 438 lines, it parses frames
off an untrusted wire, it validates paths, it writes to the card and it renames
files into place. Its failure modes are: a corrupt file accepted as a tile, a
path escaping `/trailink`, a `.part` left behind, a transfer wedged busy.

Every one of those is host-testable. The class already depends on the wire format
and `HalStorage`, nothing else — no BLE types in its own header
(`MapTransferReceiver.h` includes only `HalStorage.h` and `MapTilePath.h`).

What it needs: a `HalStorage` seam. Today it calls `Storage.` directly
(`:182`, `:192`, `:271`, `:275`, `:315`, `:380`). Introduce a narrow interface —
open-for-write, open-for-read, exists, remove, rename, ensure-directory — with
the real `HalStorage` behind it on device and an in-memory map behind it in the
test. That is a small, mechanical extraction and it is the same pattern
`IFileSource` already uses for the tile reader (`IFileSource.h`,
`HalFileSource` on device, `StdioFileSource` in `test/map_preview`). The
precedent is set; follow it.

Cases the test should cover, all currently unverified:

- `relPathIsSafe`: `..` as a whole component rejected, `a..b.tib` allowed
  (`:364-370`), absolute path rejected, trailing slash rejected, empty component
  rejected, non-ASCII rejected, backslash rejected.
- Begin frame: exact-length check (`:132`), zero and over-max total (`:136`),
  refusal when nobody is subscribed (`:153`).
- Chunk: offset mismatch abandons (`:228`), overrun abandons (`:234`).
- Completion: CRC mismatch abandons and leaves nothing behind (`:262`), CRC match
  renames over an existing file (`:271-278`).
- Stale reclaim: a second begin inside `kStaleTransferMs` is refused, one after
  it reclaims (`:158-167`).
- `.part` cleanup on every abandon path.

This is the highest-value new test in the tree, and it is the only item in this
plan that is a real gap rather than a gate.

## Second gap — `TileSyncActivity`'s row-state derivation

Row state is derived from three sources: the receiver's snapshot, the skip
observer, and membership in `MissingTilesStore` (`TileSyncActivity.cpp:193-209`).
That derivation is the screen's whole correctness argument
(`TileSyncActivity.h:41-53`) and it is untested.

It is also not directly testable — the method is on an `Activity` and touches
the renderer. Extract `stateOf()` into a free function over
`(row, transferStatus, stillMissing)` and it becomes pure. Then plan 04's items 2
and 3 arrive with tests instead of arguments.

## Gates per plan

| Plan | Gate | Cost |
|---|---|---|
| 01 render | `CMD:SCREENSHOT` byte-identical, `%lums` lower, `test/map_preview` golden unchanged | zero new tooling |
| 02 tile I/O | `bytesRead()` lower at the same coordinate and rung, `test/map_tile_reader` passes | zero |
| 03 BLE | negotiated MTU and interval in the log; wall-clock for one 30 KB tile before/after | one log line |
| 04 tile sync | new host test on the extracted `stateOf()`; a real sync with a phone that skips one tile and drops the link on another | needs the phone app |
| 05 structure | `CMD:SCREENSHOT` byte-identical on four screens | zero |
| 06 memory | `size -A` before/after, `ESP.getMinFreeHeap()` from the worst-case run | one call |
| 07 power | current draw in four states, on a meter | needs hardware and the user |

Note the pattern: **five of seven gates cost nothing new.** That is a direct
result of the screenshot channel and the byte counter already existing. Use them
rather than adding instrumentation.

## Third gap — nothing runs the host tests automatically

CI has a build check, a format check and two release builds
(firmware `CLAUDE.md`, the CI table). None of them run `test/`.

So 20 host suites exist and nothing enforces them. A refactor that breaks
`test/map_follow` gets a green PR.

Fix: one more workflow — configure CMake, build, `ctest`. The tests fetch
GoogleTest via `FetchContent` (`test/CMakeLists.txt:13-23`), so a runner needs
network on first build; cache the fetch. This is the cheapest correctness
improvement in this whole review.

## Untested behaviour worth naming

Read off the code and the docs, so the list is honest about what is claimed
versus what is checked:

- **Marker follow on hardware.** The README's own status table says it: "exists,
  host-tested, **not measured on hardware**". `MapFollow`'s policy is tested;
  the patch save/restore against a real framebuffer and the ghosting budget are
  not. `tools/blereplay.py` plus `CMD:SCREENSHOT` after each fix would settle
  it.
- **`kMaxPartialMoves = 12`.** Explicitly untuned (`MapFollow.h:41-45`).
- **The transfer path end to end with a real phone.** Verified with a script
  (`tools/blepush.py`), not with an Android app that has its own MTU and
  interval behaviour — which is exactly what plan 03 is about.
- **All four orientations on the map screen.** The firmware checklist asks for
  it. The map has portrait-specific assumptions in its geometry constants
  (compass margin, marker ladder values in `MapViewport.h:68` are absolute
  pixels against an 800-tall screen). Landscape would put the marker ladder off
  the panel. **open** — either the map is portrait-only by design, in which case
  say so in `MapActivity.h`, or the ladder needs deriving from
  `getScreenHeight()`. Worth deciding, cheap either way.

## Do this first

Of everything in this plan, in order:

1. **CI runs `ctest`.** One workflow file. Protects the other 20 suites and every
   plan in this review.
2. **`MapTransferReceiver` gets a storage seam and a test.** The only real
   coverage gap on an untrusted input path.
3. **Extract `stateOf()`** so plan 04's fixes are testable.

Everything else is a gate that already exists.
