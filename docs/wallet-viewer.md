# The wallet viewer

Papers a rider may have to show somebody -- passport, licence, insurance card,
registration -- kept on the SD card and drawn on the panel with no phone, no
network and no map. Phase P1: **read-only viewing only**. No crypto, no BLE, no
grey, no write path.

Two screens:

- `WalletActivity` -- the list of documents (`src/activities/wallet/WalletActivity.h`).
- `WalletViewActivity` -- one asset, one whole screen (`src/activities/wallet/WalletViewActivity.h`).

Home menu row **Wallet**, between *Sync map tiles* and *Settings*
(`src/activities/home/HomeActivity.cpp:14-21`, `:121-134`), opening through
`ActivityManager::goToWallet()` (`src/activities/ActivityManager.cpp:222`).

## Confirmed on the panel, 2026-08-18

Flashed, assets pushed to the card over BLE, checked from device screenshots:

- the **Wallet** row in the home menu opens the browse screen;
- the **empty state** is honest -- a missing wallet says so instead of showing a
  blank list;
- **browse** lists the item with its page count;
- **FIT** draws the whole page;
- **DETAIL** steps tiles;
- **1:1** opens on the manifest's focal tile, not on the top-left margin.

So the display path, the manifest read, the refresh policy and the button map all
work on real hardware against real generated assets. Everything below that is
marked open is open for a different reason -- design, not plumbing.

## On the card

```
/trailink/wallet/manifest.json          plaintext in P1
/trailink/wallet/<2 hex>/<16 hex>.dat   one asset = one full screen
```

Paths relative to `/trailink`, same root as the map
(`src/activities/map/MapActivity.cpp:54`). The manifest path and the wallet
directory are `src/activities/wallet/WalletStore.h:19` and
`src/activities/wallet/WalletAsset.h:32`.

An asset file is a 32-byte cleartext header, then the payload:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `EWA1` |
| 4 | 1 | assetType 1=FIT 2=DETAIL_TILE 3=ONE_TO_ONE_TILE 4=MACHINE_CODE |
| 5 | 1 | bitDepth (1 today; 2 = 4-level grey is reserved, **not implemented**) |
| 6 | 1 | tileCol |
| 7 | 1 | tileRow |
| 8 | 2 | width, little endian |
| 10 | 2 | height |
| 12 | 4 | rawLen |
| 16 | 4 | version |
| 20 | 1 | flags, bit0 = encrypted (0 in P1) |
| 21 | 1 | presentation, 0 = upright in landscape, 1 = upright in portrait |
| 22 | 2 | reserved |
| 24 | 8 | first 8 bytes of sha256 of the payload |

Parsed by `parseAssetHeader()` (`src/activities/wallet/WalletAsset.h:89-106`).
Read off the format contract, not off a real file -- **no generator exists yet**,
so nothing here has been checked against bytes a tool wrote.

`assetId` is 16 hex characters and the file lives in a directory named by its
first two: `buildAssetPath()`
(`src/activities/wallet/WalletAsset.h:129-148`). The 16-hex check
(`:115-125`) is the reason a manifest cannot name a file outside the wallet
directory -- no `/`, no `.`, no `..` survives it. Tested
(`test/wallet/WalletTest.cpp`, `RejectsAnythingThatIsNotSixteenHex`).

## The display path: no scratch buffer, ever

The payload **is** the framebuffer. The laptop-side generator does every
rotation, scale, dither and pack at build time; the device rotates nothing.

There are two paths into the framebuffer, and neither stages anything. A **tile
asset** is one whole screen, so it is one read:

```
open -> read the 32-byte header -> read rawLen bytes into the framebuffer -> displayBuffer()
```

`WalletStore::loadAssetIntoFrameBuffer()` (`src/activities/wallet/WalletStore.cpp:147-229`),
destination `renderer.getFrameBuffer()` (`lib/GfxRenderer/GfxRenderer.cpp:2072`),
length `renderer.getBufferSize()` (`:2074`), guarded by `hasFrameBuffer()`
(`lib/GfxRenderer/GfxRenderer.h:355`). The same move the sleep frame already
makes (`src/main.cpp:223-236`).

A **page image** is bigger than the screen, so it is 480 reads of one panel row
each, straight into the framebuffer rows -- `wallet::PageReader::readWindow()`
(`WalletStore.cpp`). Measured at 283 ms against the single read's 65 ms; see
"Whole-screen paging was rejected" for the decision that bought.

**No scratch buffer exists on either path, and none may be added.** A screen is
48 KB on the X4, and with BLE up the largest contiguous heap block is about 43 KB
-- measured, `docs/map-memory.md:57`. Staging a screen would fail before it could
be drawn. There is nothing to stage anyway: the bytes are already in panel order,
and in that order a window is a row range at a byte offset.

The byte convention the payload must match, read off the renderer's own pixel
write (`lib/GfxRenderer/GfxRenderer.cpp:517-524`): row-major,
`getDisplayWidthBytes()` bytes per row, MSB first inside a byte, **bit 1 = white,
bit 0 = black ink**. The X4's SSD1677 config keeps that polarity
(`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp:69-71`,
the Sticky comment states the shared "1bpp MSB, bit1=white" for the same
controller and resolution).

