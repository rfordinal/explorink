# The wallet viewer

Papers a rider may have to show somebody -- passport, licence, insurance card,
registration, a boarding pass -- kept on the SD card and drawn on the panel with
no phone, no network and no map. **Read-only viewing only.** No crypto, no BLE,
no grey, no write path.

Three screens:

- `WalletActivity` -- the list of documents (`src/activities/wallet/WalletActivity.h`).
- `WalletViewActivity` -- one asset, one whole screen (`src/activities/wallet/WalletViewActivity.h`).
- `WalletCodeActivity` -- one machine-readable code, fullscreen, meant to be read
  by a scanner off the glass (`src/activities/wallet/WalletCodeActivity.h`). P2.
- `WalletUnlockActivity` -- the direction PIN, the only way into an encrypted
  wallet (`src/activities/wallet/WalletUnlockActivity.h`). P3.

**Encryption is a separate document.** The crypto path, the key's lifetime, the
threat boundary, the provisioning test path and the PBKDF2 measurement are
[`wallet-crypto.md`](wallet-crypto.md). Everything below is the viewer, and works
the same whether the tree is encrypted or not.

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
/trailink/wallet/manifest.json          cleartext tree only
/trailink/wallet/manifest.enc           encrypted tree only (wallet-crypto.md)
/trailink/wallet/manifest.bak           previous good manifest.enc -- not read yet
/trailink/wallet/<2 hex>/<16 hex>.dat   one asset = one full screen
```

A tree is either fully encrypted or fully cleartext, never mixed, and which one it
is, is stated by which manifest file exists (`treeIsEncrypted()`). **Both must keep
working**: a card in the field may be either.

Paths relative to `/trailink`, same root as the map
(`src/activities/map/MapActivity.cpp:54`). The manifest path and the wallet
directory are `src/activities/wallet/WalletStore.h:19` and
`src/activities/wallet/WalletAsset.h:32`.

An asset file is a 32-byte cleartext header, then the payload:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `EWA1` |
| 4 | 1 | assetType 1=FIT 2=DETAIL_TILE 3=ONE_TO_ONE_TILE 4=MACHINE_CODE 5=PAGE_IMAGE |
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
| 24 | 8 | first 8 bytes of sha256 of the payload -- checked for a machine code, ignored for everything else |

Parsed by `parseAssetHeader()` (`src/activities/wallet/WalletAsset.h:89-106`).
Every field has since been checked against bytes `tools/walletgen.py` wrote --
tile, page image and machine code (see "Read against real generator output" and
"Rendered and looked at").

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

Browse: UP/DOWN move the selection with hard stops, CONFIRM opens the document,
**LEFT/RIGHT open its machine-readable codes**, BACK goes home
(`WalletActivity.cpp:loop`).

LEFT/RIGHT used to be a second way to move the selection, and P2 took that away
-- brief section 13 puts the code walk there. What it costs: the selection now
moves on the side pair only, and the side pair has no hint box on the X4, so
nothing on screen says how to move it. Accepted, for two reasons: the side pair
is where this device puts list movement everywhere else, and the front pair's
hints now read `< Code` and `Code >`, which is the thing a rider would never have
guessed. The row itself says how many codes a document has, so LEFT/RIGHT doing
nothing is explained on screen rather than felt as a dead button.

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

## The code screen

Phase P2. `WalletCodeActivity` (`src/activities/wallet/WalletCodeActivity.h`), one
machine-readable code per screen, meant to be read by a **scanner** off the glass
rather than by a person.

That is the whole reason this screen exists separately from the document viewer.
A wrong pixel on a passport scan is cosmetic. A wrong pixel in a barcode is a
rider at a gate with a pass that will not scan and no way to tell why. Every
decision below follows from that one sentence.

The asset is an ordinary full-screen panel-native tile, `assetType 4`,
`bitDepth 1`, so the read path is `Store::loadAssetIntoFrameBuffer()`'s and
nothing about the display path changed.

### The manifest contract

Per page, beside `levels` (`WalletManifestParser.cpp`, `Ctx::CodesArr` /
`Ctx::Code`):

```json
"codes": [{"id": "c001", "symbology": "qr", "payload": "...", "verified": true,
           "assetId": "16 hex", "moduleSize": 8, "quietZone": 4,
           "codeWidthPx": 384, "codeHeightPx": 384, "sha256": "..."}]
