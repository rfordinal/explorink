# Grey in the wallet viewer (P2b)

The panel does four grey levels, not one bit (`docs/eink-grayscale.md`). A wallet
page is a photograph of a paper document, which is the one thing on this device
that might actually want them. **P2b builds both versions of the same page and a
switch between them, so a person can decide on the glass.** It does not decide.

Status: **built, host-verified, one frame seen on a panel** (2026-08-18: it
rendered, it cost 2,604 ms, and it exposed two defects in the instruments that were
supposed to watch it -- see "What a host can see of a grey frame"). Everything below
marked *measured* is measured on the host unless it names the panel; everything
about how it *looks* is still open and named as such at the bottom.

The viewer itself is `docs/wallet-viewer.md`; the panel mechanism is
`docs/eink-grayscale.md`; the crypto is `docs/wallet-crypto.md`. This file is
only the grey part.

## Two asset types, and the device draws one of them

Both are page-image geometry -- the whole page, not the panel -- with the same
32-byte header as every other asset (parent repo `docs/wallet-format.md`,
section 5), and both say `bitDepth = 2`.

| assetType | name | payload |
|---|---|---|
| 6 | `PAGE_IMAGE_GREY` | 2 bits per pixel, 4 pixels per byte, MSB-first, row stride `(width + 3) / 4` |
| 7 | `GREY_PLANES` | three 1bpp planes -- base, LSB, MSB -- concatenated, each `rowBytes x nativeHeight`, stride `(width + 7) / 8` |

Level values in the 2bpp form: **0 = black, 1 = dark grey, 2 = light grey,
3 = white** -- the renderer's own bitmap numbering
(`lib/GfxRenderer/GfxRenderer.cpp:446-459`).

### Both grey types say bitDepth 2

The field describes the **picture**, not the stride of one plane. A `GREY_PLANES`
asset is built out of 1bpp planes and still says 2, because what it encodes is
four levels. `checkAssetCommon()` compares the depth **exactly**
(`src/activities/wallet/WalletAsset.h:104`, the gate at `:509` and `:520`), so:

- a 1bpp reader handed a grey asset refuses it instead of drawing the page at
  half width;
- a grey reader handed a 1bpp asset refuses it instead of reading the second and
  third plane out of the next file's worth of nothing.

Both refusals are tested (`test/wallet/WalletTest.cpp`,
`WalletGreyGate.RefusesBitDepthThatIsNotTwo`).

### The plane bits, and the two conventions that meet in them

This is the part that is easy to get silently wrong, so it is written **once**,
as `constexpr`, in terms of the firmware's own grey encoding
(`lib/GfxRenderer/GrayShade.h`) -- `greyPlaneBit()`
(`src/activities/wallet/WalletAsset.h:165`) and its inverse
`greyValueFromPlaneBits()` (`:181`).

| level | base plane bit | LSB plane bit | MSB plane bit | LUT slot |
|---|---|---|---|---|
| black | 0 (ink) | 0 | 0 | `00`, no drive |
| dark grey | 0 (ink) | 1 | 1 | `11`, stronger nudge |
| light grey | 0 (ink) | 0 | 1 | `10`, mid nudge |
| white | 1 | 0 | 0 | `00`, no drive |

Two different polarities, both load-bearing:

- **base plane** is the framebuffer's: bit 1 = white, bit 0 = ink, and **black and
  both greys are all ink**. A grey pixel is a black pixel the planes lighten. Lose
  the planes and the page reads **black, not white**.
- **LSB / MSB planes** are the controller's: bit 1 = *nudge this pixel*. That is
  exactly what `clearScreen(0x00)` plus `drawPixel(state=false)` leaves in the
  strip scratch `GrayscaleFrame` streams (`GfxRenderer.cpp:452-457`), so a baked
  plane has to hold the same bits.

Naming trap inherited from the driver: "LSB" is BW RAM (`0x24`), "MSB" is RED RAM
(`0x26`) -- the names are about the grey-level bit, not the RAM plane
(`docs/eink-grayscale.md`).