### The panel size is not a constant

The reader refuses an asset whose `rawLen`, `width` or `height` is not this
panel's (`WalletStore.cpp:196-213`). It compares against the **live** panel, not
against 48,000:

| device | panel | bytes/row | one screen |
|---|---|---|---|
| X4 / X4 Pro | 800x480 | 100 | 48,000 |
| X3 | 792x528 | 99 | 52,272 |

`freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h:47-54`,
`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:685-690` (X4).

### The manifest names the panel

One asset set per wallet, and the manifest declares which panel the set was built
for. **Decided 2026-08-18**; the alternative (per-panel asset sets in one wallet)
was rejected -- it triples a card's size for a device the rider does not own.

```json
"panel": {"name":"x4","width":800,"height":480,"rowBytes":100,"assetBytes":48000}
```

Parsed into `DeclaredPanel` (`src/activities/wallet/WalletAsset.h:188-195`) and
checked by `panelMatches()` (`:199-206`). The browse screen checks it **first**,
before it lists anything (`WalletStore.cpp:118-130`): a set built for another
panel cannot be drawn at all, so half-opening it would only waste a refresh. On a
mismatch the list is empty and the status line names both geometries --
`STR_WALLET_PANEL_MISMATCH`, "Built for x4 (800x480), this device is 792x528"
(`WalletActivity.cpp:renderScreen`). "Wrong screen" without saying which is a
dead end for whoever has to fix the card.

A manifest with **no** `panel` object is a tree generated before the field
existed. Those are treated as the live panel's geometry, and the per-asset header
check does the real work -- every asset states its own `width`, `height` and
`rawLen`, and one that disagrees is refused with "built for another screen". So an
old tree on the wrong device fails per asset instead of up front, which is worse
messaging and equally safe. A field the manifest states as 0 is likewise not
compared: it declared nothing about it.

`--panel x4|x3` on the generator side; X4 = 800x480 / 100 / 48,000, X3 =
792x528 / 99 / 52,272.

## The manifest is streamed, never held

`WalletManifestParser` (`src/activities/wallet/WalletManifestParser.h`) runs the
manifest through `StreamingJsonParser` (`lib/JsonParser/StreamingJsonParser.h`)
in 256-byte bites (`WalletStore.cpp:18`, `:33-40`), the same way
`ReleaseJsonParser` reads a GitHub release. Nothing is buffered but the answer
asked for, so a manifest with a hundred pages costs the same RAM as one with one
page.

Two questions, one parser:

- `beginList()` -- title and page count per item. The browse list.
- `beginLookup(item, page, level, col, row)` -- the grids of all three levels
  **and** the assetId of one tile, in one pass.

The viewer **re-runs a lookup on every screen** instead of caching a table of
assetIds (`WalletViewActivity.cpp:showCurrent`). Reasoning: a manifest read is a
few KB off the card; a FAST refresh is 500 ms of waveform
(`docs/refresh-modes.md`). Re-reading costs nothing measurable next to that, and
it caps the viewer's resident RAM at a few hundred bytes with no ceiling on how
many tiles a level may have.

Item identity is **the item's position in the `items` array**. `id` and
`sortOrder` are parsed by nobody: two indices into the same file are cheaper than
a string compare, and P1 has no sync that could reorder the array under a running
screen.

## The button map

The device has six buttons: four front (logically Back / Confirm / Left / Right,
user-remappable) and two side (logically Up / Down, fixed) --
`src/MappedInputManager.cpp:51-108`. The viewer needs seven roles. One pair
therefore does two jobs.

| button | in the viewer |
|---|---|
| CONFIRM (front) | cycle level: FIT -> DETAIL -> 1:1 -> FIT |
| LEFT / RIGHT (front) | move the view across the document, clamped |
| UP / DOWN (side) | move the view down/up the document, clamped |
| UP / DOWN (side), where the view cannot travel down at all | previous / next page |
| BACK (front) | back to the browse list |

The four arrows always mean the same four directions **of the document**. How far
one press moves is the source's business, not the button's -- a fraction of the
view for a page image, one whole tile for a tile grid.

Why the side pair carries pages:

- The repo's existing convention already puts page turning on the side buttons --
  that is what `Button::PageBack` / `PageForward` are
  (`src/MappedInputManager.cpp:77-98`).
- Where the view cannot travel down the document -- always true at FIT, which is
  exactly one screen tall -- the vertical arrows have nothing to move. Overloading
  a pair that is idle beats overloading one that is not.
- FIT is where a reader flips pages anyway. DETAIL and 1:1 are for inspecting one
  region of the page in front of you; flipping to another page from inside a
  zoomed corner is not a move anybody makes.

`Button::Up`/`Down` are used directly rather than `PageBack`/`PageForward`, so
the reader's side-button swap setting does **not** apply here. Deliberate: the
same two buttons also move the view vertically, where up must mean up.

**This map is a signed-off decision, 2026-08-18, not a placeholder.** Two
alternatives were on the table and both lost:

- **Touch swipe for pages.** The device this is for is on a motorcycle or a
  trail. A swipe is useless in gloves, and the wallet is a screen a rider opens
  precisely when they are standing at a roadside being asked for a document.
