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

## The display path: one read, no scratch buffer

The payload **is** the framebuffer. The laptop-side generator does every
rotation, scale, dither and pack at build time; the device rotates nothing. So
one screen is:

```
open -> read the 32-byte header -> read rawLen bytes into the framebuffer -> displayBuffer()
```

`WalletStore::loadAssetIntoFrameBuffer()` (`src/activities/wallet/WalletStore.cpp:147-229`),
destination `renderer.getFrameBuffer()` (`lib/GfxRenderer/GfxRenderer.cpp:2072`),
length `renderer.getBufferSize()` (`:2074`), guarded by `hasFrameBuffer()`
(`lib/GfxRenderer/GfxRenderer.h:355`). The same move the sleep frame already
makes (`src/main.cpp:223-236`).

**No scratch buffer exists anywhere on that path, and none may be added.** A
screen is 48 KB on the X4, and with BLE up the largest contiguous heap block is
about 43 KB -- measured, `docs/map-memory.md:57`. Staging a screen would fail
before it could be drawn. There is nothing to stage anyway: the bytes are already
in panel order.

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
| LEFT / RIGHT (front) | step tile column at the current level, clamped |
| UP / DOWN (side) | step tile row at the current level, clamped |
| UP / DOWN (side), on a level with one tile row | previous / next page |
| BACK (front) | back to the browse list |

Why the side pair carries pages:

- The repo's existing convention already puts page turning on the side buttons --
  that is what `Button::PageBack` / `PageForward` are
  (`src/MappedInputManager.cpp:77-98`).
- On a level with a single tile row -- always true at FIT, which is 1x1 -- the row
  arrows have nothing to step. Overloading a pair that is idle beats overloading
  one that is not.
- FIT is where a reader flips pages anyway. DETAIL and 1:1 are for inspecting one
  region of the page in front of you; flipping to another page from inside a
  zoomed corner is not a move anybody makes.

`Button::Up`/`Down` are used directly rather than `PageBack`/`PageForward`, so
the reader's side-button swap setting does **not** apply here. Deliberate: the
same two buttons also step tile rows, where up must mean up.

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

Arrows clamp at the edges and never wrap (`WalletViewActivity.cpp:stepTile`) --
what a sheet of paper does. A page change resets to FIT (`stepPage`).

A level change does **not** carry the tile coordinate across: the three grids are
different sizes, so there is no honest mapping between their coordinates. It
opens at the level's **focal tile** instead, which the manifest names per level
as `defaultTileX`/`defaultTileY` (`WalletManifestParser.h`, `LevelGrid`;
`WalletViewActivity.cpp:jumpToLevelDefault`). The generator's rule is the centre
biased top-left, so a 4x4 1:1 grid points at 1,1. Verified on real output: tile
1,1 of the demo page's 1:1 level opens on body text at full size, where 0,0 would
have opened on the top-left margin.

Browse: UP/DOWN (also LEFT/RIGHT) move the selection with hard stops, CONFIRM
opens, BACK goes home (`WalletActivity.cpp:loop`).

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

Run it: `pio run -t wallet-preview` renders the committed fixture, or point the
binary at a whole tree:

```
build/test/wallet_preview/wallet_preview --tree DIR --level detail --col 1 --row 0 \
    --out /tmp/tile
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
  Now honoured: a level opens at its focal tile (above).
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
| `firmware.bin` | 3,939,600 | 3,950,496 | **+10,896** |
| `.flash.text` | 2,171,060 | 2,179,432 | +8,372 |
| `.flash.rodata` | 1,649,008 | 1,651,528 | +2,520 |
| `.dram0.data` | 18,145 | 18,145 | **0** |
| `.dram0.bss` | 40,152 | 40,152 | **0** |
| `.iram0.text` | 87,350 | 87,350 | 0 |

The rodata is mostly the 21 new `STR_WALLET_*` strings across 31 language arrays.
Static RAM is unchanged: the wallet has no globals.

Heap, **derived from the type sizes, not measured on hardware**:

- `WalletActivity` while up: 24 `ItemEntry` rows heap-allocated in `onEnter()` and
  freed in `onExit()` -- 50 bytes each (a 48-byte title, a `uint16_t` page
  count), so **1,200 bytes**, plus the
  activity object itself (~100 bytes). Held while the viewer is open too, because
  the viewer is pushed on top rather than replacing it.
- `WalletViewActivity` while up: **~150 bytes** -- a title, a page index, three
  `LevelGrid` pairs.
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
- Flash and static RAM: **measured** on the two builds above.
- Heap: **derived** from type sizes. Nobody has run `heap_caps_get_info()` with a
  wallet on screen.
- **Nothing here has run on hardware.** No device was flashed. What the preview
  cannot answer stays open: whether FIT is legible on e-ink in daylight (the PNG
  says the bits are right, not that a 217 PPI page dithered to one bit reads on
  glass), whether a 4x4 1:1 grid is navigable with six buttons in gloves, whether
  HALF on every page turn is too slow to page through a passport, and the real
  heap figures. One session on the X4 settles all of it.
