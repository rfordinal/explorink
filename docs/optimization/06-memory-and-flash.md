# Plan 06 — memory and flash budget

Measured numbers first, because the project's stated constraint ("380 KB RAM is
the hard ceiling", firmware `CLAUDE.md`) is right about the constraint and the
review found the pressure is not where the framing suggests.

## Measured, this machine, 2026-08-06

Clean `develop` (`1eabf58a`), `pio run`, `env:default`, from
`riscv32-esp-elf-size -A` on `firmware.elf`:

| Section | Bytes | What it is |
|---|---|---|
| `.dram0.data` | 17,561 | initialised static DRAM |
| `.dram0.bss` | 40,216 | zeroed static DRAM |
| **static DRAM total** | **57,777** | ~15% of the 380 KB budget |
| `.iram0.text` | 87,350 | code in IRAM |
| `.flash.text` | 2,094,438 | code in flash |
| `.flash.rodata` | 1,614,148 | constants in flash |
| `firmware.bin` | 3,827,536 | |

App partition is `0x640000` = 6,553,600 bytes (`partitions.csv`), and there are
two of them for OTA. So **flash is 58% used with 2.7 MB spare in the active
slot.**

Largest flash consumers, from `nm --size-sort` (all in DROM, flash-mapped, not
DRAM):

| Bytes | Symbol | Belongs to |
|---|---|---|
| 206,259 | `de_trie_data` | hyphenation, reader only |
| 40,768 | `notoserif_14_bolditalicBitmaps` | reader font |
| 38,212 | `notoserif_14_italicBitmaps` | reader font |
| 36,561 | `ubuntu_12_boldBitmaps` | UI font, in use |
| 33,340 | `ru_trie_data` | hyphenation, reader only |
| 26,943 | `en_trie_data` | hyphenation, reader only |
| 23,591 | `sv_trie_data` | hyphenation, reader only |
| 21,308 | `uk_trie_data` | hyphenation, reader only |

Plus i18n: **281,416 bytes** of deduped strings and offset tables across 31
languages, printed by `gen_i18n.py` at build time.

Largest static DRAM users, from `nm` on `.bss`: `logMessages` 4,096,
`g_cnxMgr` 3,880 (WiFi), `xIsrStack` 2,096, two `BidiUtils` shaping buffers at
1,536 each, `s_wifi_nvs` 1,308. Nothing map-related is in the top twenty.

## What this changes about the framing

**Flash is not a constraint.** 2.7 MB spare. So "strip the reader stack to save
flash" is not an argument worth making, and `OMIT_FONTS` already did the part
that mattered (`platformio.ini`, the comment above it).

**Static DRAM is not a constraint either.** 57.8 KB of 380 KB.

The real pressure is **runtime heap during map + BLE**, which is:

- 48,000 bytes framebuffer, permanently (single-buffer mode, and that flag is
  what makes it one and not two).
- NimBLE host + BT controller, allocated by `begin()` and returned by `end()`
  (`BlePositionServer.cpp:227-259`). **open** — not measured. The `end()`
  comment says the point of the full deinit is returning that RAM, but no number
  is recorded anywhere.
- `sizeof(MapTileSource)` ≈ 5.5 KB, plus 720 bytes of marker patch, both
  allocated in `onEnter()` and both already logged as a heap before/after delta
  (`MapActivity.cpp:420-440`).
- 8,000 bytes of band scratch during a grayscale frame
  (`GrayscaleFrame.h:101-102`), transient.

So the whole budget question is: **what is the free heap on the map screen with
BLE up and a transfer running?** That number is not written down anywhere. It is
the only measurement in this plan that matters.

## Step 1 — record the heap floor

`MapActivity` already logs heap at three points: before/after the source alloc
(`:433`, `:449`) and before/after each tile load (`:1169`, `:1262`). What is
missing is the worst case, which is all of these at once:

- map screen open, BLE up, a central connected,
- a transfer in flight (so NimBLE's buffers are full and SdFat's write path is
  active),
- a viewport reset happening at the same time.

Add `ESP.getMinFreeHeap()` to the reset log line — it is the high-water mark
since boot and costs one call. Then reproduce the case with
`tools/blereplay.py` pushing tiles while a replayed ride drives resets.

Record the number in `docs/PROGRESS.md` and in this file. Everything below is
conditional on it. If the floor is comfortably above 50 KB (the project's own
threshold, firmware `CLAUDE.md` testing checklist), nothing else in this plan is
urgent.

## Step 2 — the reader stack, as a maintenance question

Still linked, all of it: `src/activities/reader/` is 22 files, `lib/Epub/` plus
`lib/expat/`, `lib/miniz/`, `lib/uzlib/`, `lib/Xtc/`, `lib/KOReaderSync/`,
`lib/OpdsParser/`, `lib/Txt/`, `lib/Dictionary`. `EpubReaderActivity.cpp` alone
is 1978 lines — the largest source file in the tree after the generated ones.