- **Hold-to-page on the side buttons.** Slower than a press for the common case,
  and it has to be taught -- a held button that does something different from the
  same button pressed is a thing a rider learns from a manual, not from the
  screen. The level-dependent split needs no teaching: at FIT there is no row to
  step, so nothing is taken away.

Arrows clamp at the edges and never wrap (`WalletViewActivity::stepView()`) --
what a sheet of paper does. A page change resets to FIT (`stepPage`).

**How far one press moves depends on the source**, and only that:

- a **page image** pans by the manifest's `windowStepX` / `windowStepY`, a fraction
  of the view. This is what the maintainer asked for and what design B delivers;
- a **tile grid** steps one whole tile, which is one whole screen. That is the
  interaction he rejected, kept only because a card in the field may hold nothing
  else.

A level change does **not** carry the position across: the levels are different
sizes, so there is no honest mapping between their coordinates. It opens at the
level's own focal position -- the `pageImage`'s `focalX`/`focalY`, or the tile
grid's `defaultTileX`/`defaultTileY` (`WalletViewActivity::jumpToLevelDefault()`).
The generator's rule for the tile case is the centre biased top-left, so a 4x4 1:1
grid points at 1,1. Verified on real output both ways: tile 1,1 of the demo page's
1:1 level opens on body text at full size where 0,0 would have opened on the
top-left margin, and the 1:1 page image's focal point 800,859 does the same.

Browse: UP/DOWN (also LEFT/RIGHT) move the selection with hard stops, CONFIRM
opens, BACK goes home (`WalletActivity.cpp:loop`).

## Whole-screen paging was rejected, and design B replaced it

**The maintainer used it on the panel and turned it down, 2026-08-18.** The viewer
worked; the *interaction* was wrong. He wants to look at an arbitrary **part** of a
document, so a pan must move by a fraction of the view, not a whole screen per
press. One press one screen is fine for turning pages and useless for finding the
line with the policy number on it.

That killed pre-cut non-overlapping tiles as the only mechanism -- a
non-overlapping grid can only ever land on multiples of a whole screen. Two
candidates:

- **B: one whole-page image per zoom level**, panel-native order, an arbitrary
  window blitted out of it. Smaller on the card than an overlapping grid, and in
  native order an 8-aligned window needs no bit rotation at all. The cost is that a
  window becomes 480 strided reads of one panel row instead of one 48,000-byte
  sequential read.
- **A: overlapping tiles at 50 %** (fallback). Keeps the single-read blit exactly
  as it was, at 49 tiles per A4 1:1 page instead of 16.

The decision turned entirely on what those 480 strided reads cost, and nobody knew
-- an estimate off the sequential rate spanned 0.25 s to 0.9 s, which is the
difference between "free next to the waveform" and "twice the refresh". So a gate
was pre-registered at **400 ms**, `CMD:WALLETBENCH` was written to measure it, and
the measurement was taken before either design was written down.

### Measured on the X4, 2026-08-18

`CMD:WALLETBENCH`, build `648a9f0f`. One A4 1:1 page image, 2576x1819 native,
322 bytes a row; a 480-row window; 3 iterations; no decrypt and no panel refresh
inside the timed section; the file opened once, outside the timing.

| mode | ms per frame | bytes read | effective rate |
|---|---|---|---|
| `windowed` -- 480 strided reads of 100 B | **282.8 / 285.5 / 287.1** | 48,000 | ~168 kB/s |
| `sequential` -- one 48,000 B read | 65.5 | 48,000 | 732 kB/s |
| `oversized` -- 480 reads of 512 B | 613.3 | 245,760 | 400 kB/s |

The three `windowed` figures are three window origins -- top-left, focal,
bottom-right clamped. **Offset does not matter**: 1.5 % spread across the whole
image. So a window costs what a window costs, wherever it is.

**Decision: design B, against the 400 ms gate.** A pan is about **850 ms**,
against about **633 ms** for a whole-screen tile step: 285 ms or 65 ms of card
time, plus a 500 ms FAST waveform, plus the controller RAM write that the
waveform figure excludes (~68 ms, derived, [`refresh-modes.md`](refresh-modes.md)).

An earlier version of this line said 785 ms and 565 ms. Both were low by the same
~68 ms, because they added card time to the waveform alone. **The ~220 ms gap
between the two designs is unaffected** -- it is card time only, and card time is
the only thing the two designs differ in. Rule that follows: `refresh-modes.md`'s
millisecond figures are *waveform*, not *frame*. Adding them to anything means
adding the RAM write too.

A pan costs ~35 % more than a page-turn did and buys the interaction the device is
for. Design A is not implemented and should not be
unless something later disqualifies B.

### Conditions, and what three iterations is worth

X4, one card, one file kept open across all iterations, **3 iterations per
window** (spread under 1.5 %), no decrypt, no refresh inside the timed section,
one document, and a **fixed mode order** -- `windowed`, `sequential`,
`oversized`, `stream`. SdFat holds a single 512-byte block cache, so the first
mode measured is the one that pays for a cold cache, and the order is not neutral.