```

Read into `CodeEntry` (`src/activities/wallet/WalletAsset.h`, "Machine-readable
codes"). Two fields are deliberately dropped:

- **`payload` is read by nobody.** The device draws a bitmap; it encodes nothing,
  so the decoded text has no use here. It is also the one field that can exceed
  the parser's 512-byte token buffer -- a boarding-pass payload runs to hundreds
  of characters -- and an overlong string value is dropped without a callback
  (`lib/JsonParser/StreamingJsonParser.cpp:226-249`), so it costs this parser
  nothing and breaks nothing. Pinned by
  `WalletCodeManifest.AnOverlongPayloadBreaksNothing`.
- **`rleLen`** is the sidecar's length, which no firmware in P1 or P2 reads.

`moduleSize`, `quietZone`, `codeWidthPx` and `codeHeightPx` are parsed and
logged. **Nothing on the device acts on them** -- the label placement reads the
framebuffer instead, see below -- so a generator that states them wrong cannot
move a pixel here.

### The code walk

Brief section 13, as implemented:

| where | button | hint | what happens |
|---|---|---|---|
| browse | RIGHT | `Code >` | opens the item's **first** code |
| browse | LEFT | `< Code` | opens the item's **last** code -- the other way round the ring |
| browse | either, no codes | as above | nothing at all. No refresh, no message |
| code screen | RIGHT | `Code >` | next code, wrapping |
| code screen | LEFT | `< Code` | previous code, wrapping |
| code screen | BACK | `Back` | back to the browse list |
| code screen | CONFIRM, UP, DOWN | -- | nothing |

Codes belong to a **page** in the manifest and the walk is **item-wide**: the
codes of page 0, then page 1, in manifest order (`ManifestParser::commitCode()`).
A rider with a two-page boarding pass thinks of "the codes on this ticket", not
"page 2's code". `WalletCodeManifest.CodesOfEveryPageAreOneWalk` pins the order.

#### The walk wraps; the document pan clamps

Two different behaviours on the same two buttons, so the reason is stated rather
than left to be discovered:

- a **document** pan clamps. A page has edges because paper has edges, and a press
  at the edge doing nothing is what a sheet of paper does;
- the **code** walk wraps. A document's codes have no edges and no spatial
  meaning -- a rider at a gate flips between a boarding pass and a bag tag, and
  should not have to remember which end of a two-item list they are at. On a ring
  of two, which is the common case, clamping would leave one of the two buttons
  dead half the time.

One function decides it, `wallet::walkCodeIndex()` (`WalletAsset.h`), and the
browse screen's entry points are steps on the same ring rather than a second rule:
RIGHT steps on from before the start (index 0), LEFT steps back off the beginning
(the last code). A ring of one returns where it started, so the caller spends no
refresh; a ring of none returns -1, its cue to do nothing. An index left over from
a shorter manifest cannot walk out of range either, which is how the code screen
recovers when the card changed under it. Pinned by
`WalletCodeWalk.WrapsAtBothEndsAndTheBrowseEntryIsTheSameRing`.

#### The hints are directional

`< Code` and `Code >`, one per front button
(`STR_WALLET_CODE_PREV` / `STR_WALLET_CODE_NEXT`), passed to
`mapLabels(back, confirm, previous, next)` which swaps them itself when the nav
direction is swapped -- so the arrow always points where the button goes.

They started as two boxes both reading `Code`, and that cost a real
misdiagnosis on the panel, 2026-08-18: the maintainer could not tell which button
went which way, pressed the left one, landed on the **last** code -- a landscape
PDF417, which stands vertical on a portrait screen -- and concluded the code was
drawn in the wrong orientation. It was not. **A label that forces a guess is a
defect**, not a cosmetic gap, because the guess becomes a bug report about
something that works.

Widths, **measured** with the firmware's own `ubuntu_10` table through
`EpdFont::getTextDimensions()` -- the same call `GfxRenderer::getTextWidth()` makes
on the device -- against the 106 px hint box
(`src/components/themes/BaseTheme.cpp:165`), where the text is centred and anything
wider spills over both borders:

| label | width | verdict |
|---|---|---|
| `< Code` | 65 px | 41 px of slack |
| `Code >` | 65 px | 41 px |
| `Back` | 45 px | for scale |
| `Select` | 60 px | for scale |
| `Prev code` | 92 px | **rejected** -- 14 px left for every other language |
| `Next code` | 95 px | **rejected** -- 11 px |

That is why the labels are the short arrow form and not words. `<` and `>` are both
in the face (checked with `EpdFont::hasCodepoint()`), as are the Cyrillic and
Latin-1 ranges a translation would need.

`WalletCodeHints.*` reads the labels **out of every translation file** and measures
what it finds, rather than carrying its own copy of the string: the risk is a
translator writing something longer, and a test with a hardcoded copy cannot see
that. It also asserts the two labels differ and that at least one carries a
direction. Both checks were **proved to fail** on a deliberately long, identical
pair (215 px) before being committed.

CONFIRM is inert because there is no zoom for a code: it is already drawn as large
as the panel allows, and a scanner needs the quiet zone more than the rider needs
a bigger picture. The side pair is inert because a code is exactly one screen --
nothing to pan -- and every page's codes are already in this one walk, so there is
no page to turn either.

**The browse row states the count** ("2 pages, 1 code",
`WalletActivity.cpp:drawRow`). Without it, "this document has no code" and "that
button does nothing" look identical, and this codebase does not do silent
omissions elsewhere either (the truncated-list line above). The count is filled by
the same list pass that fills the titles -- `ItemEntry::codeCount`, two bytes a
row, against a second manifest pass per row.

A code whose `assetId` is not 16 hex characters is refused (same rule as
everywhere else) **but still counted**: the count is what the manifest claims the
document has, and a broken entry the rider can see and report beats one the device
hides. Pinned by `WalletCodeManifest.ACodeWhoseAssetIdIsNotSixteenHexIsNoCode`.

### Why the hash is checked here and nowhere else

The header carries the first 8 bytes of the payload's sha256 and the manifest
carries all 32 (`AssetHeader::sha256Prefix`, `CodeEntry::sha256`). Both are
ignored for a document tile and for a page image. For a code, the payload is
hashed before anything reaches the panel (`Store::loadCodeIntoFrameBuffer()`,
`WalletStore.cpp`).

The asymmetry is the point:

- a corrupt **document tile** is a smudge on a passport scan. The rider sees it
  and knows;
- a corrupt **barcode** is silent. It looks exactly like a barcode, and the
  failure surfaces at the gate, in front of somebody with a scanner;
- a **page image** is read a window at a time, so hashing the whole file would
  cost several times the read it was guarding.

**One read, not two.** The brief called for a verify pass plus a draw pass, about
two reads of 48 KB. It is one: the payload *is* the framebuffer, so after the read
the bytes to hash are already in RAM. ~65 ms of card (measured, "Measured on the
X4") plus the hash of 48 KB in software, and nothing has been drawn yet -- the
caller refreshes only if the hash matched, so a failed hash never puts a bitmap on
the panel. The RenderLock is held across the read, the hash and the refresh, so
nothing can write those bytes in between (`WalletCodeActivity::showCurrent`).

The hash cost on the device is **not measured** -- estimated at under 10 ms from
~25 cycles a byte at 160 MHz, which would be under 2 % of the frame. Settling it
needs one `micros()` pair around the hash with the log open. **Open**, and it does
not change the decision at any plausible value: the frame is dominated by a
1,684 ms `HALF` waveform.

Which hash is the authority (`checkPayloadHash()`,
`src/activities/wallet/WalletSha256.h`):

| the manifest's `sha256` | authority | cover |
|---|---|---|
| 64 hex characters | the manifest, all 32 bytes | 256 bits |
| absent, short, long, or not hex | the header's 8-byte prefix | 64 bits |

A short hash is **never** accepted as a hash -- silently taking 63 characters
would turn the check into a formality
(`WalletSha256.HexParseRefusesAnythingThatIsNotSixtyFourHex`).

**This detects corruption, not tampering.** Anybody who can write the manifest can
write the payload beside it, and P2 ships no signature. A half-written card, a bad
sector or an interrupted sync cannot put a wrong barcode in front of a gate agent;
a hostile card can. That is a P3 problem (the phase that adds encryption), and
saying otherwise here would be worse than saying nothing.

sha256 is implemented a **second time** in this tree
(`src/activities/wallet/WalletSha256.h`; mbedtls is the first, used for the OTA
image at `src/network/FirmwareFlasher.cpp:129-224`). Reason: the host tests do not
link mbedtls, and the hash-mismatch path is precisely the path that has to be
proved to fire against real generator bytes. A verify step nobody can test on the
host is a verify step nobody has tested. It costs ~1 KB of flash and is pinned to
the FIPS-180-4 vectors (`WalletSha256.MatchesTheStandardVectors`), including the
55- and 56-byte lengths where a hand-written `finish()` goes wrong.

The gate also checks the **type**: `checkCodeAsset()` refuses an asset that is not
`assetType 4` even when it is the right shape for the panel. A `codes` entry
pointing at a document tile is a generator or a sync bug, and drawing page three of
an insurance policy where a boarding pass belongs is worse than drawing nothing.

### Unverified codes are shown, marked

The manifest's `verified` is the generator's own round trip: it decodes the code,
regenerates it clean, renders the device asset, and decodes the code **back out of
that asset** (`tools/walletgen.py`, `verify_code_asset`). Brief section 11: only a
verified code is a trusted fullscreen code.

Two ways to honour that, and this is the one chosen:

**An unverified code appears in the walk and is drawn with an explicit marker --
"NOT VERIFIED - may not scan". If the marker cannot be placed, the code is not
drawn at all.**

Why, against hiding it:

- hiding it makes a dead button. LEFT/RIGHT would do nothing and the rider would
  have no way to tell "this document has no code" from "this device does not trust
  the code it has". The browse row would say 1 code and pressing would do nothing:
  worse than either honest answer;
- `verified` false says the laptop's decoder could not read it back. That is not
  the same as "this will not scan" -- a different scanner at a gate may well read
  it, and the rider is the one standing there. Making that call for them,
  invisibly, is the kind of quiet decision this codebase refuses elsewhere;
- brief section 11 is still satisfied: it is drawn, and it is never *presented as
  trusted*. The marker is a hard requirement, not a decoration -- no marker, no
  draw.

The gate is one pure function with exactly one call site
(`codeVerdict()`, `WalletAsset.h`; called from
`WalletCodeActivity::showCurrent()`):

| loaded and hashed | manifest `verified` | marker placed | verdict |
|---|---|---|---|
| no | -- | -- | `RefuseAsset` -- failure screen |
| yes | yes | yes | `Draw` |
| yes | yes | no | `Draw` -- a verified code needs no marker |
| yes | no | yes | `Draw`, marked |
| yes | no | no | `RefuseUnmarked` -- failure screen |

`verified` **defaults to false**. A manifest that says nothing has verified
nothing, and the safe reading of silence is "not verified"
(`WalletCodeManifest.UnverifiedAndMissingVerifiedBothReadAsUnverified`).

### The label, and why it reads the framebuffer

One line, centred, at the bottom of the logical screen: the symbology, upper-cased
(`symbologyLabel()`), plus the unverified marker when there is one. Nothing else --
no code index, no payload, no button hints. **The quiet zone is part of the code**:
a status line across a quiet zone breaks a scan exactly as a mark across a module
does.

The obvious placement is "below the code, using `codeHeightPx` and the fact that
the generator centres it". That was rejected. It is an assumption about a tool in
another repo, guarding the one thing on this screen that must not be drawn over.

Instead the framebuffer is asked: `logicalBandIsBlank()` (`WalletAsset.h`) walks
the band the label would occupy and returns false if there is a single inked
pixel. No assumption, and it degrades correctly -- a tall symbology that fills the
panel simply gets no label, and if that code is unverified it is refused instead.

The band is a **panel column range, not a row range**. Logical portrait `(x, y)`
maps to panel `(y, panelHeight - 1 - x)`
(`GfxRenderer::rotateCoordinates()`, `lib/GfxRenderer/GfxRenderer.cpp:216-223`), so
a horizontal band of the page is a vertical slice of panel memory across every
panel row. Getting that the wrong way round would check the wrong 480 columns and
pass a band that is not blank at all --
`WalletCodeLabel.TheBandIsCheckedInPanelColumnsNotRows` puts one pixel of ink in
the band and asserts the label is refused.

The band is at the very bottom edge (10 px margin, 4 px pad either side of the
text) and no taller than the text, because the further from the code the smaller
the chance of sitting in its quiet zone. **How far is far enough is reasoned, not
measured** -- nothing has been scanned off this panel yet.

`logicalBandIsBlank()` knows one logical-to-panel mapping, the Portrait one the
assets are generated for (`presentation = 1`). In any other orientation the band
cannot be located, so no label is drawn at all and an unverified code is refused
(`WalletCodeActivity::drawLabel`).

### Refresh: HALF, and never FAST

Every code frame -- entry and every step of the walk -- is `HALF_REFRESH`.

`FAST` is differential: it drives only the pixels that differ from the RED plane
(`docs/refresh-modes.md`), so stepping from one code to the next would leave the
previous code's modules as ghosts under this one. A scanner reads contrast between
neighbouring modules and nothing else, so that is the one artefact this screen
cannot afford. Brief section 12's rule -- scanning reliability beats saving a
refresh -- costs 1,684 ms a frame instead of 500 ms, and this is exactly the screen
where that is the right trade.

**`FULL` was considered and rejected.** On the X4 `HALF` (`0xD7`) and `FULL`
(`0xF7`) both rewrite both planes and clear the panel absolutely; `FULL` only adds
inversion passes on the way (`docs/refresh-modes.md`, "What each one selects on the
X4"). So `HALF` is the strongest *clean* this panel has, and `FULL` would buy a
flash storm and an untimed wait in front of somebody holding a scanner, for no
extra cleanliness. That reading is **off the driver source, not measured** --
`FULL` has never been timed on this device.

If a real scan test later fails on ghosting, `FULL` is the first thing to try, and
it needs a reason in a comment when it lands (this repo's rule). Until a scanner
has been pointed at the glass, nothing here is settled by measurement.

### Rendered and looked at, 2026-08-18

`test/wallet/fixtures/codes/` is verbatim generator output:

```
tools/walletgen.py --demo --paper a4 --panel x4 --title "Boarding pass" \
    --code "qr:M1DOE/JOHN       EABC123 BTSFRAAF 0123 250Y012C0045 100" \
    --code "pdf417:M1DOE/JOHN EABC123 BTSFRAAF"