The flash argument is gone (step 0's numbers). Three arguments remain, and they
are about the diff and the build, not about bytes:

1. **Merge cost from upstream.** Keeping the reader keeps the fork mergeable
   against CrossPoint, which the fork rule explicitly wants ("keep the diff
   against upstream small enough to keep merging from it", firmware
   `CLAUDE.md`). Deleting it makes every future upstream merge a conflict
   exercise. This argues for **keeping** it.
2. **Build time.** 8m49s from clean on this machine. Most of it is not the
   reader (`Arduino-wolfSSL`, the platform, `expat`), so the saving is real but
   modest. **open** — measure with a scratch branch that drops the reader libs
   from `lib_deps` before deciding.
3. **Reachability.** The home menu no longer offers reading
   (`d0944a32`, "strip reading items and last-book preview from home menu") and
   `HomeMenuItem` has no reader entry (`ActivityManager.h:20`). So the code is
   compiled and unreachable. That is dead weight in the review sense — every
   future reader of this tree has to work out that 22 files do not run.

**Recommendation: do not delete it yet. Mark it.** One line in the firmware
`README.md` doc map and one comment at the top of
`src/activities/reader/ReaderActivity.h` saying the reader path is compiled but
unreachable from the home menu as of 2026-08-05, with the commit. That costs
nothing, keeps the merge base, and stops the next agent from reasoning about
dead code as if it ran.

Revisit if and when an upstream merge stops being wanted. Then it is a deletion
plan of its own, not a line item here.

## Step 3 — trim the i18n table, maybe

281,416 bytes over 31 languages, 468 keys. Flash, so not urgent. But a large
share of those keys are reader strings for screens that cannot be reached
(step 2), and `gen_i18n.py` already strips keys unused in code — it stripped 2
on this build.

The cheap version: the generator could take a language allow-list from
`platformio.ini`, so a personal build carries English plus Slovak instead of 31
languages. That is roughly 260 KB of flash and, more usefully, a smaller
`I18nStrings.cpp` (currently 90,184 lines) to compile every clean build.

**open**: whether this is worth touching. It is inherited, generated, shared
with upstream, and flash is not tight. Low priority; recorded so the number is
known.

## Step 4 — things checked and confirmed fine

- **`MapTileSource` is ~5.5 KB and allocates nothing after construction**
  (`MapTileSource.h:17-24`). Both claims verified by reading: the members are
  `MapTileReader` (4 KB stream buffer at `MapTileReader.h:191`), two 256-entry
  `int16_t` point arrays (`MapTileSource.h:137-138`, 1 KB), a 64-byte name
  buffer and a 160-byte path buffer. Nothing grows with tile count or density.
- **The marker patch is 720 bytes, allocated once, and its size is derived not
  guessed** — the `+16` and `+8` slack is for `readFramebufferRegion`'s
  multiple-of-8 x snapping (`MapActivity.cpp:122-129`, and
  `GfxRenderer.cpp:1568-1584` confirms the snapping).
- **Every fallible allocation is `makeUniqueNoThrow` with a null check and a
  `LOG_ERR`** (`MapActivity.cpp:421-435`). OOM degrades to "follow is off, every
  fix redraws in full", which is the right failure mode.
- **`openMapMenu()` allocates three `std::string`s per press**
  (`MapActivity.cpp:638-642`). It is a button press, not a hot path, and
  `OptionPopup`'s interface takes `std::vector<std::string>`. `reserve(3)` is
  already there. Leave it.
- **No `std::function` anywhere on the map or BLE path.** Hooks are plain
  function pointers with a `ctx` (`BlePositionServer.h:121-128`,
  `MapCommandConsole.h:285`). The one lambda is `OptionPopup`'s callback, which
  is upstream's interface.

## Step 5 — the preview bench gating, checked

`PreviewActivity` is behind `ENABLE_PREVIEW_BENCH` and off by default
(`PreviewActivity.cpp:3`, commit `817bd216`). **Checked, and the gating is
complete**: the home menu's row list, its icon list, both index/enum mapping
helpers and the open handler are all behind the same `#if`
(`HomeActivity.cpp:117-127`, `:48-49`, `:152`; `HomeActivity.h:22-23`, `:36-37`,
`:46`). `HomeMenuItem::PREVIEW` staying in the enum unconditionally
(`ActivityManager.h`) costs nothing — it is one enumerator, never reachable in a
default build.

No change. Recorded so the same question is not re-opened.

## Gate

`riscv32-esp-elf-size -A firmware.elf` before and after any change in this plan,
and `ESP.getMinFreeHeap()` from the worst-case run in step 1. Record both in
`docs/PROGRESS.md`. No claim of a saving without both numbers — firmware
`CLAUDE.md`, "No Unfounded Claims".