## The device draws the baked planes. Three reads, no render.

`GreyPageReader::render()` (`src/activities/wallet/WalletGreyPage.cpp:180`) is the
whole grey path:

```
read the BASE plane's window into the framebuffer      (1 x 48,000 B, 480 strided rows)
displayGrayscaleBase(HALF)
preconditionGrayscale()          no-op on X4, an OEM settle on X3
waitRefreshComplete()
for plane in {LSB, MSB}:
  for band of 80 physical rows:
    read the band's window into 8 KB of scratch        (6 bands x 8,000 B)
    writeGrayscalePlaneStrip(plane, scratch, band, rows)
displayGrayBuffer()                                    the grey nudge
cleanupGrayscaleWithFrameBuffer() + resyncControllerBwRam()
```

**Nothing draws a pixel.** The generator baked the planes, so a grey page is
144 KB of card reads and four driver calls -- not the 13-callback
`GrayscaleFrame::render()` the map would pay (`docs/eink-grayscale.md`, the
optimisation note). That is why this shipped and the 2bpp path did not:

- **`GREY_PLANES` is what the device reads.** No bit shuffling: a window of the
  base plane *is* a framebuffer, and a band of a plane *is* what
  `writeGrayscalePlaneStrip()` wants.
- **`PAGE_IMAGE_GREY` (2bpp) is not read by the device at all.** It would need a
  2bpp-to-three-planes expansion per band, on top of reading the window twice
  (once for the base, once for the planes), and the baked asset already carries
  the answer. It is smaller on the card -- 2 bits a pixel against 3 -- which is a
  BLE argument, not a render argument.
- The fallback the brief allowed (`GrayscaleFrame::render()` with a callback
  reading the 2bpp asset per band) was **not needed and not built**. It would
  have called the callback 13 times, and the callback cannot know which band it
  is drawing, so the card reads would have had to be tracked in the context --
  more state and more reads for the same picture.

`PAGE_IMAGE_GREY` still earns its place: it is the only **independent** statement
of the same picture, and the host preview reads it to cross-check the planes. The
device never looks at a grey pixel, so nothing on the device can notice a wrong
bit order -- see "Verification" below.

Band size is `GrayscaleFrame::STRIP_ROWS`, taken from it rather than repeated
(`WalletGreyPage.cpp:24`): 80 physical rows x 100 bytes = **8,000 bytes of
scratch**, allocated and freed inside the call. The RLE sidecar uses the same 80
for the same reason (parent repo `docs/wallet-format.md`, section 7).

### The rules it obeys, and why each one is in the code

All four come from `docs/eink-grayscale.md`, and three of them were learned by
breaking a panel:

- **The base frame goes up before a single plane byte is written.** The LSB plane
  *is* BW RAM, so writing planes destroys the frame the controller holds.
- **`cleanupGrayscaleWithFrameBuffer()` + `resyncControllerBwRam()` run before
  `render()` returns, on every path that wrote a plane byte** -- including the
  failure path (`WalletGreyPage.cpp:265`). Without them the next windowed or fast
  update is silently promoted to a full HALF that wipes every grey
  (`Ssd1677Driver.cpp:434-437`), and BW RAM still holding plane bits drives
  nearly every pixel to black.
- **Nothing refreshes in the same breath as the grey render.** The frame says
  everything it has to say and the function returns. Two builds destroyed the
  picture doing otherwise.
- **The first BW frame after a grey one is HALF, never FAST**
  (`WalletViewActivity.cpp:382-386`). Grey residue ghosts the following frame and
  a plain fast diff cannot clear it; the reader forces the same cadence
  (`EpubReaderActivity.cpp:1560-1567`). The promotion is logged when it happens,
  so it is visible rather than assumed.

### Failing without leaving a wrong page on the glass

The 8 KB scratch is allocated **before** the base frame is displayed, and
`supportsStripGrayscale()` is asked before that. Reason: a base frame with no
planes behind it is a page whose greys are **black** -- darker and less legible
than the 1bpp version of the same page. So the two "not here, not now" answers
(`NoGreySupport`, `NoScratch`) happen with nothing drawn, and the viewer quietly
draws the 1bpp page instead (`greyOutcomeIsCapability()`,
`WalletGreyPage.h:70`).