**Open: rerun with the order reversed.** Until that exists, the ranking is
trustworthy and the absolute figures carry an unquantified first-mover penalty on
`windowed`, the mode the decision rests on -- which biases *against* the design
that was chosen, so the decision is safe either way.

Three iterations is thin on purpose. It is enough to separate 285 ms from 613 ms.
It is not enough to claim 285 over 290, and no conclusion here depends on that
difference.

### Two beliefs the numbers corrected

- **Sequential SD read is 732 kB/s here, not 550-608 kB/s.** That older figure is
  real and stands for what it measured (`docs/optimization/01-render-pipeline.md:175-187`
  -- the map's tile reads, which reopen a file per tile). This one is a single open
  file read straight through. **Different access pattern, different number. Cite
  whichever matches the pattern; do not average them.**
- **`oversized` was slower, and backward seeks are the likely reason -- read, not
  measured.** The mode was written to ask whether the card's block size already
  dominates a 100-byte read. It answered something else: 613 ms against 283. The
  explanation on offer is that a 512-byte read over a 322-byte stride must
  overshoot the row, so every read is followed by a seek *backwards* -- but
  **nothing in the bench observed a seek**, and the arithmetic is where that
  reading comes from, not the instrument. What would settle it: rerun mode 3
  against a page image whose stride is >= 512 (a wider level, or a synthetic
  file), where no overshoot is possible. If it stays slow there, the cause is
  something else. Either way the mode answered a different question than it was
  asked, which is the argument for keeping a mode whose result you cannot
  predict.

### The pan step, and what a level opens at

Both come out of the manifest, per level, so the device invents neither:
`windowStepX` / `windowStepY` for the step and `focalX` / `focalY` for where a
level opens. Clamped with `max(0, min(v, span - window))`
(`clampWindowOrigin()`, `src/activities/wallet/WalletAsset.h:239-244`) -- never
wrapped, exactly like the tile arrows.

The x limit is derived in **bytes** and multiplied back by eight
(`maxWindowX()`, `:251-254`), so every reachable origin on that axis is 8-aligned
by construction even for an image whose `nativeWidth` is not a multiple of 8. The
generator guarantees the step and the focal point are 8-aligned too; an unaligned
one is **refused and logged, not rounded** -- rounding would hide a generator bug
behind a half-pixel shift.

### The axis note, and the bug it caught

A page image is the document stored turned a quarter, because that is what the
panel wants. `GfxRenderer::rotateCoordinates()` maps logical portrait `(x, y)` to
panel `(y, panelHeight - 1 - x)` (`lib/GfxRenderer/GfxRenderer.cpp:216-223`), so
inside a stored page image:

- **native x runs DOWN the document**, 0 at the top. It is also the byte-offset
  axis -- a row read starts at `x / 8` -- so this is the axis that must stay
  8-aligned.
- **native y runs ACROSS the document, inverted.** The *largest* native y is the
  document's **left** edge, so panning left means increasing native y.

The first wiring of this had LEFT/RIGHT on native x and UP/DOWN on native y, which
rotates the whole pan by ninety degrees. The host preview caught it before the code
ever reached the panel: rendering native `(0, max)` of a real generated 1:1 page
image shows the **top-left of the page**, title and left margin, which is only
consistent with the mapping above. Pinned by
`WalletWindow.NativeXRunsDownThePageAndNativeYRunsAcrossItInverted` and stated in
full above `WalletViewActivity::stepView()`.

### Two sources per level, and the tile path is not deleted

A level offers a `pageImage`, a tile grid, or both. **`pageImage` wins when it is
there**; the tile grid is the fallback, and it stays because the generator still
emits both and a card in the field may hold either
(`WalletViewActivity::showCurrent()`). `Store::lookupPage()` therefore succeeds when
*either* source is present -- a design-B level may carry an empty `assets` array.

```json
"one_to_one": {"cols":4,"rows":4,"assets":[...],
  "pageImage": {"assetId":"...","nativeWidth":2576,"nativeHeight":1819,
                "rowBytes":322,"rawLen":585718,"sha256":"...",
                "windowStepX":400,"windowStepY":240,"focalX":800,"focalY":859}}
```

`assetType 5 = PAGE_IMAGE`. The file is opened **once and kept open across
presses** (`wallet::PageReader`, `WalletStore.h`): that is what the 282.8 ms was
measured with, and reopening per frame is unmeasured cost. A pan re-seeks; it does
not reopen.

**A page image is not checked against the panel.** It is not panel-shaped by
design: the 1:1 page image is byte-identical on an X4 and an X3, because there is no
grid in it and so nothing about it is cut to a screen. `checkPageImage()`
(`WalletAsset.h:281-295`) validates it against its own stated geometry --
`rowBytes * nativeHeight == rawLen`, and the header agreeing with the manifest --
plus the window fitting inside it (`rowBytes >= panel rowBytes`,
`nativeHeight >= panel height`). A page image *smaller* than the panel is refused as
`WrongPanel`, because no window out of it can fill the screen. Tile assets keep the
existing panel check unchanged, `checkAssetForPanel()`; the two gates share their
head, `checkAssetCommon()`, so magic, encryption and bit depth cannot drift apart.

The **button map survived the redesign** -- same six buttons, same roles, same
overloaded pair. Only the step size changed, and the idle test moved from "does the
grid have more than one row" to "can the view travel down the document at all". At
FIT the page image is exactly one panel, so both tests give the same answer, which
is why nothing a rider does at FIT changed.

### CMD:WALLETBENCH

`src/main.cpp`, in the same dispatch as `CMD:GOTO_MAP` and `CMD:SCREENSHOT`
(which all drop power saving first -- at 10 MHz every timing on this device is a
lie, `docs/power-management.md`).

```
CMD:WALLETBENCH <path-under-/trailink> <stride> <x> <y> <iters>
```

`stride` is the source image's bytes per row, not the panel's. `x` must be a
multiple of 8: an 8-aligned window is the whole reason design B needs no bit
rotation, so an unaligned request is refused rather than silently measured. Four
modes, same file, same window, `iters` times each (1..32), reported as min /
median / max per frame plus the implied kB/s over payload bytes:

| mode | what it does | what it told us |
|---|---|---|
| `windowed` | 480 seeks + 480 reads of `rowBytes`, straight into the framebuffer | design B's real per-frame cost. **283 ms** |
| `sequential` | one read of the whole framebuffer from the payload start | the baseline. **65.5 ms**, 732 kB/s |
| `oversized` | the same 480 rows, pulling 512 B each and keeping `rowBytes` | asked about block size, answered about seek direction. **613 ms** |
| `stream` | the window's whole row range read sequentially, no seeks at all: `rows * stride` bytes through the same 512-byte chunk buffer, lifting `rowBytes` out of each row on the way past | the candidate optimisation. 154,560 bytes instead of 48,000, but at the sequential rate. Estimated ~211 ms against the measured 283. **Written, never run** |

Everything goes through `HalStorage`/`HalFile`, never raw SdFat, per this repo's
threading rule. A `RenderLock` is held across every iteration of every mode: the
destination is the framebuffer, which the render task also owns, and nothing may
repaint in the middle of a measurement. The summary prints after the lock is
released, so CDC writes are outside both the lock and the timed section.

Two static buffers, 1,024 bytes of `.bss` and no heap: the 512-byte chunk buffer
that modes 3 and 4 share (over the 256-byte guidance for a stack buffer in this
tree, so it is not on the stack) and a 4 x 32 sample array that has to outlive the
lock scope.

**What it does not measure**, and must not be read as measuring: any decrypt (P1
has none), any panel refresh (deliberately outside the timed section -- a FAST
waveform is 500 ms and would swamp the card completely), the cost of opening the
file (opened once, outside the timing, which is what design B does too), more than
one file, more than one card, and any order other than the fixed mode order. SdFat
keeps a single 512-byte block cache, so a rerun with the modes reversed is the
sanity check on that last point.

Host-side friction worth knowing, from the run: a raw pyserial reader needs
`dtr=True` or the C3's CDC never sees the write at all, and `mapcmd.py` eats the
first lines of the reply.

## Refresh policy

Per `docs/refresh-modes.md`. `FULL` is never used anywhere here.

| event | mode | why |
|---|---|---|
| entering the browse list | `HALF` | first paint over another screen |
| moving the list selection | `FAST` | a small differential change |
| entering an item | `HALF` | the list is on the panel |
| tile step | `FAST` | same document, a shifted window |
| level change | `HALF` | a different picture, not a shifted one |
| page change | `HALF` | a new document surface |
| back to the list | `HALF` | clearing a full-page document |
| any failure screen | `HALF` | replacing a full-page image with text |

No grayscale path exists here and none should be added: grey costs a second
waveform pass (`docs/eink-grayscale.md`).

## No overlay

The asset fills the panel and nothing is drawn on top of it -- no level badge, no
page counter, no button hints. Every pixel belongs to a paper somebody may have
to read. The cost is discoverability: the button map is only in this document and
in the user guide, not on the screen. **Open** -- worth revisiting after the
first real use, and worth measuring rather than guessing.

## Failure states

Every one of them draws a legible screen and leaves every button working, so the
rider can navigate away. Nothing here can crash a screen.
Strings: `lib/I18n/translations/english.yaml`, `STR_WALLET_*`. Mapping:
`wallet::errorText()` (`src/activities/wallet/WalletStore.cpp:62-89`).

| state | shown |
|---|---|
| manifest declares another panel | "Built for x4 (800x480), this device is 792x528" -- both, always |
| no `manifest.json` | "No wallet on the card" |
| manifest is not JSON, or carries no `formatVersion` | "Wallet list is unreadable" |
| `formatVersion` is not 1 | "Wallet needs newer firmware" |
| manifest parses, holds no items | "No documents in the wallet" |
| more items than the list can hold (24) | "Showing 4 of 30 documents" -- never a silent cut |
| assetId in the manifest, no file on the card | "This page is not on the card" |
| bad magic, short header, bitDepth != 1 | "This page file is damaged" |
| payload ends early | "This page file is damaged" |
| `flags` bit 0 set | "This page is encrypted" |
| `rawLen` / `width` / `height` is not this panel | "This page was built for another screen" |
| framebuffer lent out | "Screen buffer is busy" |
| item, page or tile not in the manifest | "This page is not on the card" |

A failed lookup also zeroes the level grids, so the arrows have nowhere to step
rather than asking for tiles that may not exist either
(`WalletViewActivity.cpp:showCurrent`).

## Read against real generator output

The format was implemented **twice from one written contract** -- once in the
generator (`tools/walletgen.py`, parent repo) and once here -- and for a while the
two had never met. A unit test on hand-authored bytes cannot close that: it agrees
with whichever side wrote it.

`test/wallet_preview/wallet_preview.cpp` closes it. It reads a generated wallet
tree through the firmware's **own** code -- `ManifestParser`, `buildAssetPathIn()`,
`parseAssetHeader()`, `checkAssetForPanel()` -- and expands the payload to a PNG
exactly as the panel does: row-major, `rowBytes` per physical row, MSB first,
bit 1 = white. It writes two images per asset:

- `PREFIX.png` -- the panel's own 800x480 landscape frame, what the framebuffer
  literally holds.
- `PREFIX-portrait.png` -- the same bits through
  `logical (x,y) -> physical (y, panelHeight - 1 - x)`, which is
  `GfxRenderer::rotateCoordinates()` for `Portrait`
  (`lib/GfxRenderer/GfxRenderer.cpp:216-223`). This is what a rider sees.

It also renders a **design-B window**: when the level carries a `pageImage`, the
window at `--win-x` / `--win-y` is blitted out of it row by row -- the same
arithmetic `PageReader::readWindow()` runs on the device, with a `memcpy` where the
device has a seek and a read. Origins default to the manifest's focal point and are
clamped exactly as the device clamps them. **This is what caught the rotated pan
axis** (see "The axis note").

Run it: `pio run -t wallet-preview` renders the committed fixture, or point the
binary at a whole tree:

```
build/test/wallet_preview/wallet_preview --tree DIR --level detail --col 1 --row 0 \
    --out /tmp/tile
build/test/wallet_preview/wallet_preview --tree DIR --level one_to_one \
    --win-x 0 --win-y 99999 --out /tmp/corner
```

A tree with only sidecars is read through them (`test/wallet_preview/WalletSidecar.h`
decodes EWRL on the host; the firmware has no EWRL reader in P1).

### What the pictures showed, 2026-08-18

Generated with `walletgen.py --demo --paper a4`, rendered through the reader,
**looked at**:

| asset | verdict |
|---|---|
| FIT, portrait view | **upright**, correct polarity -- black text on white, title at the top, reads left to right |
| FIT, panel view | rotated 90 degrees, as it must be: the panel holds the page sideways and the portrait read un-rotates it |
| DETAIL 0,0 | top-left quarter -- title and the first paragraphs |
| DETAIL 1,0 | the right-hand continuation of the same lines. **col increases to the right** |
| DETAIL 0,1 | the lower half -- table, swatches, footer. **row increases downward** |
| DETAIL 1,1 | bottom-right quarter |
| 1:1 tile 1,1 | body text at full size, legible |

So: the generator's build-time rotation is the exact inverse of the firmware's
portrait transform, the ink polarity matches, and the manifest's `col`/`row` mean
what the arrows assume. Ink coverage 6.87 % on the FIT page -- an inverted
polarity would read about 93 %.

### And again for design B, same day

`walletgen.py --demo --paper a4 --panel x4 --page-image` writes a tree with a
`panel` object and a `pageImage` per level. Read through the same reader code:

| what | verdict |
|---|---|
| the declared `panel` | `x4 800x480, 100 B/row, 48000 B/asset -- matches` |
| the 1:1 page image | header type 5, 2576x1819, `rawLen` 585,718 -- **accepted by `checkPageImage()` and refused by `checkAssetForPanel()`**, which is the whole point of the second gate |
| x travel limit | 1776, exactly `(322 - 100) * 8` |
| window at the focal point 800,859 | body text at full size, upright, legible |
| window at 400,859 -- one step back | overlaps the focal window by half a view, and the overlap is the same text. **The pan is a fraction of the view** |
| window at 0,max | the **top-left of the page**: left margin, title starting. This is the shot that settled the axis mapping |
| window at max,max | the bottom-left, clamped on both axes |

Every field name in the manifest matched what this reader parses, first try:
`assetId`, `nativeWidth`, `nativeHeight`, `rowBytes`, `rawLen`, `windowStepX`,
`windowStepY`, `focalX`, `focalY`, and `panel.{name,width,height,rowBytes,assetBytes}`.
Two implementations of one contract, written apart, agreeing. The generator also
emits `rleLen` and `sha256`, which this reader ignores.

The page-image tree is **not committed**: the 1:1 asset alone is 585 KB and its
sidecar 175 KB. Regenerate it with the line above when it is needed.

### The test that keeps it that way

`test/wallet/fixtures/` holds **verbatim generator output**: the demo
`manifest.json` and the FIT asset's `.rle` sidecar (17 KB, versus 48 KB for the
`.dat`; the sidecar carries the 32-byte header verbatim as a prefix, so the `.dat`
can be rebuilt from it byte for byte). `WalletGeneratedTree.*` asserts against it:
the item and grids come out of the real manifest, the real assetId maps to the
file the generator wrote, the header parses field for field (including
`presentation = 1`), and the decoded pixels are checked five ways:

1. **Polarity** -- ink between 1 % and 20 % of the panel.
2. **Margins** -- the top and bottom 10 % of the logical page carry exactly zero ink.
3. **Which way up** -- the title band outinks the footer band five to one.
4. **Which way round** -- the left column band outinks the right margin five to one.
5. **Bit order inside a byte** -- the transition rate across a byte boundary,
   against the rate inside bytes.

Check 5 exists because checks 1-4 **do not catch** an intra-byte bit reversal:
reversing every byte keeps the margins white and the bands dense, it only
scrambles pixels within each group of eight. Correlation catches it -- adjacent
pixels agree far more often than pixels seven apart, so a reversal pushes the
byte-boundary transition rate up. Measured on this fixture: **0.887 correct,
1.254 reversed**, threshold 1.05.

All five checks were **proved to fail** on deliberately broken payloads before
being committed -- inverted bits, a 180-degree rotation, and a payload packed in
logical portrait order instead of panel order. A check that cannot fail is worth
nothing.

## Where the two implementations did not line up

Found by reading the generator and rendering its output. None of these is a bug in
either side; they are places the written contract and the code disagreed.

- **`type` in the manifest is a number, not a string.** The contract sketch showed
  `"type":"FIT"`; the generator writes `"type":1`. Harmless here -- the reader
  never looks at the manifest's `type`, it trusts the asset header's `assetType` --
  but a later phase that does read it must expect an integer.
