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

Consequence, and an **open gap**: the manifest carries no panel identity, so a
wallet built for the X4 and put on an X3 shows "This page was built for another
screen" on every page. Whoever builds the generator has to decide whether the
manifest names a panel or the card carries one asset set per panel.

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
| UP / DOWN (side) **at FIT** | previous / next page |
| BACK (front) | back to the browse list |

Why the side pair carries pages:

- The repo's existing convention already puts page turning on the side buttons --
  that is what `Button::PageBack` / `PageForward` are
  (`src/MappedInputManager.cpp:77-98`).
- At FIT the level is exactly one tile, so the row arrows have nothing to step.
  Overloading a pair that is idle beats overloading one that is not.
- FIT is where a reader flips pages anyway. DETAIL and 1:1 are for inspecting one
  region of the page in front of you; flipping to another page from inside a
  zoomed corner is not a move anybody makes.

`Button::Up`/`Down` are used directly rather than `PageBack`/`PageForward`, so
the reader's side-button swap setting does **not** apply here. Deliberate: the
same two buttons also step tile rows, where up must mean up.

Arrows clamp at the edges and never wrap (`WalletViewActivity.cpp:stepTile`) --
what a sheet of paper does. A level change resets to the top-left tile
(`cycleLevel`), because the three grids have different sizes and there is no
honest mapping between their coordinates. A page change resets to FIT
(`stepPage`).

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
| `firmware.bin` | 3,939,600 | 3,949,408 | **+9,808** |
| `.flash.text` | 2,171,060 | 2,178,692 | +7,632 |
| `.flash.rodata` | 1,649,008 | 1,651,192 | +2,184 |
| `.dram0.data` | 18,145 | 18,145 | **0** |
| `.dram0.bss` | 40,152 | 40,152 | **0** |
| `.iram0.text` | 87,350 | 87,350 | 0 |

The rodata is mostly the 19 new `STR_WALLET_*` strings across 31 language arrays.
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

- Format layout, path mapping, level cycle, manifest shape: **host-tested**,
  `test/wallet/WalletTest.cpp`, 18 cases, `pio run -t unit-tests`.
- Byte order, bit polarity, framebuffer accessors, refresh modes, button
  mapping: **read off the code**, cited above.
- Flash and static RAM: **measured** on the two builds above.
- Heap: **derived** from type sizes. Nobody has run `heap_caps_get_info()` with a
  wallet on screen.
- **Nothing here has run on hardware.** No device was flashed, no asset file has
  ever existed. Everything about how a real document looks on the panel -- whether
  FIT is legible at all, whether a 4x4 1:1 grid is navigable with six buttons,
  whether HALF on every page turn is too slow to page through a passport -- is
  **open** and needs a generator plus one session on the X4.