Anything else -- a missing file, a header that disagrees with the manifest, a
short read, a failed decrypt -- is the card being wrong, and that gets the
failure screen with a reason on it. The rider asked for grey; silently showing
something else would hide a broken card.

## The manifest

A level gains two optional objects beside `pageImage`, with the same fields:

```json
"fit": {
  "cols": 1, "rows": 1,
  "pageImage":     {"assetId": "...", "nativeWidth": 1600, "nativeHeight": 960,
                    "rowBytes": 200, "rawLen": 192000,
                    "windowStepX": 240, "windowStepY": 160, "focalX": 0, "focalY": 0},
  "greyPlanes":    {"assetId": "...", "nativeWidth": 1600, "nativeHeight": 960,
                    "rowBytes": 200, "rawLen": 576000,
                    "windowStepX": 240, "windowStepY": 160, "focalX": 0, "focalY": 0},
  "greyPageImage": {"assetId": "...", "nativeWidth": 1600, "nativeHeight": 960,
                    "rowBytes": 400, "rawLen": 384000}
}
```

- `rowBytes` is the stride of **one row of one plane** for `greyPlanes`, and of
  one **2bpp row** for `greyPageImage`.
- `rawLen` is the **whole payload**: `rowBytes * nativeHeight * 3` for
  `greyPlanes`, `rowBytes * nativeHeight` for `greyPageImage`.
- The geometry of `greyPlanes` should equal the geometry of `pageImage` for the
  same level. Nothing refuses a difference -- each asset is checked against its
  own manifest entry -- but the viewer logs it, because the A/B is then not the
  same window of the same page (`WalletViewActivity.cpp:290-300`).

One parser context handles all three objects, with a pointer to the slot being
filled instead of three flags (`WalletManifestParser.cpp:167-180`). They are read
**for the requested level only**, same as `pageImage`, and an `assetId` that is
not 16 hex characters is no asset at all (`:236`).

A card with neither object behaves exactly as it did before P2b, and there is a
test that says so (`WalletGreyManifest.ACardWithNoGreyAssetsReadsExactlyAsItDidBefore`).

## Encrypted grey is the same crypto, more rows

No second path. A grey asset is an asset: `flags` bit 0, AES-256-CTR, IV =
`assetId || version || 0`, decrypt in place
(`docs/wallet-crypto.md`).