```

Both codes came back `verified` from the generator's own round trip. Read back
through the firmware's own code with `wallet_preview --code N` and **looked at**:

| what | QR (c001) | PDF417 (c002) |
|---|---|---|
| header | type 4, 800x480, rawLen 48,000, presentation 1 | same |
| hash | **MATCH** against the manifest's sha256 | **MATCH** |
| modules | 29x29 at 12 px | 120x27 at 3 px |
| drawn box incl. quiet zone | 444x444, manifest says 444x444 | 384x105, manifest says 384x105 |
| margins | 66 px each side, quiet zone needs 48 | 60 px each side, needs 12 |
| centring | 0 px off on both axes | 0 px on x, 1 px on y (an odd height) |
| share of the 480 px logical width | 92.5 % | 80.0 % |
| label band (logical y 772..794) | blank -- the label fits | blank |
| ink | 15.56 % | 4.35 % |

Judged by eye off `code_qr-portrait.png`: **upright, centred, module grid crisp
with hard black/white edges and no dither or smear, quiet zone clearly wider than
the four modules asked for, and as large as a 480 px-wide panel allows.** The
PDF417 is centred and crisp too, and its 3 px modules are the honest worry on this
screen -- a wide symbology on a 480 px panel gets four times less module than a QR
does. That is a generator-side trade (module size falls out of the matrix width),
not something this screen can fix, and whether 3 px scans off e-ink glass is
**open** until somebody points a scanner at it.

One extra check, outside the firmware: both PNGs -- the ones the firmware's own
reader wrote -- decode back to the exact payloads through `zxing-cpp` on the
laptop. So the round trip closes through this side's read path as well as the
generator's. **It closes on a PNG, not on the panel.**

### CMD:GOTO_WALLET

`src/main.cpp`, in the same dispatch as `CMD:GOTO_MAP` and `CMD:GOTO_TILESYNC`
(which all drop power saving first -- at 10 MHz every timing on this device is a
lie, `docs/power-management.md`).

```
CMD:GOTO_WALLET              the browse list
CMD:GOTO_WALLET 0            open document 0
CMD:GOTO_WALLET 0 3          open document 0's code 3, fullscreen
```

**It exists so a verification round does not need a person at the device.** The
wallet was three presses deep -- Down, Down, Select -- and every asset push meant
walking back into it, so every look at the screen cost somebody standing there.
That is how a screen ends up checked once and never again; the same argument that
bought `CMD:GOTO_TILESYNC`. The code index matters more than the item index: a code
screen is *four* presses deep and there was no other way for a host script to land
on one and screenshot it.

Replies `GOTO_WALLET_OK item=<n> code=<n>` for a well-formed request, or
`GOTO_WALLET_ERR usage: ...` for one it refuses.

What it refuses, and where (`wallet::parseGotoWalletArgs()`, `WalletAsset.h`):

| argument | outcome |
|---|---|
| nothing | the browse list |
| `0` | document 0 |
| `0 3` | document 0, code 3 |
| `-1`, `x`, `0x2`, `1.5` | `GOTO_WALLET_ERR` -- refused, not coerced |
| `1 2 3` | `GOTO_WALLET_ERR` -- a third argument is a typo, not a feature |
| `99999` | `GOTO_WALLET_ERR` -- no wallet has ten thousand documents |

A refusal leaves both indices at -1, so a caller that ignores the return value still
cannot open the wrong document. `WalletGotoArgs.*` covers every row.

**Range is checked in the activity, not in the command, and never clamped.**
Nothing outside the manifest knows how many documents a wallet holds, and the
manifest is only read once `WalletActivity::onEnter()` runs -- which happens after
the command handler has already replied. So:

- the **command** validates syntax and replies `OK`, exactly as `CMD:GOTO_MAP`
  replies `OK` without checking that its route path exists;
- the **activity** checks the index against the real list. Out of range logs
  `GOTO_WALLET: no item 9 (wallet has 2)` and puts *"No document 9 in this
  wallet"* on the panel, then shows the list
  (`WalletActivity::applyGotoTarget()`). Clamping to document 0 was rejected
  outright: a host script that mistypes an index would be handed the wrong
  document and believe it got what it asked for, which is the worst possible
  failure for an automated loop.

The panel message matters as much as the log line, because a host-driven loop
verifies by screenshot -- a refusal that only appears in the serial log is invisible
to the thing doing the checking.

Two implementation notes worth knowing before the next screen gets a `GOTO_`:

- **The request travels in the activity's constructor**, not through a global.
  `goToWallet(item, code)` builds `WalletActivity` with them and
  `applyGotoTarget()` consumes them once in `onEnter()`. Once, deliberately:
  coming back out of the code screen must land on the list, not reopen the same
  code for ever.
- **Pushing a child activity from `onEnter()` is supported.**
  `ActivityManager::loop()` empties `pendingActivity` before it calls `onEnter()`
  and handles a new pending action on its next iteration
  (`ActivityManager.cpp:153-159`). The list frame is skipped entirely when a target
  was reached, so landing on a code costs one `HALF` refresh instead of two and does
  not flash the list on the way.

The code index needed no contortion in the activity API, so the full three-argument
form shipped.

### Confirmed on the panel, 2026-08-18

Flashed and used:

- **codes render** and the walk works;
- the drawn framebuffer was **byte-diffed against the stored asset** and differs by
  **133 bytes** -- the symbology label and nothing else. So the read path puts the
  asset on the panel unchanged, measured rather than reasoned, and the label lands
  where the blank-band test said it would;
- the two front hints both read `Code`, which cost a misdiagnosis and was fixed in
  the same pass (see "The hints are directional").

One thing that looked like a bug and was not: the demo PDF417 is the landscape
variant, so it stands **vertical** on a portrait screen. That is the code the
generator was asked for, drawn correctly.

### Scanned off the panel — 2026-08-18, all eight codes read

The one test no amount of byte-checking could stand in for has been run. Binary
Eye 1.75.2 (F-Droid, `zxing-cpp`) on a phone, each code straight off the glass:

| code | module | verdict |
|---|---|---|
| pdf417 portrait | **2 px (~0.23 mm)** | **reads** |
| pdf417 landscape | 4 px | reads |
| qr, both orientations | 9 px | reads |
| aztec, both orientations | 10 px | reads |
| code128, both orientations | 2 px and 4 px | reads |

So e-ink contrast is enough, the surface reflection does not defeat a camera at a
normal angle, `HALF` leaves no ghosting a decoder trips over, and the label at the
bottom edge is far enough from the quiet zone. The 2 px PDF417 reading is the
notable one: a real 115-character IATA boarding pass at the bottom edge of the
X-dimension handheld imagers specify, and it read anyway. **The landscape variant
is headroom, not a requirement** — nobody has to turn the device sideways at a
gate.

Two limits, stated because the claim will be repeated:

- **A phone camera is not a gate imager.** Airline and rail scanners bring their
  own illumination and optics, and some retail scanners are laser rather than
  imaging. Failure here would have been decisive; success is strong evidence, not
  a guarantee.
- **Binary Eye is `zxing-cpp`-based**, the same decoder family the generator uses
  to verify its own output, so the decode half is not independent of our
  toolchain. What was independent, and what was actually under test, is the
  physical path: glass, ambient light, optics, a real lens at a real distance.

Still unmeasured: what a backlight would do to a scan, and any scanner that is not
a phone.

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
| **entering a code** | `HALF` | see below -- a code is never `FAST` |
| **stepping to the next code** | `HALF` | same |

No grayscale path exists here and none should be added: grey costs a second
waveform pass (`docs/eink-grayscale.md`).

## No overlay

The asset fills the panel and nothing is drawn on top of it -- no level badge, no
page counter, no button hints. Every pixel belongs to a paper somebody may have
to read. The cost is discoverability: the button map is only in this document and
in the user guide, not on the screen. **Open** -- worth revisiting after the
first real use, and worth measuring rather than guessing.

The code screen has **one** exception, and it is not a relaxation of this rule:
one line naming the symbology, drawn only where the framebuffer proves there is
no ink. See "The label, and why it reads the framebuffer".

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
| the item has no codes at all | "This document has no codes" -- unreachable from browse, which checks the count first |
| a code asset that is not `assetType` 4 | "This code file is damaged" |
| a code whose payload does not hash | "This code does not match its checksum" + "Not shown: a wrong code is worse than none" |
| an unverified code with nowhere to put the marker | "No room to mark this code unverified" |
| an encrypted tree and no key held | "Wallet is locked" -- normally unreachable, the PIN screen comes first |
| the manifest's GCM tag does not verify | "Wrong key, or the wallet list was altered" |
| an encrypted manifest over the 32 KB cap, or no heap for it | "Wallet list is too big for this device" |
| an asset that did not decrypt to its plaintext | "This page did not decrypt" |

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

## What is deliberately absent

- ~~**Encryption.**~~ Done in P3 -- `flags` bit 0 is decrypted with the session
  key, and refused with a message when there is no key.
  [`wallet-crypto.md`](wallet-crypto.md).
- **sha256 verification of a document tile or a page image.** Still parsed and
  checked by nobody there. A machine code is the exception and the only one --
  see "Why the hash is checked here and nowhere else".
- **BLE.** Nothing in the wallet opens a radio. Assets arrive by whatever put
  them on the card.
- **Grey.** `bitDepth` 2 is refused.
- ~~**Machine codes.**~~ Done in P2 -- `assetType` 4 and the manifest's `codes`
  array are read and drawn. See "The code screen".
- **Any write path.** See above.
- ~~**A generator.**~~ `tools/walletgen.py` (parent repo) writes these files, and
  everything in this document that says "real generator output" means its bytes.

## Cost

### P2, the code screen

Measured against the branch tip before this work, `pio run -e default`,
`riscv32-esp-elf-size -A`, both builds on the same machine:

| | before P2 | with P2 | delta |
|---|---|---|---|
| `firmware.bin` | 3,956,480 | 3,964,832 | **+8,352** |
| `.flash.text` | 2,183,814 | 2,189,548 | +5,734 |
| `.flash.rodata` | 1,653,128 | 1,655,760 | +2,632 |
| `.dram0.data` | 18,145 | 18,145 | 0 |
| `.dram0.bss` | 41,176 | 41,176 | **0** |
| `.iram0.text` | 87,350 | 87,350 | 0 |

**No static RAM at all.** The rodata is the 12 new `STR_WALLET_*` strings across 31
language arrays (~195 bytes a string, the same rate P1 paid) plus sha256's 256-byte
round-constant table. The text is sha256 (~1 KB), the code activity, the parser's
code mode and `CMD:GOTO_WALLET`.

The first figure of this table was +7,072, before the directional hints and
`CMD:GOTO_WALLET` were added in the same pass; both numbers were measured the same
way, against the same baseline.

Heap while the code screen is up, **derived from the type sizes, not measured**:
one `CodeEntry` (~110 bytes: a 17-byte assetId, a 65-byte hash, a 12-byte
symbology, four `uint16_t`) plus the activity object, and the same transient
~750-byte `ManifestParser` every other lookup allocates and frees. No scratch
buffer: the hash reads the framebuffer where it lies. The browse screen grew a
64-byte message buffer for a refused `CMD:GOTO_WALLET` index and two ints.

### P1, the viewer

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

- Format layout, path mapping, level cycle, manifest shape, the panel gate, the
  codes array, the code walk, the hash and the label band: **host-tested**,
  `test/wallet/WalletTest.cpp`, 75 cases, `pio run -t unit-tests`.
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
- **P2, the code screen: on the panel, not yet scanned.** Rendering and the walk
  are **confirmed on the panel** 2026-08-18, and the drawn framebuffer was
  **byte-diffed** against the stored asset -- 133 bytes of difference, all label.
  The manifest fields, the walk order and its wrap, the hash (match and mismatch),
  the unverified verdict, the label band and `CMD:GOTO_WALLET`'s arguments are
  **host-tested** against verbatim generator output; the two codes were **rendered
  and looked at** as PNGs and both decode back through `zxing-cpp` on the laptop.
  The hint-label widths are **measured** with the firmware's own font table. The
  refresh choice and the `FULL`-vs-`HALF` reasoning are **read off the driver
  source**. The hash's millisecond cost is **estimated, not measured**. Whether any
  of it scans off e-ink glass is **completely open** -- see "Nothing has been
  scanned off the panel" -- which is no longer true, see "Scanned off the panel".
- **P3, encryption: written and compiled, never run on hardware.** The CTR
  arithmetic, the manifest envelope, the KEK formula, the PIN codec and the rate
  limiter are **host-tested** against verbatim encrypted generator output with an
  independent crypto library, and an encrypted tree **renders byte-identically** to
  its cleartext twin through `wallet_preview --key`. mbedtls's own calls, NVS, the
  unlock screen and the PBKDF2 timing are **unverified** --
  [`wallet-crypto.md`](wallet-crypto.md) says exactly what and how to settle it.
- Still open after the on-panel session: whether FIT is legible in daylight,
  whether HALF on every page turn is too slow to page through a document, the real
  heap figures (nobody has run `heap_caps_get_info()` with a wallet on screen), and
  `search24.h`'s quarter turn.