- **`defaultTileX` / `defaultTileY` exist and the contract did not mention them.**
  Now honoured: a level opens at its focal tile (above). Design B's `focalX` /
  `focalY` are the same idea as a point rather than a tile.
- **`presentation` defaults to 1 (portrait), not 0.** The generator rotates at
  build time so the document stands upright on a device held in portrait -- the
  same orientation the rest of the UI uses. The earlier assumption here was that
  the rider turns the device sideways. They do not. Nothing in the code acted on
  the field either way, but the doc said the wrong thing.
- **48,000 is the X4 only.** The X3's 792x528 panel needs 52,272 bytes at 99 bytes
  a row. Settled by the `panel` object above.
- **The generator's `DEVICE_PPI = 217` is its own unverified assumption**
  (`walletgen.py`, the comment says so). Every 1:1 grid size derives from it, so
  the 4x4 grid for A4 is a guess about the panel's physical size, not a
  measurement. Not this side's problem to fix, but the 1:1 level is only "actual
  size" if that number is right.

## The menu icon

Lucide `wallet`, at 32 px, through the SDK's own pipeline
(`freeink-sdk/libs/assets/Icons/tools/gen_icons.py`) -- the repo rule for any
static icon (`CLAUDE.md`, "Icons: prefer Lucide"). It lives in
`src/components/icons/wallet.h` as `WalletIcon`, is mapped from `UIIcon::Wallet`
in `src/components/themes/lyra/LyraTheme.cpp` (`iconForName`, the 32 px table) and
is picked by `HomeActivity`. Cost: **128 bytes of rodata** -- exactly the 32x32
bitmap -- plus 16 bytes of switch, and no RAM.