The window read is `readPlaneWindow()` (`WalletStore.cpp:370`), which is now
shared by the 1bpp page reader (`planeBase = 0`) and the grey reader (called once
per plane with that plane's payload offset). One implementation of the seek, the
read, the row stride and the CTR offset, so the two cannot drift.

The offset that matters: **the file offset includes the 32-byte cleartext header,
the CTR offset does not.** The keystream is indexed from the first byte of the
payload, and a plane's rows live at `planeBase + (y + r) * rowBytes + x / 8`.
Rows in the second and third plane are past the first 48,000 bytes, which is
where a naive implementation goes wrong; that is the one thing
`WalletGreyCrypto.EveryPlaneRowOfAWindowDecryptsAtItsOwnPayloadOffset` exists to
pin.

**No plaintext hash on a grey window**, exactly as on a 1bpp window: a window is
a fraction of the payload, so there is nothing to check it against. What catches
a wrong key is the manifest's GCM tag, which fails before any `assetId` is read.

## The switch

**Off by default, in every build.** Nothing about a card without grey assets
changes, and nothing changes for a build nobody switched.

Two ways to flip it, both hitting the same process-wide state
(`wallet::grey`, `WalletGreyPage.cpp:290`):

- **Hold CONFIRM** on the document screen for 700 ms (a press still cycles the
  level). Compiled in with the wallet's other test seams,
  `ENABLE_WALLET_TEST_CMDS` -- on in the dev environment, off in every release
  one. The viewer doc's argument against hold-to-anything applies to a rider's
  build; this is a lab build's affordance for a decision that has to be made by
  hand.
- **`CMD:WALLETGREY [on|off|toggle|status] [half|fast]`** (`src/main.cpp:1251`).
  A flip while a document is on the panel repaints it in place -- the screen
  consumes a repaint request in `loop()` -- so the same window can be seen both
  ways without leaving the screen.

The second argument is the **base frame's refresh mode**. HALF is what every
working grey path in this firmware uses and what `SleepActivity.cpp:227-231` says
is required; FAST is exposed only because "is a FAST base good enough" is an open
question that needs the panel rather than an argument.

### Driving the comparison from a host

The whole point of P2b is a person looking at the glass twice. The orchestrating
session has no hands, so:

```
CMD:GOTO_WALLET 0          # open the document
CMD:WALLETGREY off         # 1bpp
CMD:SCREENSHOT             # the 1bpp frame -- a real 1bpp page
CMD:WALLETGREY on          # QUEUES a repaint; the reply describes the frame BEFORE it
CMD:WALLETGREY status      # poll until repaint_pending=0, then read the numbers
CMD:SCREENSHOT_GRAY        # the grey frame itself, both planes (tools/greyshot.py)
```

**The order matters, and it bit us.** `CMD:WALLETGREY on` returns from the serial
handler; the repaint happens in the next `WalletViewActivity::loop()`
(`WalletViewActivity.cpp:79`) and takes ~2.6 s. So the reply to `on` **always**
describes the frame before the flip. Read `repaint_pending` and poll; the numbers
are stable once it is 0.

`CMD:SCREENSHOT` of a grey page is **a solid dark page**, and that is not a bug:
the framebuffer holds the base frame, where black and both greys are all ink
(`docs/eink-grayscale.md`, "Getting grey off the device").

## What a host can see of a grey frame

**Both defects this section documents were found on hardware on 2026-08-18 and
fixed on 2026-08-19.** Before the fix the status counter said `grey_rendered=0`
while the panel carried a grey frame, and `CMD:SCREENSHOT_GRAY` answered
`planes=0` for a page that was visibly grey -- so the only instrument that worked
was a human eye.

**The one sentence to keep:** after the fix a host can see, without touching the
panel, **whether a grey frame ever rendered** (`grey_frames`), **whether the glass
still carries it** (`grey_on_panel`, modulo a reboot), **what the last attempt
cost** (the `mode=grey` line) and **the frame's own plane bytes**
(`CMD:SCREENSHOT_GRAY`, which for the wallet re-reads them off the card); what it
still cannot see is **whether those bytes are the bytes the controller received**,
and **what the panel makes of them** -- both need the glass.

### The status line, and what each field is for

```
WALLETGREY_OK enabled=1 base=half grey_frames=3 grey_attempts=4 grey_on_panel=1 \
              last_outcome=ok repaint_pending=0 capture=1
WALLETGREY mode=grey card_base_us=... base_us=... ... total_us=... card_bytes=...
WALLETGREY mode=1bpp card_us=... refresh_us=... total_us=... card_bytes=...
```

| field | means | cleared by |
|---|---|---|
| `grey_frames` | grey frames finished since boot. **This is "did grey render?"** | nothing -- monotonic |
| `grey_attempts` | calls to `GreyPageReader::render()`, however they ended | nothing |
| `grey_on_panel` | the picture on the glass right now is that frame | the next BW frame, a failed attempt, leaving the screen -- and a reboot loses it while the panel keeps the picture |
| `last_outcome` | how the last attempt ended (`greyOutcomeName()`), `none` before any | -- |
| `repaint_pending` | a flip is queued and its frame is **not drawn yet** | the repaint being drawn |
| `capture` | `CMD:SCREENSHOT_GRAY` has a source and will send both planes | -- |

The `mode=grey` line is **the last frame that finished**, not the last attempt: a
failed attempt no longer zeroes the only measurement of a working one.

**What `grey_rendered` actually counted**, for the record, because the name was the
whole problem: it was `GreyTimings::rendered` of the **last attempt**, and
`render()` zeroes its timings struct on entry (`WalletGreyPage.cpp:177`). So one
capability refusal -- `NoScratch`, which draws nothing and is not the card's fault
-- erased every trace that grey had ever worked. On top of that it was read too
early, per the ordering note above. It carried neither of the two facts its name
implied. `wallet::GreyLedger` (`src/activities/wallet/WalletGreyStatus.h`) now keeps
them apart, and seven host tests pin the semantics.

### Capturing a grey wallet page

`CMD:SCREENSHOT_GRAY` works on a wallet grey page. It did not before 2026-08-19,
and the reason is the whole design of the channel: it replays the last
`GrayscaleFrame` **draw callback**, and the wallet has no callback -- the generator
baked the planes and `render()` streamed them from the card to controller RAM
without drawing a pixel.

`GrayscaleFrame` now takes a second kind of source, a **plane producer**
(`GrayPlaneSource`, `lib/GfxRenderer/GrayscaleFrame.h`), and
`GreyPageReader::render()` registers itself as one on success, remembering the
window it just streamed. A replay asks the producer for each band and gets the same
`readPlaneRows()` call the panel path made -- same window, same stride, same
decrypt offsets.

**Why a producer and not a buffered fallback.** The alternative was for
`CMD:SCREENSHOT_GRAY` to send whatever plane bytes the wallet last streamed, which
means keeping them: 96,000 bytes of DRAM on a device with ~380 KB and no PSRAM,
which is exactly the cost `GrayscaleFrame` was designed to avoid. A producer costs
**8 bytes of registration** and the 8 KB band the replay allocates and frees, and
it cannot drift from the panel path because it *is* the panel path's read. The
price is that a capture re-reads 96 KB off the card (~570 ms expected, unmeasured)
and needs the wallet still unlocked and the file still open. A locked session or a
pulled card makes the producer refuse **mid-dump**: the header already promised three
blobs, so a host gets the BW frame plus whatever bands got out and `greyshot.py`
says `truncated: N of 144000 bytes`. That is the honest outcome -- padding the rest
with zeros would hand a host a picture that was never on the glass -- and the reason
is in the device log (`grey capture band y=... refused: locked`), which prints once
the payload mute is lifted.

Rules it keeps:

- **Nothing refreshes.** A producer replay touches no framebuffer, no controller
  RAM, no waveform -- and unlike the callback path it does not even set a render
  mode or a strip target. It also must not call `waitRefreshComplete()`, which can
  make the SDK print to Serial mid-payload (`EpdBus.cpp:220`) and corrupt the dump.
- **The registration dies before the data does.** `close()`, the destructor and
  `ActivityManager::exitActivity` (`GrayscaleFrame::clearSource()`) all drop it, and
  every BW frame the viewer draws drops it too (`noteBwFrameOnPanel()`) -- once a
  1bpp page is on the glass, those planes are not what a host would be looking at.
- **A new grey render drops it first, re-arms last.** A pan re-registers with the
  new window; a failed attempt leaves nothing registered.

`tools/greyshot.py` in the parent repo needs **no change** -- the wire format is
untouched. What changes in its output for a wallet page: `planes=48000` instead of
`0`, `exact=1`, and a real 4-level PGM instead of the "nothing has rendered grey
since boot" message.

## What grey costs

The instrument is in the shipping path, not in a bench: every frame the viewer
draws logs one line, both modes, same shape
(`WalletViewActivity.cpp:339`, `:352`), and `CMD:WALLETGREY status` reprints the
last of each.

```
WALLETGREY mode=grey base=half card_base_us=... base_us=... card_planes_us=...
           planes_us=... nudge_us=... cleanup_us=... total_us=... card_bytes=...
WALLETGREY mode=1bpp card_us=... refresh_us=... total_us=... card_bytes=...
```

Card time and panel time are separate on purpose: the plan predicted card time
would be irrelevant next to the waveform, and this is what settles it.

**One frame has been through the panel.** On 2026-08-18 the device's own
`WALLETGREY mode=grey` line reported `total_ms=2604` with a 1,684 ms base
waveform -- instrument: that log line, read over serial by the session that found
the two defects above. That is a single frame of one window and the per-stage
numbers below are still expectations, not a measurement; a clean sweep (both
modes, several windows, HALF and FAST bases) is still owed.

What is known, and where it comes from:

| stage | expectation | where the number comes from |
|---|---|---|
| card, base plane window | ~283 ms | measured, 480 strided rows of a page image (`docs/wallet-viewer.md`, "Measured on the X4") |
| base refresh, HALF | ~1,720 ms | `Ssd1677Driver.cpp:48-50`, and `GrayscaleFrame`'s own comment |
| card, both planes | ~2 x 283 ms | same strided read, twelve bands instead of one window |
| plane streaming | unmeasured | 96,000 bytes over SPI to controller RAM |
| the nudge | short -- 3 active frame groups | `Ssd1677Luts.h:24-26` |
| cleanup | ~22 ms | `docs/eink-grayscale.md`, "Grey ladder" (planes 190 + nudge 144 + cleanup 22) |
| **1bpp comparison point** | ~283 ms card + ~500 ms FAST | measured, both (`docs/wallet-viewer.md`) |

So a grey frame is expected to land around **2.5-3 s against 0.8 s**, and the
waveform is not the only reason -- three windows instead of one is ~850 ms of
card time on its own. If that holds, grey is a **still-page** feature: fine for
looking at a document, wrong for panning, and the panning case may want the 1bpp
page even after grey wins on looks.

RAM and flash, measured by building both ways
(`pio run -e default`, this worktree):

| | before | after | delta |
|---|---|---|---|
| static RAM | 59,372 B | 59,420 B | **+48 B** |
| flash | 3,971,011 B | 3,976,631 B | **+5,620 B** |

The status ledger and the capture producer (2026-08-19), measured the same way
against the branch tip before them:

| | before | after | delta |
|---|---|---|---|
| static RAM | 59,420 B | 59,444 B | **+24 B** |
| flash | 3,976,691 B | 3,977,891 B | **+1,200 B** |

The 24 bytes are the ledger (two counters, three flags, one `GreyTimings`, minus
the two statics it replaced) plus 8 bytes of plane-source registration in
`GrayscaleFrame`. **No plane shadow**: a capture allocates the same 8 KB band the
render does, and frees it.

Plus, at run time: **8,000 bytes** of band scratch during a grey render only, and
~150 bytes for the second open reader inside the (heap-allocated) view activity.
No second framebuffer, no page-sized buffer, on either path -- same rule as the
rest of the wallet.

## Verification, and what it can and cannot prove

Host side, all green:

- **474 host tests** (444 before P2b, 17 for the grey layout, 10 for the status
  ledger and the capture path). They cover the encoding table against the LUT
  table, the 2bpp packing order, both strides, the two gates and every way they
  refuse, the plane offsets, the manifest keys, the encrypted row offsets, every
  transition of the status ledger, and the band walk the capture path performs --
  including that its three blobs decode, through `greyshot.py`'s own rule, to the
  levels the generator baked.
- **The two forms are decoded independently and compared pixel for pixel**, in
  the tests and in the preview. This is the only check that can catch a wrong bit
  order, because the device streams planes without looking at them.
- **The assertions were proved able to fail.** Three mutations, each reverted and
  re-run green:
  - `greyPlaneBit()`'s LSB case set to the MSB expression -- three tests failed and
    the preview reported `DISAGREE on 66800 of 384000 pixels` plus `WARNING one of
    the two greys is absent`;
  - `GreyLedger::noteAttempt()` returned to the old per-attempt semantics
    (`frames_ = 1` on Ok, zeroed otherwise) -- two `WalletGreyLedger` tests failed
    (`AFailedAttemptAfterAGoodFrameDoesNotUncountIt`,
    `ABwFrameTakesGreyOffTheGlassButNotOutOfTheCount`), which is the defect
    reproduced as a test;
  - the capture band walk reading rows at the window origin instead of
    `window + band + row` -- all three `WalletGreyCapture` tests failed.

  (The warning about a stale test binary is real -- the binary's mtime was checked
  against every run.)
- **`tools/greyshot.py` decodes the capture payload byte for byte.**
  `wallet_preview --grey` writes `<out>-capture.bin`, the three blobs exactly as
  `CMD:SCREENSHOT_GRAY` lays them out, and greyshot's own `split_blobs()` +
  `decode_levels()` were run against it off-device: 384,000 pixels, level shares
  30.34 / 17.40 / 34.50 / 17.77 %, **0 anomalies**, and the PGM it wrote is
  identical to `wallet_preview`'s own portrait PNG of the same window. So the layout
  the firmware would emit and the layout the host tool expects are the same layout.

The tools:

```
wallet_preview --synth-grey DIR                       # a synthetic grey tree
wallet_preview --tree DIR --grey --out PREFIX         # four levels as a PNG, plus
                                                     # PREFIX-capture.bin: the exact
                                                     # CMD:SCREENSHOT_GRAY payload
wallet_preview --tree DIR --grey --win-x 800 --win-y 480 --out PREFIX
wallet_preview --tree DIR --out PREFIX                # the 1bpp page, same window
```

`--synth-grey` exists because the layout had to be checkable before the
generator's grey output existed. It writes one page and one level -- the 1bpp page
image and both grey forms of one test pattern -- and it bakes them out of the
firmware's own encoding table (`test/wallet_preview/WalletGreySynth.h`), so it is
a reference for the layout rather than a second opinion about it. When the
generator's own grey tree arrives, this is what a disagreement gets diffed
against.

**Looked at, 2026-08-18** (`--synth-grey` at 1600x960, window 0,0, X4 panel):
the PNG shows the four levels as four distinct tones, the 2x2 quadrant pattern
exactly where the encoding predicts it (black, dark grey, dark grey, light grey),
the 2 px black page frame square rather than skewed -- which is what a wrong row
stride would have done to it -- and the 1/2/3 px lines of each level intact on
white. Level histogram 17.8 / 34.5 / 17.4 / 30.3 %, both mid tones present. The
planes and the 2bpp image agreed on all 384,000 pixels. The same window rendered
1bpp collapses dark grey to black and light grey to white, which is the A/B pair
the switch flips between.

**What only the panel can settle.** All of it, and this is the phase's whole
question:

- **Does grey read better than a 1bpp dither on a real document?** Human
  judgement, at arm's length, on the glass. Nothing on the laptop can answer it,
  and the two mid tones are exactly where the panel's own contrast decides
  (`docs/eink-grayscale.md`, "Grey scale" page).
- **What a grey frame actually costs**, per stage. One total came off the panel
  (2,604 ms, 2026-08-18); the per-stage split has not been read yet.
- **Whether a capture is bit-identical to what the controller got.** The producer
  makes the same `readPlaneRows()` call the panel path made, and the host tests say
  the band walk and the decode agree -- but code cannot diff itself against
  controller RAM. `CMD:SCREENSHOT_GRAY` against a photograph of the same page is
  what would settle it.
- **What a capture costs.** 96 KB re-read off the card plus three 48 KB serial
  blobs; ~570 ms of card time expected, unmeasured.
- **Whether a FAST base frame works.** `CMD:WALLETGREY on fast` versus `half`,
  same window, photographed. `SleepActivity` says a wrong base state gives
  blotchy greys; nothing here has tried it.
- **Whether panning in grey is usable at all** at the expected ~2.5 s a press.
- **Whether the grey page needs a different generator pipeline.** The 1bpp
  pipeline dithers (Floyd-Steinberg); the grey one quantises to four levels. On a
  photograph of a document, the honest comparison is dither-at-1bpp against
  quantise-at-4-levels, and a bad quantiser would lose a fair fight.
- **Grey residue after the wallet.** The first BW frame after a grey one is
  forced to HALF here; whether that is enough on a real page, or whether the
  frame after that still ghosts, has not been seen.