Two traps found on the way, both worth knowing before the next icon:

- **`UIIcon::File` has no 32 px entry.** The wallet row was on `File`, which only
  exists in `iconForName`'s 24 px table, so `drawIcon` was never called and the row
  drew no icon at all while every other row drew one. Only the Lyra theme draws
  main-menu icons; `BaseTheme::drawButtonMenu` ignores its `rowIcon` argument
  entirely (`BaseTheme.cpp:745-772`).
- **`gen_icons.py` output is not in the layout `drawIcon()` wants.**
  `GfxRenderer::drawIcon()` plots source `(row, col)` at screen
  `(x + size - 1 - row, y + col)` -- a quarter turn (`GfxRenderer.cpp:1231-1249`).
  Every hand-drawn icon in `src/components/icons/` is stored pre-turned to cancel
  it; the generator explicitly does not pre-rotate. So the generated bytes were
  turned here before committing: `src[row][col] = upright[col][31 - row]`. Checked
  by rendering both layouts as ASCII before and after -- without the turn the
  wallet lands on the panel lying on its side.

  **This means `src/components/icons/search24.h` is drawn rotated a quarter turn
  today** (OPDS browser, `OpdsBookBrowserActivity.cpp:203`). It is generator output
  consumed straight by `drawIcon`, and nobody noticed because a magnifying glass
  rotated 90 degrees still reads as a magnifying glass -- its handle points the
  wrong way. Not fixed here: it is a different screen and wants its own look on the
  panel. **Open.**

One more thing about reproducing this file: `rsvg-convert` is not installed on the
machine it was generated on, so the raster came from `cairosvg` through a PATH
stand-in taking the same arguments. A regeneration with real librsvg may differ by
an antialiased pixel.

## Open: the browse header says `1 documents`

Seen on the panel 2026-08-18. The count is not pluralised, so a single-item wallet
reads `1 documents`. i18n needs a real plural form for the count, not a suffix
glued on -- Slovak alone has three, and the string table has no plural mechanism
today.

## Read-only by construction

There is no write, rename, delete, move or truncate call anywhere under
`src/activities/wallet/`. Not "no UI for it" -- no code path at all. The card is
often the only offline copy of a rider's documents, and a device that can destroy
it is worse than one that cannot show it.

## What is deliberately absent in P1

- **Encryption.** `flags` bit 0 is parsed and refused, never decrypted.
- **sha256 verification.** `sha256_prefix` is parsed and carried
  (`AssetHeader::sha256Prefix`) and checked by nobody. It lands in the phase that
  adds encryption, where the payload is authenticated anyway -- hashing 48 KB
  here would need a second read of bytes the framebuffer has already swallowed.
- **BLE.** Nothing in the wallet opens a radio. Assets arrive by whatever put
  them on the card.
- **Grey.** `bitDepth` 2 is refused.
- **Machine codes.** `assetType` 4 and the manifest's `codes` array are parsed
  past, not rendered.
- **Any write path.** See above.
- **A generator.** Nothing on the laptop writes these files yet. Until one does,
  the viewer has never been fed a real asset.

## Cost

Measured against the branch point `51bfbbc0`, `pio run -e default`,
`riscv32-esp-elf-size -A`:

| | baseline | with the wallet | delta |
|---|---|---|---|
| `firmware.bin` | 3,939,600 | 3,956,480 | **+16,880** |
| `.flash.text` | 2,171,060 | 2,183,814 | +12,754 |
| `.flash.rodata` | 1,649,008 | 1,653,128 | +4,120 |
| `.dram0.data` | 18,145 | 18,145 | **0** |
| `.dram0.bss` | 40,152 | 41,176 | +1,024 |
| `.iram0.text` | 87,350 | 87,350 | 0 |

The rodata is mostly the 21 new `STR_WALLET_*` strings across 31 language arrays,
plus 128 bytes of menu icon. The `.bss` is `CMD:WALLETBENCH`'s two static buffers
and nothing else -- the viewer itself has no globals. Measured separately, in the
same way: the icon is **+144 bytes of binary** (128 rodata + 16 text) and no RAM.

Heap, **derived from the type sizes, not measured on hardware**:

- `WalletActivity` while up: 24 `ItemEntry` rows heap-allocated in `onEnter()` and
  freed in `onExit()` -- 50 bytes each (a 48-byte title, a `uint16_t` page
  count), so **1,200 bytes**, plus the
  activity object itself (~100 bytes). Held while the viewer is open too, because
  the viewer is pushed on top rather than replacing it.
- `WalletViewActivity` while up: **~230 bytes** -- a title, a page index, three
  `LevelGrid` entries, the window origin, and a `PageReader` holding one open
  `HalFile` and the level's `PageImageSpec`. The open file is the point: a pan
  re-seeks, it never reopens.
- Per screen and per lookup, transient: one `ManifestParser` on the heap,
  **~750 bytes** (`StreamingJsonParser`'s 512-byte token buffer, a 32-entry
  context stack, a key buffer), freed before the function returns. Heap, not
  stack, deliberately -- the main task's stack has no business carrying 512 bytes
  of JSON token.
- Per screen: **0 bytes** for the image itself.

## Status

- Format layout, path mapping, level cycle, manifest shape, the panel gate:
  **host-tested**, `test/wallet/WalletTest.cpp`, 30 cases, `pio run -t unit-tests`.
- Byte order, ink polarity, tile order, orientation: **checked against real
  generator output** and looked at as pictures, 2026-08-18 -- see above. This is
  the strongest evidence that exists short of hardware, and it covers exactly the
  things a unit test on hand-written bytes could not.
- Byte order, bit polarity, framebuffer accessors, refresh modes, button
  mapping: **read off the code**, cited above.
- Flash and static RAM: **measured** on the builds above, and the icon and the
  bench command measured separately by building with each reverted.
- Menu entry, empty state, browse, FIT, DETAIL and 1:1's focal tile: **confirmed on
  the panel**, 2026-08-18, from device screenshots with assets pushed over BLE.
- Heap: **derived** from type sizes. Nobody has run `heap_caps_get_info()` with a
  wallet on screen.
- The card figures behind design B: **measured on the X4**, 2026-08-18, three
  origins, three iterations -- table above. The 400 ms gate was pre-registered
  before the measurement, which is why the decision is a decision and not a
  preference.
- Design B's row-blit arithmetic, the clamp, the 8-alignment and the axis mapping:
  **host-tested** and **rendered as pictures** off real generator output. Never run
  on the panel.
- `CMD:WALLETBENCH` mode 4 (`stream`): **written, never run.** Estimated ~211 ms
  against the measured 283. If it wins, the shipped path should move to it; if it
  loses, the estimate was wrong and that is worth writing down. Until then
  `windowed` is what ships. **Open.**
- Still open after the on-panel session: whether FIT is legible in daylight,
  whether HALF on every page turn is too slow to page through a document, the real
  heap figures (nobody has run `heap_caps_get_info()` with a wallet on screen), and
  `search24.h`'s quarter turn.
