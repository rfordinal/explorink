# Windowed / partial refresh on the T5 S3 Pro

How to make `displayWindow()` actually cost less than a whole-panel frame on
the LilyGo T5 S3 4.7" Pro -- for **speed** and for **battery**, which turn out
to need different changes (section 3b). Written 2026-09-07 from a read of our
own driver, of LovyanGFX's `Panel_EPD` (the engine we already run on this
board), and of the two projects that solved a version of this on the same
panel: **FastEPD** (bitbank2, <https://github.com/bitbank2/FastEPD>; notes in
[`fastepd.md`](fastepd.md)) and **OpenTrailPaper**
(<https://github.com/RaemondBW/OpenTrailPaper>; notes in
`docs/prior-art-opentrailpaper.md` in the parent repo).

Nothing here is built. Nothing here is measured on hardware except where the
line says so. **Every claim below survived an adversarial pass on 2026-09-07**;
where that pass corrected an earlier version of this doc, the correction is in
the text rather than in a footnote.

## 1. The window request dies in the driver base class

The whole chain from the map down exists and works:

```
MapActivity -> GfxRenderer::displayBufferWindow()   lib/GfxRenderer/GfxRenderer.cpp:1584
            -> HalDisplay::displayWindow()          lib/hal/HalDisplay.cpp:118
            -> FreeInkDisplay::displayWindow()      freeink-sdk/.../FreeInkDisplay.cpp:685
            -> PanelDriver::displayWindow()         freeink-sdk/.../driver/PanelDriver.h:53
```

`Ssd1677Driver` overrides that last call and drives only the requested
rectangle (`Ssd1677Driver.cpp:424`). **`LgfxEpdDriver` does not override it at
all** (`LgfxEpdDriver.h:28-53` -- no `displayWindow`, no
`supportsAsyncDisplay`). So the base implementation runs:

```cpp
virtual void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOff) {
  display(bus, fb, prev, RefreshMode::Fast, turnOff);   // PanelDriver.h:53-56
}
```

The rectangle is dropped on the floor and a whole-panel frame is pushed. Callers
exist and are not gated by board: `MapActivity.cpp:817`, `:1487`, `:1832`,
`:3019`, `:3038`, `:4668`, `:4686`, `:5080`, `:5188`, plus
`TileSyncActivity.cpp:1203`, `PreviewActivity.cpp:177`, `SleepActivity.cpp:287`.

Instrument for the cost: the device's own `PowerTelemetry` counters over one
4 h 36 min walk, read out of `power.csv` afterwards -- `panel_busy_ms` 2,944,634
over `ref_window` 2,608 window refreshes plus 29 whole-panel ones. **Two
different numbers get quoted off that walk and they are not the same number**:
the flat quotient is 2,944,634 / 2,637 = **1,117 ms**, while the widely-quoted
**1,081 ms** is the mean of seven per-segment figures which held between 1,049
and 1,101 ms. The 1,081 ms and its seven-segment spread are in
[`refresh-modes.md`](refresh-modes.md) under "The T5 S3 Pro is not the X4", on
this branch as well as on `develop`; the 1,117 ms quotient was first written down
here. Section 3b shows the spread is far wider on other sessions.

`PowerTelemetry::Refresh::Window` is incremented at the call site in
`lib/hal/HalDisplay.cpp:118-124`, before the driver sees anything, so it counts
**requests**, and nothing outside `HalDisplay` calls `einkDisplay.displayWindow`.
**So all 2,608 of those requests took the whole-panel path.**

One accounting caveat: `HalDisplay::displayBufferAsync` bills a whole-panel
refresh at 0 ms (`HalDisplay.cpp:97`) and expects the time to arrive later from
`waitRefreshComplete`. On this board `supportsAsyncDisplay()` is false, the
facade routes back to the blocking path, `_refreshPending` never becomes true,
and that later call returns ~0. Whether the walk's 29 whole-panel refreshes were
billed at all is **unverified**, and any claim that they were "under 1 % of the
total" rests on it.

**"Whole-panel push" means the data path, not the ink.** The CPU converts and
pushes all 518,400 pixels and the gate scan clocks every row, but `Panel_EPD`
still arms only pixels whose target changed (section 2b), so unchanged pixels
are not re-driven electrically. The waste is prep and scan time, not a
full-screen flash.

One doc in the parent repo needed fixing and has been fixed:
`docs/prior-art-opentrailpaper.md` said "we already have ... an explicit
`displayWindow(x, y, w, h)`". True of the X4, false of this board.

## 1b. The 2x survived every check, and nothing in the source explains it

Written 2026-09-09, off `power.csv` and the source, **no hardware involved**.
Three things were tested and the answers are not the ones the plan expected.

### The comparison is sound, and the gap is not a population artefact

The doubt was that the 1,030 / 545 ms pair mixed populations: the whole-panel
figure had **n=1** inside the one walk that produced the window figure, the
other 34 samples coming from 20 short boots that were mostly menu and home. So
the arithmetic was redone by hand, and then again over more data.

Method, unchanged from the one `develop` used and now independently reproduced:
`panel_busy_ms` is a single bucket for all four counters, so keep only the
one-minute rows in which **exactly one** refresh counter grew, and divide that
row's panel time by that counter's increment. Any mixed row charges its whole
panel time to whichever counter the reader divides by.

`docs/power-runs/run8-2026-09-08.csv` in the parent repo turns out to be the
card's whole history and a **superset of run7** -- its boots 13 and 27 are
run7's, byte for byte the same counters. So it is the file to read, and it more
than doubles both populations:

| file | `ref_window` | `ref_fast` |
|---|---|---|
| run7, isolated rows | n=374, median **1,029.7 ms** | n=35, median **545.2 ms** |
| run8, isolated rows | n=998, median **1,030.0 ms** | n=73, median **517.0 ms** |

**The gap survives.** It is not an averaging artefact and it is not a small
sample. Two further things say so:

- The window distribution is **tight and unimodal**: p5 1,013, p50 1,030, p95
  1,063, with 953 of 998 samples between 1,000 and 1,100 ms.
- The two distributions **do not overlap at all**. No whole-panel sample in
  1,071 reaches 700 ms; no window sample above the 3 % low tail falls below
  1,000. A mixture of one operation doing cheap and expensive work would
  overlap. This does not.

### Both counters bracket byte-identical work

`LgfxEpdDriver::display()` opens `(void)prev;` and then does exactly
`fillCanvasBW(fb)` plus `pushCanvas(epdModeFor(mode))`
(`freeink-sdk/.../driver/LgfxEpdDriver.cpp`, `display`). The base
`PanelDriver::displayWindow` calls `display(..., RefreshMode::Fast, ...)`. A
whole-panel `FAST` calls the same function with the same mode. So the two
buckets time **the same function with the same argument**, and the rectangle
never reaches the driver at all.

Worse for the comparison, the surrounding facade code makes the whole-panel
bracket the **larger** one: `FreeInkDisplay::displayBuffer` additionally runs
`resolveReleasedMode`, `consumePrevFrameFor` and `swapBuffers`, none of which
`displayWindow` runs. Every source-level difference found points the wrong way
-- the path with more work in it is the one measuring half the time.

One thing does differ, and it is a reason the two buckets are not comparable by
construction rather than an explanation of the 2x: `GfxRenderer::displayBuffer`
runs `takeCleanRefreshMode()`, which promotes a pending clean `FAST` to `HALF`
(`lib/GfxRenderer/GfxRenderer.cpp`, `takeCleanRefreshMode`). `HalDisplay` then
counts that as `ref_half`, not `ref_fast`. `displayBufferWindow` skips the
promotion entirely -- a window cannot spend a clean request (section 6). So the
cheap bucket has had its expensive members moved out of it, into a third
counter, and the expensive bucket has not.

**So the 2x is `[open]`, and it is open in a stronger sense than before**: not
"we have not looked", but "the source permits no mechanism of the right sign".

### The one mechanism the source does permit, as arithmetic only

Section 2b: an unchanged frame arms nothing, so the task runs one empty pass
instead of eleven. That is the only lever in the code that changes a refresh's
cost by a large factor without changing the code. Fitting the two observed modes
to `cost = fixed + passes * per_pass` with 1 and 11 passes gives:

- **per pass ~52 ms**, **fixed ~455 ms**
- and it predicts a 37-pass `epd_text` clean frame at **~2,390 ms**

`[derived, unverified]`. It replaces nothing. It is written down because it is
falsifiable in one afternoon by the unchanged-frame subtraction in section 3,
and because it is the first per-pass figure in this doc that does not rest on a
PSRAM bandwidth number that exists nowhere in the tree. It also has a known
weakness: the lowest whole-panel sample observed is 274 ms, below the fitted
455 ms floor, which a refresh straddling a one-minute row boundary would explain
and which the fit does not.

### Lead A is dead: `syncPendingAsync()` is free on this board

The suspicion was that a window refresh waits out a previous async refresh
inside its own timing bracket. It cannot.

`LgfxEpdDriver` overrides **neither** `displayWindow` **nor**
`supportsAsyncDisplay` -- zero occurrences of either in its header or its
implementation -- so `PanelDriver::supportsAsyncDisplay()`'s `return false`
stands. `FreeInkDisplay::displayAsyncImpl` then hits
`if (!_driver->supportsAsyncDisplay()) { displayBuffer(...); return; }`
**before** it reaches `syncPendingAsync()` and before `_refreshPending` can be
assigned. Nothing else on this board sets that flag. So
`syncPendingAsync()` returns at its own first line and costs nothing.

### But that same gate was hiding real panel time, and now it is counted

The accounting caveat section 1 marked `[unverified]` is **verified, and it was
a defect.** `HalDisplay::displayBufferAsync` billed the refresh at 0 ms on the
theory that the time would arrive later from `waitRefreshComplete()`. On a
driver that never defers, the whole refresh completes **inside** that
0-ms-billed call and the later wait returns immediately, so the time left the
log entirely -- it was not misattributed, it was lost.

It does not contaminate run7 or run8: the async path has exactly one caller
tree, the EPUB reader (`src/activities/reader/ReaderUtils.h`), and those files
are map and BLE boots. It is fixed anyway, and an `async_inline` counter now
makes the assumption observable instead of assumed -- expected 0 on a deferring
driver, equal to the async call count on every `LgfxEpdDriver` board.

### A second instrument defect, not previously written down

**The entire grayscale panel path is untimed and uncounted.**
`HalDisplay::displayGrayscaleBase`, `displayGrayBuffer`,
`preconditionGrayscale`, `writeGrayscalePlaneStrip` and
`cleanupGrayscaleBuffers` forward straight to the facade with **no
`POWER_TELEMETRY` call at all** (`lib/hal/HalDisplay.cpp`). On this board
`displayGray` runs a real `pushCanvas`, i.e. a whole-panel refresh.

So `panel_busy_ms` under-reports total panel time, and it under-reports a map
session -- which uses grey overlays for anti-aliased text -- by more than a
menu. That matters for section 3b's 5 % to 21 % duty table, which is therefore
a **floor**, not a measurement of the panel path's share.

### What was built for this

`t5s3-refresh-instr`, off `fix/popup-confirm-ring` (which contains
`release/lilygo-t5-s3-pro` entirely: 146 ahead, 0 behind). Unflashed, untested
on hardware.

- `micros()` instead of `millis()` around every refresh, and a **per-waveform**
  busy total, so no future reader has to throw away 90 % of the rows to
  separate two waveforms. New columns `busy_full_ms`, `busy_half_ms`,
  `busy_fast_ms`, `busy_win_ms`.
- **Window requests split by call site** (T-277), which is what makes
  `ref_window` readable: `win_marker`, `win_overlay`, `win_status`,
  `win_chrome`, `win_other`. All twelve `displayBufferWindow` callers in
  `MapActivity` are tagged.
- `async_inline`, above.
- The false comment at `HalDisplay::displayWindow` fixed: a window on this
  board is not "neither a full frame nor free", it **is** a full frame.

Not built, and deliberately: the intra-refresh phase brackets around
`fillCanvasBW`, `pushSprite` and `waitDisplay`. All three live inside
`freeink-sdk`, which CLAUDE.md keeps as a mirror of `Free-Ink/freeink-sdk`
carrying almost nothing of ours, so they need that fork's own branch and PR
rather than a local edit. Sections 3 steps 2 and 3 are the measurements that
matter more anyway, and neither needs an SDK change.

## 2. What `Panel_EPD` already gives us

The board runs LovyanGFX's `Panel_EPD`, bundled inside **M5GFX 0.2.28**, pinned
at `platformio.ini:322`
(`.pio/libdeps/t5s3pro/M5GFX/src/lgfx/v1/platforms/esp32/Panel_EPD.cpp`, read
2026-09-07). It is not a dumb blitter. It is a per-pixel state machine, and it
already does most of what we would otherwise write ourselves.

**Vocabulary, used below.** A *pass* is one whole-panel scan: every row clocked
out once. A *LUT* here is a table of passes -- one row of the table per pass, one
column per grey level, each cell saying `1` drive to black, `2` drive to white,
`3` do nothing, `0` end. *Arming* a pixel means recording a new target for it in
the step framebuffer, so the next passes drive it. FastEPD's wiki makes the
naming point: this is not a "waveform", it is a fixed list of digital steps
([`fastepd.md`](fastepd.md), 2a).

### 2a. Three buffers, all in PSRAM

| buffer | what | size at 960x540 |
|---|---|---|
| `_buf` | the panel's own 4bpp image | 259.2 kB |
| `_step_framebuf` | 2x `uint16_t` per pixel: current step + reserved next | 1,036.8 kB |
| our `g_canvas` `LGFX_Sprite` | 8bpp copy we fill and push | 518.4 kB |
| our `g_lsb` + `g_msb` | 1bpp grey overlay planes | 129.6 kB |

`Panel_EPD::init_intenal()` allocates the first two (`Panel_EPD.cpp:232`,
`:235`); `LgfxEpdDriver::allocCanvas()` allocates ours
(`freeink-sdk/.../driver/LgfxEpdDriver.cpp:133-147`). **About 1.94 MB of PSRAM
for one panel**, and the sprite is a pure duplicate of what `_buf` already
holds.

### 2b. The diff is per pixel and it is the driver's own model

`task_update()` (`Panel_EPD.cpp:895`) walks the requested rectangle and arms a
pixel only when its new target differs from what is already recorded:

```cpp
if (d1 != s0) { d[1] = s0; d[0] = s0 - 0x8000; }   // Panel_EPD.cpp:948-951, fast path
```

Pushing an unchanged frame arms nothing. This is the same property epdiy has and
it has the same consequence: **a re-push cleans nothing**. Only a mode that
inserts the eraser LUT resyncs the glass.

**Except in `epd_text` and `epd_quality`, where the diff is much weaker.** Those
branches arm on `white != d1 || d1 != s0` (`:974`, `:1006`) -- every non-white
pixel is re-armed whether it changed or not. A "clean" frame over a hatched map
therefore rewrites most of the step framebuffer rather than a diff, and since
our `Full` and `Half` both map to `epd_text` (`LgfxEpdDriver.cpp:105-111`), that
is what every clean frame on this board does.

`_step_framebuf` is the driver's model of what is on the glass. Our `g_canvas`
sprite is a private copy of the **input frame**, not of that model -- harmless
only because we refill all of it from the 1bpp frame on every push. The moment
we scope the fill, it becomes stateful, and OpenTrailPaper's shadow-drift trap
is live.

### 2c. The rectangle scopes the CPU work, never the scan -- on this bus

`display(x, y, w, h)` (`Panel_EPD.cpp:553`) accumulates the rectangle into
`_range_mod` and queues it; the arguments are only OR-ed in (`:555-569`) and
`_range_mod` is reset after a successful queue send (`:588-593`). `task_update`
arms only rows `y .. y+h` and columns `x .. x+w`. Then it scans:

```cpp
for (uint_fast16_t y = 0; y < mh; y++) { ... bus->writeScanLine(...); }   // :1048-1060
```

Unconditional, every row, every pass, whether anything is armed or not.

**Three S3 drivers do this and one older driver does not.** FastEPD's
`bbepPartialUpdate` clocks every row ([`fastepd.md`](fastepd.md), 4a), epdiy's
S3 LCD path clocks every row with no-op rows (`render_lcd.c:99-103`), and
`Panel_EPD` clocks every row. But **epdiy's older ESP32 I2S path skips rows
outside the band with a bare CKV pulse and no data** (`render_i2s.c:72-86`,
`pulse_ckv_ticks(45, 5, false)`, with a note about the panel's 200 kHz CKV
limit). **Verified against epdiy `main` on 2026-09-07**, and the detail matters:
the first two consecutive skipped rows are still clocked with a zeroed buffer,
and only the third onward takes the bare pulse (`render_i2s.c:72-87`). The LCD
path memsets every out-of-area row instead and clocks it
(`render_lcd.c:99-103`). So the gate scan *can* be shortened in principle -- it
is a property of the frame-DMA approach these three share, not of the panel. **Settled for our
stack, not for the panel**, and worth remembering if the scan ever turns out to
be the wall.

### 2d. Pass count: two more than the table rows suggest

Counted off the arrays (`Panel_EPD.cpp:82-156`, and
`freeink-sdk/libs/hardware/BoardT5S3/src/LilyGoT5S3LgfxConfig.cpp:26-38`). A
`~0u` row is all-`3`, an idle settle pass; `0u` ends the table.

**Counting table rows undercounts by two.** The `0u` terminator row costs a whole
pass of its own -- `blit_dmabuf` handles it inside a scan
(`Panel_EPD.cpp:735-741`, C version `:834`) and still returns non-zero, so
`remain` stays true -- and the loop then runs one more entirely empty pass
before it clears `remain` and drops the rails (`:1048-1068`).

| mode | LUT in our build | drive | idle | terminator | trailing empty | **passes** |
|---|---|---|---|---|---|---|
| `epd_fast` | our `kFastLut` | 8 | 1 | 1 | 1 | **11** |
| `epd_fastest` | our `kFastLut` (aliased) | 8 | 1 | 1 | 1 | **11** |
| `epd_text` | LovyanGFX default + eraser | 2+12 | 1+19 | 1+1 | 1 | **37** |
| `epd_quality` | LovyanGFX default + eraser | 2+15 | 1+16 | 1+1 | 1 | **37** |
| default `lut_fastest`, if un-aliased | 5 | 1 | 1 | 1 | **8** |

The eraser is prepended only in the non-fast branches: the fast branch writes
`d[0] = s0 - 0x8000` directly (`:948-956`, its comment says so), while text and
quality knock `d[0]` down to its low 8 bits, which is the eraser LUT at offset 0
(`:979-981`, `:1011-1013`).

Pixels do **not** march in lockstep -- each carries its own cursor into the LUT
(`d[0]`, advanced by 256 per pass) -- so passes per frame is the maximum
remaining steps over armed pixels, plus the trailing empty one.

`LgfxEpdDriver::epdModeFor()` maps `Full` and `Half` to `epd_text` and
everything else to `epd_fast` (`LgfxEpdDriver.cpp:105-111`). So a clean frame on
this board is **37 passes against a fast frame's 11**, it runs a LUT the board
never tuned, and (2b) it arms nearly everything.

Two knobs nobody has turned: `epd_fastest` is aliased to `kFastLut`, so asking
for it buys nothing today -- un-aliasing it to the library's own five-drive-pass
table would take a marker move from 11 passes to 8, not to 5, because the two
structural passes are paid by every mode. And `lutQuality` / `lutText` are
`nullptr` in our config (`LilyGoT5S3LgfxConfig.cpp:157-181`), so the clean path
runs an untuned default.

### 2e. The scan already runs in its own task, and we throw that away

`init_intenal()` starts `xTaskCreatePinnedToCore(task_update, "epd", ...)`
(`Panel_EPD.cpp:293`). `task_pinned_core` defaults to `-1` in a `uint8_t`, so it
is 255, fails the `>= portNUM_PROCESSORS` test, and becomes
`(xPortGetCoreID() + 1) % 2` -- genuinely the other core from whichever ran
`init()`. The panel scan is already asynchronous.

`LgfxEpdDriver::pushCanvas()` then does:

```cpp
g_dev.waitDisplay();          // LgfxEpdDriver.cpp:182-188
g_dev.setEpdMode(epdMode);
g_canvas->pushSprite(0, 0);
g_dev.waitDisplay();          // <- blocks the caller for the whole scan
```

and never overrides `supportsAsyncDisplay()`, so the facade's async path is off
on this board. `refresh-modes.md` records the consequence: the map loop spends
up to a quarter of its wall clock inside a blocking panel call. **The
asynchrony is already paid for and we do not collect it.**

## 3. Cost model: what scoping actually buys

Per whole-panel frame today, in order:

1. `fillCanvasBW()` -- 518,400 PSRAM byte writes, bit-extracted one at a time
   (`LgfxEpdDriver.cpp:149-160`). Scales with area.
2. `pushSprite(0,0)` -- 518,400 pixels through `Panel_EPD::writeImage` and
   `_draw_pixels`, Bayer-dithered into `_buf` (`Panel_EPD.cpp:375`, `:467`).
   Scales with area.
3. arming loop in `task_update` -- reads 259.2 kB of `_buf`, reads and writes
   1,036.8 kB of `_step_framebuf`. Scales with area.
4. **11 passes.** Each one reads the **whole** 1,036.8 kB step framebuffer
   through `blit_dmabuf` and clocks 540 x 248 = 133,920 bytes.
   **Does not scale with area.**
5. one `vTaskDelay(1)` per pass (`:1063`, tick 1 ms) -- ~11 ms, and a full
   TPS65185 rail up/down cycle per frame (section 3b). Neither scales with area.

### The bus is not the wall, and the bus time is not additive

Bus arithmetic, **derived and now checked against the IDF on disk**:
`write_len = memory_w/4 + line_padding` (`Panel_EPD.cpp:901`), 960/4 + 8 = 248
bytes x 540 rows = 133,920 bytes. 16 MHz is exactly achievable --
`LCD_CLK_SRC_PLL160M` (`Bus_EPD.cpp:128`) with a forced /2 to 80 MHz, then
`pclk_prescale = 5` -- and an 8-bit bus sets `lcd_2byte_en = 0`, one byte per
clock. So **8.37 ms of pure clocking per pass, ~92 ms per fast frame.**

Add per-row overhead: every row is its own i80 transaction with an ISR turnaround
(`esp_lcd_panel_io_i80.c:567-571`, `:707-729`, including an
`esp_rom_delay_us(1)`). Estimated 5-10 us per row on top of 15.5 us of data puts
a realistic pass at **11-14 ms**, a fast frame at **120-150 ms** of bus.

**But that time cannot be subtracted from the total.** `_dma_bufs` is a
ping-pong pair (`:1049`): `blit_dmabuf(y)` runs while row `y-1` transmits, and
`writeScanLine` waits on `_bus_busy` only just before sending
(`Bus_EPD.cpp:103`). A pass costs roughly `540 x max(blit, tx + overhead)`. If
the blit is slower than 15.5 us per row, the bus time never appears as a term at
all.

**So the earlier headline "93 % is CPU, not panel" was wrong as phrased** and has
been removed. The defensible statement is narrower: **the panel's electrical
clocking is small; the floor that scoping cannot touch is 11 full passes over a
1 MB step framebuffer, plus ~11 ms of ticks, plus one rail cycle.**

### What a small window would actually cost `[derived, unverified]`

Per pass, the cost is `max(1,036.8 kB / PSRAM read bandwidth, ~11-14 ms of bus)`.
The S3 here runs octal PSRAM at 80 MHz with a 32 kB data cache, and `blit_dmabuf`
strides it in 32-byte lines, so this is miss traffic end to end -- **11.4 MB of
PSRAM reads per fast frame.** At 40-60 MB/s that is 17-26 ms per pass; at
25 MB/s, 41 ms. **No measured PSRAM bandwidth figure exists anywhere in this
tree**, so that range is an estimate and nothing else.

Eleven passes gives **190-450 ms**, plus ~11 ms of ticks and ~10-30 ms of rail
sequence: a floor of roughly **210-500 ms, centred near 300 ms**, against a
measured 1,081-1,117 ms. **A 2x to 5x win, and the spread is set by PSRAM
bandwidth, not by the panel.** Steps 1-3 are what scoping removes; steps 4 and 5
are what remains no matter how small the rectangle.

### Measured on hardware, 2026-09-09. The scan is 34 ms a pass and 85 % of it is `blit_dmabuf`

**This replaces the estimates above.** Instrument: a pass counter and two
`micros()` brackets inside `Panel_EPD::task_update` -- one round the whole pass,
one round `blit_dmabuf` alone -- in a **scratch copy** of
`.pio/libdeps/t5s3pro/M5GFX/.../Panel_EPD.cpp`, printing one `[EPDPASS]` line
per frame when `remain` clears. Branch `t5s3-refresh-instr` at `09c19901`,
flashed to the **LilyGo T5 S3 Pro** (ESP32-S3 QFN56 rev v0.2, 8 MB PSRAM, MAC
`7c:2c:67:8a:4c:b4`, confirmed by `esptool chip-id` before the flash). 23 frames
over four runs, driven from the host with `CMD:GOTO_MAP`, `pos` and `redraw`.
Binary and ELF archived as
`docs/firmware-builds/t5s3-refresh-instr-09c19901-passcounter-measured.*`.

| passes | n | frame | per pass | `blit_dmabuf` | blit share |
|---|---|---|---|---|---|
| **1** | 1 | **23.3 ms** | 23.3 ms | 19.2 ms | 82 % |
| **11** | 19 | **366 ms** | 33.3 ms | 287 ms | 84 % |
| **37** | 3 | **1,253 ms** | 33.9 ms | 1,118 ms | 89 % |

Medians. Every figure is time inside `task_update`, i.e. the scan only.

**Section 2d's pass-count table is confirmed exactly.** 11 for `epd_fast`, 37
for `epd_text`, and a 1-pass frame exists where nothing arms -- all three
predicted values observed, including the two structural passes 2d argued for
from the arrays. The frames are also deterministic to the microsecond: the same
37-pass boot frame measured 1,253,195 and 1,253,196 us on two separate runs.

**The scan is purely linear in pass count and has no fixed cost of its own.** A
straight line through the 1-pass and 37-pass medians gives **34.16 ms per pass
and an intercept of -10.8 ms**, i.e. zero. So there is no arming sweep or rail
cycle worth naming inside the scan; a frame's scan time *is* its pass count
times 34 ms.

**`blit_dmabuf` is 82-89 % of that.** The structural suspect this doc named
before anyone measured it is the answer: every pass re-reads the whole
1,036.8 kB step framebuffer, and that read is where the refresh goes. It is also
the term section 3b predicted scales with nothing, so it is paid 11 times on a
marker move and 37 times on a clean frame.

**Against the reference (section 4): 33.8 ms against EPD_Painter's ~5.6 ms of
row sending per pass, a factor of 6.0.** Same panel, same every-row scan. Their
whole 13-pass 4-level frame, measured with their own profiler, is ~125 ms; our
11-pass scan alone is 366 ms.

**What this refutes.** The estimate above -- 17-26 ms per pass at 40-60 MB/s of
PSRAM, 190-450 ms for a fast frame -- lands close on the frame total (366 ms
measured against 190-450 estimated) but for the wrong reason: the real per-pass
cost is 34 ms, above the top of the estimated range, and the 210-500 ms "floor
centred near 300 ms" was never a floor at all, because the intercept is zero.
Section 1b's two-mode fit off `power.csv` is refuted in its details too: it read
455 ms of fixed cost and 52.3 ms per pass, where the truth is **zero fixed cost
inside the scan, 34 ms per pass, and the 455 ms it saw was prep sitting outside
`task_update` entirely** (see below).

### Prep is outside the scan, and it is the other half `[measured]`

Pairing each frame's `[EPDPASS]` scan time against the same refresh's wall clock
from `PowerTelemetry` (`stats` over the serial console, `panel_busy_ms` delta):

| what | counter | wall clock | scan | **prep** |
|---|---|---|---|---|
| `redraw`, whole-panel `FAST` | `ref_fast` +1 | **488 ms** | 254 ms | **234 ms** |
| `pos`, one marker move | `ref_window` +1 | **1,048 ms** | 474 ms | **574 ms** |

Prep is `fillCanvasBW` plus `pushSprite`, which run before the panel task is
handed anything. So a marker move on this board is roughly **half prep, half
scan**, and neither half is the panel.

**And this reproduces the field 2x on the bench: 1,048 ms against 488 ms, where
`power.csv` gave 1,030 against 517.** Both bench frames were **11 passes**,
which settles what section 1b could only argue from the source: the window and
whole-panel counters do time the same operation, and the 2x is not windowing.

**What the 2x actually is, `[open]` but narrowed.** Both halves roughly doubled,
and prep is a pair of fixed-size loops over 518,400 pixels that cannot vary with
content -- so the only thing that can double it is the CPU clock. The device
reports `cpu_mhz=80`, the low-power floor, with `throttled_ms` at 226 s against
`full_clock_ms` at 10 s, and the log shows the clock going up and down around
commands:

```
[134394] [DBG] [PWR] Restoring normal CPU frequency
[135051] [DBG] [PWR] Going to low-power mode (80 MHz)
```

The cheap `redraw` fell inside such a window: `full_clock_ms` grew by 658 ms
across the snapshot that contained it, and by **0 ms** across the one containing
the dear `pos`. That is consistent and it is the leading explanation for the
prep half.

**It does not explain the scan half, and that part stays open.** The two scan
populations are 474 ms at 60 % blit and ~314 ms at 85 % blit, with
`blit_dmabuf` itself almost unchanged between them (286 ms against 268 ms) --
so the variable is the **non-blit** part of the pass, 188 ms against 46 ms, and
a slower core would have moved the blit too. Bus contention or scheduling, not
clock. **What would settle both:** pin the clock (the map already claims to do
this while BLE is up, `PowerTelemetry.h`) and re-run the same `redraw` / `pos`
pair, with the field's own caveat in view -- the 2026-09-07 walk had `ble=2` in
246 of 249 rows, so it should have been at full clock and still measured
1,030 ms.

### What a real partial refresh would actually save, now that the scan is measured

**Correcting this doc's own framing.** Section 2c said the rectangle "scopes the
CPU work, never the scan", and section 3b concluded scoping is "mostly a speed
fix" that "does not shorten rail-on time by one microsecond". Both are still
true as written, but they left the impression that a windowed refresh cannot
touch the scan. **It can, and after the 2026-09-09 measurement it is the second
biggest lever we have.**

Three facts, the first two verified in upstream source on 2026-09-09 (epdiy at
`42c1612`, cloned and read here), the third read off `Panel_EPD` in our own
`.pio/libdeps`:

- **The glass can be advanced without data.** epdiy's older ESP32 I2S path
  skips a row with a bare CKV pulse and no transmission at all --
  `pulse_ckv_ticks(45, 5, false)` (`src/output_i2s/render_i2s.c:79-86`), with a
  comment citing the OC4 spec's 200 kHz CKV ceiling. So a short scan is not
  forbidden by the panel.
- **On the S3 it is forbidden by the peripheral, not the panel.** epdiy's S3 LCD
  path sends every row and only zeroes the data outside the band -- its own
  comment is `// Output no-op outside of drawn area`, then
  `memset(buf, 0, ctx->display_width / 4)`
  (`src/output_lcd/render_lcd.c:98-104`). The LCD peripheral generates its own
  frame timing and is fed line by line, so the row count is fixed by its
  configuration. `Panel_EPD` and FastEPD do the same (2c).
- **`Panel_EPD` already scopes the arming and does not scope the scan.** The
  arming loop works off `new_data.x/y/w/h` (`Panel_EPD.cpp:924-945`), but the
  scan loop runs `for (y = 0; y < mh; y++)` where
  `mh = (memory_height + magni_h - 1) / magni_h` (`:907`) -- the whole panel,
  unconditionally.

**And the row send is not the expensive part.** Measured: `blit_dmabuf` is
82-89 % of a pass and the bus is the rest. `blit_dmabuf` is called **per row**,
with that row's slice of the step framebuffer
(`&me->_step_framebuf[(y * memory_w >> 1) * 2]`). So the megabyte is read a row
at a time, and a row outside the band does not need reading at all -- it needs a
`memset`, exactly as epdiy does it.

So the shape of a real partial refresh here is: **keep clocking all 540 rows,
because the peripheral requires it, and skip the step-framebuffer read for rows
outside `_range_mod`.** Rough arithmetic on the measured numbers, for a
100-row marker band out of 540:

| | today | band-scoped blit |
|---|---|---|
| blit per frame (11 passes) | 286 ms | ~53 ms |
| bus per frame | ~46-188 ms | unchanged |
| **scan per frame** | **366 ms** | **~130-240 ms** |

`[derived from the measured per-row blit share, not yet built or measured]`.

**Two independent levers, and they compose.** Pass count (T-273) scales the
whole frame and costs image quality, because a shorter table delivers less dose
-- EPD_Painter measured that directly and its 7-pass fast train cannot express
16 greys at all (section 4). Band-scoped blit scales with area and costs no
image quality whatsoever, because it changes which bytes are read and not what
reaches the glass.

**What it does not do.** It does not reduce the pass count. Pass count is
independent of area: each pixel carries its own cursor into the LUT and a frame
runs as many passes as the longest remaining table among armed pixels, plus the
trailing empty one (2d). A one-pixel change still runs the full 11. So "it is
only a small update, send fewer passes" does not follow, and nothing in the
driver could make it follow.

**Consequence for T-271.** Its premise was wrong in the way section 1b and
section 4 describe -- the rectangle never reaches the driver, and the project
hitting ~125 ms on this panel does not scope at all. But the *work* T-271 stands
for is worth more than this doc previously said, because the win is inside
`Panel_EPD`'s scan loop rather than in `LgfxEpdDriver`'s override. That makes it
an upstream or fork change to M5GFX, not a change to our driver, which is a
different and larger piece of work. **Written down rather than started.**

### The two measurements that settle it

The obvious instrument -- `micros()` around `fillCanvasBW`, `pushSprite` and
`waitDisplay` -- separates prep from scan, and that is worth having, but it
cannot see the pass count or the bus share inside the scan. Two better ones:

- **No code change at all.** Push a byte-identical frame. Nothing arms
  (`:948`), so the task does an arming sweep, **one** empty pass, and a rail
  cycle. The difference in `panel_busy_ms` between a frame with one changed
  pixel (11 passes) and an unchanged one (1 pass) is the cost of ten passes
  directly, with `fillCanvasBW` and `pushSprite` cancelling out of the
  subtraction.
- **In a scratch copy of `Panel_EPD.cpp`**, a `micros()` bracket and a pass
  counter around `:1044-1065`, printed when `remain` clears. Confirms 11 vs 37
  and gives a per-pass time. A second bracket around `blit_dmabuf` alone, over
  540 unarmed rows with no `writeScanLine`, isolates the PSRAM sweep; the
  difference against the pass time is the real bus exposure.

## 3b. Power, which is a different question with a different answer

Speed and battery are the two reasons this work exists, and **they are not
bought by the same change.** Five consumers in one refresh, each scaling with
something else:

| consumer | scales with | cut by |
|---|---|---|
| TPS65185 rail cycle -- `epdPowerOn()` / `epdPowerOff()` | **number of refreshes** | fewer refreshes (T-274), or amortising a burst |
| rails up through the scan (the -15/+22 V charge pumps) | **number of LUT passes** | shorter tables (T-273) |
| ink actually moved | number of **changed pixels** | already differential in fast mode; not in `epd_text` (2b) |
| MCU + PSRAM prep (fill, push, arm) | **rectangle area** | scoping (T-271, T-272) |
| `blit_dmabuf` inside each pass | **nothing** -- reads the whole 1,036.8 kB step framebuffer every pass | fewer passes (T-273) |

**So scoping the rectangle is mostly a speed fix, not a power fix.** It removes
MCU and PSRAM work. It does not shorten rail-on time by one microsecond, because
the gate scan clocks every row whatever the rectangle says (section 2c). The
power items in the plan are **T-273** (pass count: 11 today, 8 with a real
`epd_fastest`, and 37 on a clean frame running an untuned default) and **T-274**
(how often a refresh happens at all).

### The rail cycle is not free, and partial refresh tempts you into more of them

`epdPowerOn()` is six PCA9535 expander writes over I2C, a `delay(1)`, a poll of
`TPS_PWR_GOOD` with a 400 ms ceiling, a TPS enable, a VCOM register write, and a
second ready-poll with another 400 ms ceiling
(`freeink-sdk/libs/hardware/BoardT5S3/src/LilyGoT5S3LgfxConfig.cpp:106-151`).
`task_update` runs that on the way into a refresh (`Panel_EPD.cpp:1044`) and
drops the rails the moment no pixel has steps left (`:1067-1068`). Estimated
10-30 ms per cycle from the TPS65185's default power-up sequencing; **not
measured, and the code never programs UPSEQ.**

That cost is **per refresh and flat**. It ignores the rectangle and it ignores
the pass count. **The trap is the obvious one: once a window refresh is cheap,
the temptation is to do many more of them, and a hundred cheap refreshes can
cost more energy than ten expensive ones.** Whatever comes out of T-271 has to
be judged against refresh *count*, not only against per-refresh milliseconds.

There is a knob nobody has turned: `Bus_EPD::powerControl` is already
state-guarded, so holding the rails up across a burst of marker moves would
amortise one rail cycle over several frames. That runs straight into the vendor
guidance in [`refresh-modes.md`](refresh-modes.md), "What each mode leaves the
panel's rails doing" (BUG-023, T-515 in the parent repo), which says a panel
left powered can be damaged. And FastEPD is explicit that the rails must not be
cut without a neutral pass first (`// This clear to neutral step is necessary;
do not remove`). `task_update` runs no explicit neutral pass before
`powerControl(false)`; whether its trailing all-`3` idle pass is the electrical
equivalent is **unverified** -- nobody has read `_lut_2pixel` to see whether `3`
means floating or skip. **Open, and it needs a measurement, not an argument.**

### How much of the wall clock the panel path actually owns `[measured]`

Off `docs/power-runs/run6-2026-09-04.csv` in the parent repo, build
`0.2.0-t5s3pro`, seven boots longer than fifteen minutes:

| boot | length | panel busy | refreshes (window / fast / half) | mean per refresh |
|---|---|---|---|---|
| 35 | 3,123 s | **21.3 %** | 1205 / 95 / 3 | 510 ms |
| 37 | 1,741 s | 11.4 % | 278 / 77 / 0 | 559 ms |
| 40 | 11,541 s | 12.3 % | 1412 / 159 / 3 | 905 ms |
| 54 | 1,981 s | 2.6 % | 35 / 3 / 9 | 1,113 ms |
| 59 | 2,945 s | 11.1 % | 285 / 57 / 7 | 934 ms |
| 60 | 18,932 s | 4.8 % | 867 / 40 / 5 | 991 ms |
| 43 | 2,521 s | 0.1 % | 0 / 3 / 1 | 764 ms |

**The panel path owns 5 % to 21 % of wall clock on a real session.** That is the
number that makes this worth doing at all, and it is a counter rather than a
voltage, so it is trustworthy.

It also corrects the framing above: neither 1,081 nor 1,117 ms is a constant.
The same build spans **510 to 1,113 ms** per refresh across boots, a factor of
two. The mechanism is in the code: an unchanged frame arms nothing and costs one
empty pass plus a rail cycle instead of eleven passes, so the mean is a function
of how many pushes were genuine redraws. The unchanged-frame measurement in
section 3 turns that from an explanation into a number.

### What the existing telemetry cannot answer, and what would

`batt_mv` cannot attribute energy to the panel. Fitting drain against panel duty
over those same seven boots gives a **negative** slope, and the boot with the
lowest duty (43, at 0.1 %) shows the **highest** drain at 117 mV/h, while the
boot with the highest duty (35, at 21.3 %) shows zero. The cell sits on its
voltage plateau, the boots are short, and the ADC resolution swamps the signal.
**Do not quote a per-refresh energy figure off `power.csv`. It is not in there.**

Two instruments do answer it, and both are already ours.

**On-device**: the **BQ27220 fuel gauge at I2C `0x55`**, whose `Current()`
register `0x0C` is a signed mA reading (`lib/hal/HalGPIO.h:26-29`,
`lib/hal/HalGPIO.cpp:33`). `power.csv` has no current column today -- run6's
header stops at `ble` and `build`.

**On the bench**: the USB meter, with the cell taken out of the path in
software. Disabling charging already opens the BQ25896's BATFET (SLUSC76C p.18,
9.2.3.5), so VBUS current becomes the board's draw with no charge term;
`REG09` bit 5 `BATFET_DIS` / `PPM.shutdown()` forces it and survives the
charger's watchdog (`docs/t5s3-power-path.md` in the parent repo). **No opening
required and no bare board needed** -- an earlier version of this doc said the
cell-side path was closed to us and that was wrong.

**Read it as a difference, never as an absolute.** VBUS is ~5 V and the system
rail is `VBAT + 50 mV`, so the meter reads roughly 0.8x the board's current at a
3.85 V cell; the buck's efficiency at our 20-130 mA load is `[open]` in the
datasheet, whose light-load curve plots nothing below ~50 mA; and the charger
itself draws up to 3 mA (`docs/usb-power-meter.md`, "VBUS milliamps are not
board milliamps"). In a difference between two states the 3 mA offset cancels
exactly and the scale factor is common to both. **Every question this doc asks
is a difference**: a window against a whole panel, eleven passes against one,
`epd_fast` against `epd_text`, rails cycled per frame against rails held across
a burst. So the imprecise absolute does not block any of them.

What stays out of reach either way is the sub-milliamp end -- deep sleep at
~873 uA and ship mode are below what a whole board through a charger resolves,
and the gauge reports them as an exact zero. Irrelevant here: a refresh is tens
of milliamps.

That is T-275.

## 4. What the two reference projects say

Read off upstream source and upstream docs, **nothing in this section is
measured by us**. Observed 2026-09-07: FastEPD `main` at `9113bdd`,
OpenTrailPaper `main` at `19ea78c`. Added 2026-09-09: EPD_Painter `6335a14`
(2026-08-01) and OpenTrailPaper `981ef4a` (2026-09-08), both cloned and read
directly. **EPD_Painter is the one that matters most**, because it is what
OpenTrailPaper ships and it is the only project on this panel that publishes a
measured per-pass time.

**Neither project has anything on the sunlight refresh failure** (BUG-022, and
[`eink-refresh-degradation.md`](eink-refresh-degradation.md)). Everything below
is ghosting, cost and residue.

### FastEPD (drives this exact panel, `BB_PANEL_LILYGO_T5PRO`)

Since `4ab7155` there is exactly one commit, `9113bdd`, a Linux build fix adding
two `#ifndef __LINUX__` lines near `FastEPD.inl:2541`. **Every citation in
[`fastepd.md`](fastepd.md) past that line is now off by +2.**

- **A row-ranged partial update still clocks every row** --
  `for (i = 0; i < pState->native_height; i++)` at `:3627`, unchanged. Matches
  `Panel_EPD` exactly (section 2c).
- **The ping-pong bug is still there and it is a real bug**, not a design
  choice: `iDMAOff` alternates two halves of `dma_buf` but the `memset` runs only
  when `iSkipped == 0` (`:3635-3637`), so **every other skipped row re-clocks
  the band's last diff row**. Independent confirmation that this is wrong:
  epdiy's I2S path does the same job correctly with `if (ctx->skipping < 2)
  memset(...)` (`render_i2s.c:72-86`). 1bpp path only; nobody has read
  `bbep2BppPartial`.
- **Do not compare its 32 ms pass to our 8.4 ms.** The 32 ms is a source comment
  (`:3616`) on its own hardware at a 20 MHz bus (`:226`), where the data alone is
  ~6.7 ms -- so most of a FastEPD pass is row control, which our 8.4 ms figure
  also omits. Neither number is measured on our board. Its default is 4 partial
  passes plus the mandatory neutral clear, five passes, roughly 160 ms. **Whether
  our eleven passes are the expensive part is exactly what section 3's
  measurements answer; it does not follow from this comparison.**
- **`bKeepOn = false` by default**, and `// This clear to neutral step is
  necessary; do not remove` on every path. Read correctly, that comment is a
  warning against cutting the rails *without* a neutral pass, not an argument
  for cutting them -- see 3b.
- **An S3 LCD-peripheral quirk described in a code comment** (`:1711-1720`,
  commit `3783215`): a spurious clock cycle before data is ready, corrupting the
  start of each native scan line, worked around with an opt-in bit-banged bus.
  **Not checked against Espressif's errata, and nobody has checked whether
  M5GFX's `Bus_EPD` drives the same peripheral the same way.** A suspect if
  scoped updates show a corrupted band at the start of scan lines, not a
  diagnosis.

### EPD_Painter, which is what OpenTrailPaper actually ships

Read 2026-09-09 at `tonywestonuk/EPD_Painter` `6335a14` (2026-08-01) and
`RaemondBW/OpenTrailPaper` `981ef4a` (2026-09-08), both cloned and read here,
not taken secondhand. **Nothing in this subsection is measured by us**; the
numbers marked measured are theirs.

**Their shipping build is EPD_Painter, not epdiy.** `default_envs =
t5s3-painter` (`OpenTrailPaper/platformio.ini:13`), whose block at `:98` is
commented "THE SHIPPING BUILD... Proven on hardware: boots, renders the
dashboard, BLE/GPS/SD all up", with `[env:t5s3-pro]` on epdiy kept only "as the
epdiy fallback if the port needs backing out". So the maintainer's reported
partial refresh, "probably 100 ms", runs through EPD_Painter.

**What their fast path actually is.** `QUALITY_FAST` uses a **7-slot** waveform
train -- `wf_len = 7`, explicit in the dispatch at `EPD_Painter.cpp:1253`, and
the arrays are declared `uint8_t fast_lighter[3][7]` (`EPD_Painter.h:82`). It is
a **4-level** mode and cannot be anything else: `EPD_Painter.cpp:216-217`
refuses `QUALITY_FAST` with 16 greys and tells the caller to
`setGreyLevels(4)` first. And 4-level FAST pads nothing between passes --
`EPD_Painter.cpp:1505`, verbatim, `// QUALITY_FAST = No delay.`

**So the 15 ms constant pass period does not apply to their fast path**, and a
"7 x 15 ms = 105 ms" reading of it is wrong. The 15 ms is
`g16_pass_us_normal` / `g16_pass_us_high` (`EPD_Painter.h:187-188`), it exists
only for the **16-grey** qualities, and there is no `g16_pass_us_fast`. Its
purpose is stated: the pass loop **pads** every pass to it "so retention dose
depends only on the trains, and the trains are calibrated AT this period". It
is a deliberate ceiling, not a cost.

**The number worth comparing against is their row loop, and it is measured.**
`EPD_Painter.h:256-259`: **7-8 ms** on a match card, **13.3 ms** on a
photograph, against the 15 ms period with a 4 ms settle floor -- the note exists
because a photograph's 13.3 ms plus settle "stretched the pass to 17.3 ms" and
over-dosed every level by 15 %. Their pass clocks **every row**
(`for (int row = 0; row < _config.height; row++)`, `EPD_Painter.cpp:1419`),
exactly as `Panel_EPD` and FastEPD do (2c), so it is comparable to ours.

**But OpenTrailPaper does not run the fast path, and there is a measured frame
number for the path it does run.** The preset default is
`.quality = EPD_Painter::Quality::QUALITY_NORMAL`
(`EPD_Painter_presets.h:201`), nothing in OpenTrailPaper's `src/` ever calls
`setQuality`, and its pixel mapping goes to four levels ("4bpp nibble
(0 black .. 15 white) -> EPD_Painter level (0 white .. 3 black)",
`epd_compat.cpp:500`). So it paints **4-level NORMAL**: 13 passes.

4-level does not pad to a constant period. It holds a fixed
**inter-pass gap** instead, and that gap is where their frame number comes
from -- `EPD_Painter.h:170-178`, verbatim: *"at NORMAL it is 13 x 4 ms = 52 ms
of a ~125 ms cycle, measured with setPaintProfile()"*.

**So their whole-panel frame is ~125 ms, measured by them with their own
profiler.** That is the figure the maintainer's "probably 100 ms" is an eyeball
of, and it is a far better anchor than anything derived from the fast path.
Back out the settling and ~73 ms remains for 13 passes of row sending, about
**5.6 ms per pass** `[our arithmetic on their measured numbers]`, consistent
with the 7-8 ms match-card row loop for content as light as a dashboard.

| their path | passes | cost | source |
|---|---|---|---|
| **4-level NORMAL -- what they ship** | 13 | **~125 ms** | measured, `setPaintProfile()` |
| of which inter-pass settling | 13 x 4 ms | 52 ms | stated |
| implied row sending | 13 | ~73 ms, ~5.6 ms/pass | our arithmetic |
| 16-grey NORMAL | 13 | 195 ms | stated, padded 15 ms |
| a GL16 clean they **deleted** | -- | **~950 ms** | their comment |
| 4-level FAST | 7 | unpadded, no delay | **not used by them** |

The last two rows matter as much as the first. Their 16-grey clean cost
**~950 ms** and they removed it: *"The driver DC-balances its own output, so
there is nothing left to clean up after -- and the ~950 ms hitch that clean
cost is gone"* (`OpenTrailPaper/src/ui_dashboard.cpp:2521`). **That is our
number.** We pay roughly what their deleted 16-grey clean cost, on every marker
move.

**One of their own numbers does not survive, and our first correction of it was
also wrong.** The comment at `EPD_Painter.h:158-167` says HIGH is "20 slots
against 13" for "~300 ms". We first answered 13, off `high_lighter[3][13]`
(`:86`) and `wf_len = 13` (`EPD_Painter.cpp:1259`). **That is the 4-level train
length, not the 16-grey step count**, and 15 ms is a 16-grey period, so the two
never meet -- the same category error as the "7 x 15 ms" above, made in the
other direction. Corrected by the commissioning session, 2026-09-09, and
re-checked here.

The 16-grey step counts are enforced by the field types themselves:
`g16_apply` is `const uint8_t (*)[13]` and `g16_apply_high` is
`const uint8_t (*)[32]` (`EPD_Painter.h:115-118`), and this board supplies
`TUNED16_LILYGO_T5S3_NORMAL[16][13]` and `TUNED16_LILYGO_T5S3_HIGH[16][32]`
(`LilyGo_T5S3_GPS_Trains.h:72`, `:144`). **So 16-grey NORMAL is 13 steps and
HIGH is 32**, giving 195 ms and **480 ms** at this board's 15 ms period. The
comment's 20 matches neither; the nearest thing to it in the tree is the
`apply[16][20]` array in the tuning struct at `EPD_Painter_tuned.h:132-133`.

**And 15 ms is genuinely per-board, as its own comment claims**: another preset
carries `g16_pass_us_normal = 20000` (`EPD_Painter_presets.h:56-57`), so
nothing about 15 travels to a different panel.

Separately,
OpenTrailPaper's "~475 ms" whole-panel figure is **derived by dividing a stated
1.9 s by four** in a comment about a boot clear
(`OpenTrailPaper/src/epd_compat.cpp:712-713`), with no instrument named, and it
describes a *clear* rather than the fast path.

### What their number does to ours, and it is not about windowing

**The bus and the glass are exonerated, now by someone else's measurement.**
Section 3 derives 8.37 ms of pure clocking per pass and 11-14 ms realistic with
row overhead. EPD_Painter **measures** 7-13.3 ms for the same job on the same
panel. Those are the same range. So none of our excess is the panel, and that
was previously only our own arithmetic.

**The gap is not windowing, and T-271 will not explain it.** Their ~100 ms is a
whole-panel push with a short train and a delta engine, the same shape as our
whole-panel `FAST`. This agrees with section 1b from the other direction: our
own `ref_window` and `ref_fast` are already the same whole-panel operation.

**Where ours goes.** Section 1b's two-mode fit gives **455 ms fixed plus
~52 ms per pass** over 11 passes. Set against their measured 7-13.3 ms:

| | theirs | ours (fitted, `[derived]`) |
|---|---|---|
| per pass, row sending | 5.6 ms implied, 7-13.3 ms measured range | ~52 ms |
| passes in the shipped path | 13 | 11 |
| fixed cost before any pass | 52 ms of inter-pass settling | **~455 ms** |
| whole-panel frame | **~125 ms, measured** | ~1,030 ms |

So it is **two independent problems, and the ratio is 8x rather than 6x**:
~1,030 ms against a measured ~125 ms, at a comparable pass count. Roughly 44 %
of our frame is prep and rail before a single pass runs, where their equivalent
term is 52 ms of deliberate settling that the panel actually needs. And our
passes are still **4-9x** dearer than the same work measured next door. A
per-pass reading of "~94 ms" (1,030 / 11) assumes the whole frame is passes,
which the fit says it is not.

**The structural suspect for the per-pass half is already in section 3**:
`blit_dmabuf` re-reads the **whole 1,036.8 kB step framebuffer on every pass**,
where EPD_Painter streams a packed 4bpp buffer into a per-row DMA buffer. Ours
scales with nothing and is paid eleven times.

**And Lead A is not a suspect.** Section 1b killed it off the source:
`LgfxEpdDriver` overrides neither `displayWindow` nor `supportsAsyncDisplay`, so
`displayAsyncImpl` takes the blocking path before `syncPendingAsync()` is
reached and `_refreshPending` is never set here.

### OpenTrailPaper (same board, bike head unit)

Two commits since `fcfe128`, both Android and CI only -- nothing in `src/` or
`investigations/`. Our notes from 2026-09-05 are current.

**The important correction: their shipping build is the architecture this doc
calls behind.** The scoped-DU work lived in their epdiy era and was deleted;
today `epd_compat.cpp:688-693` hands whole 8bpp frames to the Painter's delta
engine, and the epdiy fallback calls `epd_hl_update_screen` on the whole screen.
So the items below are physics and history, not a description of a running
system we are chasing.

- **The motivation, and it was reasoned rather than measured.** In the epdiy era
  they argued that a full-screen DU for one moving GPS dot "was electrically
  disturbing all 518,400 pixels" (`investigations/display-ghosting.md:113`) --
  DU is not DC-balanced, so every held pixel sits through the drive voltages
  anyway. The quote is verbatim; no instrument was involved. They scoped each DU
  to the changed rows. **We do not even scope the CPU work.**
- **Diff against the driver's model, never a private shadow.** See 2b.
- **Two-phase flash through white** for residue inside a dithered dark area:
  force the region white and clean, then restore content and clean; a single
  inverting pass fails because adjacent dither pixels drive in opposite
  directions. epdiy era; their shipping build uses the Painter's scoped `SOFT`
  scrub instead. An **untested analogue** on `Panel_EPD`.
- **Anti-aliased text leaks mid-grey.** They found ~20 % of one bundled font's
  glyph pixels at intermediate levels, ~6,300 pixels or 1.2 % of a dashboard.
  Their `flattenTo1Bit()` does not appear anywhere in `src/` on `main` today, so
  treat this as *they found*, not *they do*.
- **`FULL_REFRESH_EVERY 60` is defined and used nowhere** (`config.h:63`, one
  stale comment in `ui_dashboard.h`). It was **abandoned as a misdiagnosis
  before it was ever built** -- a genuinely static frame already short-circuits
  on a `memcmp` and never touches the panel, so a periodic clean had nothing to
  clean. Read that before anyone builds a `ghostClearInterval` here.
- **Defer the expensive clean** until ~900 ms of no input, each new tap pushing
  it out. `GHOST_CLEAN_SETTLE_MS` is epdiy-era and not in the shipping build; the
  idea is what transfers.
- **Async trap**: `paint()` returned while rows were still clocking, and "an
  interrupted clear is how ghosts get baked in" (`epd_compat.cpp:735-736`). They
  now wait 6,500 ms to cover the driver's stock 5 s idle countdown. Ours is the
  same shape once T-270 lands.

## 5. The plan, cheapest first

Two goals, and section 3b says they do not share a fix: **speed** comes from
T-271 and T-272, **battery** comes from T-273 and T-274. T-269 and T-275 are the
instruments.

**Revised again 2026-09-09, after T-269 ran: T-273 is the lever, and it is now
backed by measurement.** A pass costs 34 ms and 85 % of it is one PSRAM sweep
that reads the whole step framebuffer regardless of what changed, so pass count
is very nearly the whole bill: a marker move is 11 x 34 ms, a clean frame
37 x 34 ms = 1.25 s, measured. Un-aliasing `epd_fastest` to take `epd_fast`
from 11 passes to 8 is a straight ~100 ms off every marker move, and giving
`lutText` a tuned table is worth far more on every clean frame. Neither needs
new code. The deeper fix is to stop re-reading 1,036.8 kB per pass at all,
which is a `Panel_EPD` change and so an upstream or fork question.

**Superseded, kept for the reasoning: build T-269 first.** The earlier advice was to reach for
T-271 if only one thing gets built. Section 4 removes its premise -- the project
running this panel at ~100 ms does it with a **whole-panel** push and a short
train, so scoping the rectangle is not what separates us from them, and section
1b shows our own window and whole-panel counters are already the same
operation. T-271 is still worth having for the CPU work it removes, but it is no
longer the item that explains the number. T-269 is, and it needs no SDK change
and, for half of it, no code change at all.

**T-269. Measure the frame properly. This is now the first thing to build, not
the instrument for the others.** Not just `micros()` around the three phases in
`pushCanvas` -- that cannot see inside the scan. Run both measurements in
section 3: the unchanged-frame subtraction, which needs no code change and
isolates the cost of ten passes, and a scratch-copy pass counter and timer
inside `task_update`.

**It has a target now.** OpenTrailPaper on EPD_Painter measures a whole-panel
4-level frame at **~125 ms** with their own profiler, 13 passes with 52 ms of
inter-pass settling inside it, so about **5.6 ms of row sending per pass**
(section 4). Report the pass counter's output against those, and **not** against
their 15 ms constant period, which is a padded ceiling for their 16-grey
qualities and does not apply to the path they ship. Two numbers come out and they answer different
questions: the per-pass time says whether our passes are 4-7x dear, and the
unchanged-frame subtraction says how much of the 1,030 ms is not passes at all
-- section 1b's fit predicts **~455 ms** of it, which would be the larger of the
two problems.

**Done, 2026-09-09, measured on hardware.** Pass counts confirmed as 1, 11 and
37; per-pass 33.8 ms; `blit_dmabuf` 82-89 % of it; the scan linear with a zero
intercept; prep measured separately at 234-574 ms. Section 3 carries the table
and the instrument. What it hands the next step: **the fix is `blit_dmabuf`,
not the bus and not the rectangle**, and what stays open is the scan-side half
of the 2x.

**T-270. Collect the asynchrony that already exists.** Implement
`supportsAsyncDisplay()`, `displayStart()` and `displayFinish()` on
`LgfxEpdDriver` over `Panel_EPD`'s existing task: `displayStart` pushes and
returns without the trailing `waitDisplay()`, `displayFinish` waits. No panel
behaviour changes -- the same bytes reach the glass in the same order. Frees up
to a second of CPU per map frame for tile work. Two rules come with it: never
sleep or cut rails between start and finish, and keep `fadingFix` users on the
blocking path exactly as `GfxRenderer::displayBufferAsync` already does. **Done
when** a map session on the board shows tile work overlapping a refresh in the
log and no new ghosting. Independent of T-269.

**T-271. Override `displayWindow` on `LgfxEpdDriver`.** No library patch needed:
`LGFXBase::display(x, y, w, h)` is public (`LGFXBase.cpp:96-106`) and
`Panel_EPD::writeImage` already narrows `_range_mod` to whatever rectangle was
written. Three things the obvious implementation gets wrong, all found before
anyone wrote it:

- **`pushImage(x, y, w, h, band, lgfx::grayscale_8bit, nullptr)` does not
  compile** -- a bare `nullptr` cannot deduce the palette template parameter --
  and the tempting fallback `pushImage(x, y, w, h, (const uint8_t*)band)`
  compiles and is wrong, because `create_pc(const uint8_t*)` reinterprets the
  bytes as rgb332 (`LGFXBase.hpp:1189`). Use
  `pushImage(x, y, w, h, reinterpret_cast<const lgfx::grayscale_t*>(band))`, or
  build the `pixelcopy_t` by hand the way `push_sprite` does
  (`LGFX_Sprite.hpp:421-425`). For an 8-bit source the band is tightly packed
  `w * h` bytes (`LGFXBase.cpp:1427-1428`).
- **`setAutoDisplay(false)` in `begin()` would silently blank the whole-panel
  path.** `pushCanvas` never calls `display()`; it relies on
  `pushSprite -> pushImage -> endWrite -> auto display` to queue the refresh.
  With auto off, the sprite lands in `_buf`, nothing is queued, both
  `waitDisplay()` calls return instantly, and the panel stops updating. Either
  keep auto-display on and accept one duplicate queue entry per window (harmless
  -- the second diff pass arms nothing), or add an explicit `g_dev.display()` to
  `pushCanvas` in the same commit.
- **In the same pass, make `MapActivity::windowRefreshAffordable` driver-aware.**
  See section 6.

Alignment is compatible: `Panel_EPD::display` floors `x` to even and rounds `w`
up by two (`:573-580`), then `task_update` rounds the byte width **down** to even
bytes, i.e. 4 pixels (`:933-934`); `GfxRenderer::displayBufferWindow` already
hands us an 8-pixel-aligned rectangle in panel memory coordinates
(`GfxRenderer.cpp:1584-1590`, `:283-285`). Panel geometry is landscape 960x540
with `rotation` 0 (`BoardConfig.h:880-885`), so the driver needs no coordinate
translation. Depends on T-269.

**T-272. Delete the sprite.** Once T-271 pushes bands directly, `g_canvas`
(518.4 kB PSRAM) has no reader for the B/W path and the private-shadow trap goes
with it. `cleanupGrayscaleBuffers` refills the canvas without pushing
(`LgfxEpdDriver.cpp:281-289`) -- a wasted 518 kB write today, and it needs
rethinking with the sprite gone. The grey path needs a scratch band of its own.
Feeds the planned firmware memory audit. Depends on T-271.

**T-273 is a 1-bit problem on the fast path, and that removes the usual
constraint.** Read off the code 2026-09-09, and it sharpens this item from "try
a shorter table" into something with a known shape.

`Panel_EPD`'s fast branch Bayer-thresholds every value to pure black or white --
`readbuf[i] = (sum + (b << 4)) < 248 ? 0 : 0xF;` (`Panel_EPD.cpp:492-497`) --
so **14 of `kFastLut`'s 16 columns are unreachable on the marker-move path.**
Only columns 0 and 15 are ever selected. Reading our own table down those two
columns (`LilyGoT5S3LgfxConfig.cpp:26-37`):

| column | across the 8 drive passes |
|---|---|
| 0, black | `2,2,1,1,1,1,1,1` -- drive white twice, then black six times |
| 15, white | `1,1,2,2,2,2,2,2` -- drive black twice, then white six times |
| 1-14, grey | two passes of anything, then `3` (do nothing) x6 |

So a pixel heading for white is **driven the wrong way for two passes first**.
That is the flash a marker move produces, and section 6 already noted our table
does not prevent it. Six of the eight rows are pure `3` for every grey column,
i.e. dead weight for levels that cannot be selected.

**Therefore the grey-level cost of a shorter table does not apply here.** The
only remaining constraint on the fast path is **dose**: how many passes a pixel
needs to actually land on black without residue. That is a panel question, not
a configuration question, and it is cheap because the table is data.

Estimated against the measured 34 ms per pass, `[derived]`:

| table | passes | scan |
|---|---|---|
| `kFastLut` today | 11 | 366 ms |
| library `lut_fastest`, un-aliased | 8 | ~272 ms |
| a hand-written 1-bit table, 4 drive passes, no opposite pre-drive | 7 | ~238 ms |
| the same plus band-scoped blit, 100 rows of 540 | 7 | **~90 ms** |

The two levers multiply: pass count scales the whole frame, band-scoped blit
scales with area.

**The clean path is NOT 1-bit, so do not carry this over to it.** `Full` and
`Half` map to `epd_text`, where the Bayer threshold does not run, and our own
grayscale overlay writes `0x55` and `0xAA` for anti-aliased text
(`overlayCanvasGray`, `LgfxEpdDriver.cpp`). Those four levels are real. What
does carry over is that its 37 passes are the **library default** -- a 12-row
eraser prefix plus 19 idle rows that nobody chose -- and that is where the
measured 1,253 ms sits.

Calibration reference: EPD_Painter drives full black with 7 continuous passes
and calls its extreme levels "well into saturation" (section 4), so 7 is
generous rather than marginal there. Its pass is a padded 15 ms against our
34 ms, so dose per pass is not directly comparable and ours may need fewer.

**T-273, settled 2026-09-17 on two boards.** A second T5 S3 Pro made the
comparison that one panel cannot: same firmware, same four z13 tiles, every
command written to both ports before either reply was read.

| fast table | opposite pre-drive | passes | on the glass |
|---|---|---|---|
| `kFastLut` | 2 | 11 | clean |
| library `lut_fastest` | 1 | 8 | **indistinguishable from 11** |
| `kFast1bitLut` | 0 | 7 | visible ghosting, acceptable but a quality drop |

Forty marker moves, never leaving the map. **One pre-drive pass is necessary
and sufficient**, so the answer is the 8-pass table: ~100 ms off every marker
move at no visible cost.

**The 2026-09-09 ordering was right about the mechanism and wrong about the
threshold.** It had 8 as "unpleasant", from judging one panel after another --
a harsher test than side by side, as the same tables showed today.

**The idle tail is a residue lever after all, on one surface.** Shipped at
`idle=6` on 2026-09-17, board A woke from the sleep screen and carried its logo
as a ghost over the Home menu. A second T5 S3 Pro running the stock waveform
beside it did not. Put back to the stock 19 on 2026-09-18.

The mechanism was never wrong. An idle row is all-`3` and drives no pixel, so a
longer tail still erases nothing: erasure is the eraser prefix plus the twelve
drive rows, and the tail is settle. **The test was wrong.** Every residue trial
ran the map and the menu and none ran sleep to Home, which is the one path where
a full-screen solid has to be lifted. So "4 against 19 is indistinguishable" was
only ever true of the paths tried, and the 2026-09-09 claim that `idle=4` and
`idle=6` failed was withdrawn on 2026-09-17 on that same incomplete evidence.
Both the claim and its withdrawal outran what had been looked at.

What settle buys is still unpriced, and now there is a surface that reacts to
it. **Open**: whether the ghost is the tail's length or the 8-pass fast table
underneath it. The idle-19 build is on board B, board A stays at `idle=6` as the
failing reference, and one sleep-to-Home on each answers it.

**A defect fell out of it, `BUG-204` in the parent repo**: one clean does not
fully erase Home. Walk the Home menu, take the single clean into tilesync, and
the menu items, the logo and the bottom buttons come through -- faintly at stock
settings, pronounced with the 7-pass table.

**Three protocols that return a false clean**, each of which cost hours here:

- **Cycling two screens** only asks whether a clean tidies up after another
  clean. It passes all the way down to `idle=4` and proves nothing.
- **`CMD:GOTO_MAP` is two cleans**, with a tile-loading screen between them, so
  nothing survives it. Anything measured through that transition comes back
  spotless.
- **Judging on Home** hides faint residue against its flat white.
- **Never running sleep to Home** leaves the hardest surface untested. The sleep
  screen is a full-screen solid and the Home menu that replaces it is mostly
  white, so it is the widest area one clean ever has to lift. Every idle-tail
  conclusion before 2026-09-18 was drawn without it.

The single-clean paths are `map -> CMD:BUTTON back` and
`Home -> CMD:GOTO_TILESYNC`. Use those.

**Superseded, kept for its numbers: T-273 fast half, 2026-09-09.**
`kFast1bitLut` -- four passes of pure drive, no opposite pre-drive, greys inert
-- is the default for the marker-move path. Seven passes against eleven.

Measured on a T5 S3 Pro over a **real Bratislava render** (`tiles_ok=4`, 2,465
ways drawn, 1.2 MB of tile data -- not an empty viewport, which would arm far
fewer pixels and prove nothing):

| table | passes | wall clock | scan | prep |
|---|---|---|---|---|
| `kFastLut` | 11 | 1,046 ms | 474 ms | 572 ms |
| `kFast1bitLut` | **7** | **875 ms** | **302 ms** | 573 ms |

**171 ms off every marker move, 36.3 % off the scan.** The pass count predicts
4/11 = 36.4 %, so the linear model from T-269 holds to a tenth of a percent.
And **prep did not move** -- 572 against 573 ms -- which is the control that
says a waveform table touches the scan and nothing else.

**The half no instrument here can answer was answered by eye.** Dose does not
appear in `CMD:SCREENSHOT`, which reads the framebuffer rather than the glass,
so whether four drive passes still land a solid black without residue needed
the maintainer to look at the device. Confirmed after eight consecutive marker
moves on 2026-09-09.

`CMD:EPDLUT 0` rolls back to the old table without a rebuild, and that is worth
keeping: residue accumulates over a ride, this table is shorter than the one it
replaces, and **nothing on the map path promotes a fast refresh back to a clean
one** (`refresh-modes.md`, "The map never asks for a clean after entry"). A long
ride is the test this has not had. That is T-274's problem and it is now
slightly more urgent.

**Still open on this item: the clean frame.** `epd_text` remains 37 passes and a
measured 1,253 ms, which is the single largest number in this doc. It cannot
take the same treatment: `epd_text` does not run the Bayer threshold, and our
own grayscale overlay writes `0x55` and `0xAA` for anti-aliased text, so those
levels are real and a table for it has to be designed rather than shortened.

**T-273. Tune the pass count.** Un-alias `epd_fastest` from `kFastLut` and use it
for marker-only moves: 8 passes instead of 11. Give `lutText` a tuned table so a
clean frame is not 37 default passes. Both are data, not code. FastEPD's
`gray_matrix_editor` is the prior art for the iteration loop -- live edit, live
redraw, dump source, the same shape as `tools/style_watch.py`. **Done when** a
marker move uses a real short table and a clean frame uses a tuned one, both
judged on the panel at 1:1. This is the largest battery item. Depends on T-269.

**T-274. A scrub policy, and not a counter.** Scoped clean on named transitions
-- entering the map, a popup closing over it, a page flip -- deferred until input
goes quiet, two-phase through white where the region is dithered. Explicitly
**not** a `ghostClearInterval`: OpenTrailPaper's `FULL_REFRESH_EVERY 60` was
abandoned as a misdiagnosis before it was built. Note that our clean mode
(`epd_text`) re-arms every non-white pixel (2b), so a scoped clean over a hatched
map is expensive twice over. **Done when** a long map session ends without
accumulated residue and no clean fires during a zoom burst. Depends on T-271.

**T-275. Measure what a refresh costs, two ways.** On-device: the BQ27220 at
I2C `0x55` already answers `Current()` in signed mA (`lib/hal/HalGPIO.h:26-29`,
`lib/hal/HalGPIO.cpp:33`) and nothing samples it into the log -- add the column
and take a sample deliberately inside a refresh and one between refreshes. On
the bench: the USB meter with charging disabled so the BATFET is open and the
cell is out of the path, read as a **difference** between states (3b explains why
the absolute carries an open efficiency term and why the difference does not).
The states worth pairing are the ones the plan turns on: a window against a whole
panel, an unchanged frame against a one-pixel change, `epd_fast` against
`epd_text`, and rails cycled per frame against rails held across a burst. Without
this every power claim about this panel stays an argument, because `batt_mv`
demonstrably cannot carry it. Independent of the others, and needed before T-273
or T-274 can be judged.

## 6. Traps, written down before anyone hits them

- **Mixing epd modes defeats the diff, for every pair of modes.** `Panel_EPD`
  stores `pixel + (lut_offset << 8)` per pixel, and `lindex` advances per LUT
  step even when two modes point at the same table (`Panel_EPD.cpp:266-284`), so
  `epd_fast` and `epd_fastest` get different offsets despite both being
  `kFastLut`. Any mode change re-arms every pixel in the rect. The board config
  comment (`LilyGoT5S3LgfxConfig.cpp:18-25`) has this right but blames "the
  LovyanGFX default `lut_fast`" for the resulting flash -- **our own `kFastLut`
  column 15 is `1,1,2,2,2,2,2,2`, two frames to black then white, the same
  shape.** The diff is the only thing preventing the flash; our LUT does not
  prevent it.
- **`kFastLut`'s grey columns are unreachable through the current write path.**
  In fast and fastest, `_draw_pixels` Bayer-thresholds every 8-bit value to 0 or
  0xF (`Panel_EPD.cpp:495`), so the 0x55 / 0xAA grey canvas becomes an ordered
  dither and columns 1-14 are never selected. The board config's "AA nudge
  columns" comment describes a path fast mode never takes. **Read off code, not
  measured** -- worth confirming on the panel before anyone tunes those columns.
- **The auto-display cache writeback is a real library defect and irrelevant
  here.** `Panel::endWrite()` calls `display(0, 0, 0, 0)` (`Panel.hpp:88`), and
  `Panel_EPD::display` computes its writeback from the **arguments**, not
  `_range_mod` -- `cacheWriteBack(&_buf[0], 0)` flushes nothing
  (`Panel_EPD.cpp:586`). `LGFX_USE_CACHE_WRITEBACK_ADDR` *is* defined in this
  build, so the call is live, not compiled out. It does not matter on this chip:
  the ESP32-S3 has **one** data cache serving both cores (a shared DCache with
  per-core buses -- `extmem_reg.h:15,37-42`, `cache_ll.h:566-568` in the pinned
  IDF 5.5.2; contrast the original ESP32, whose `soc_caps.h:154` says the cache
  is within one core), and nothing DMA-reads `_buf` -- `task_update` reads it
  with CPU loads and writes into internal-RAM DMA buffers. **So call
  `display(x, y, w, h)` explicitly for deduplication and control, not for cache
  coherency.**
- **`_range_mod` accumulates, and an odd width loses pixels.** Two writes before
  one display give the bounding box of both. Worse, `task_update` rounds the
  half-width down to even (`:933-934`), so a stray draw that leaves `w/2` odd
  drops its last two pixels from the diff entirely -- not merely a widened
  window. Eight-aligned rectangles from `screenRectToAlignedMemRect` are safe.
- **`FreeInkDisplay::displayWindow`'s inverted early-out is dead code here.** It
  redirects to a whole-buffer `FAST_REFRESH` when `_inverted || _inversionDirty`
  (`FreeInkDisplay.cpp:689-695`), and a dirty inversion is then promoted to
  `HALF` -- 37 passes. Both flags default false and **nothing in `src/`, `lib/`
  or `freeink-sdk/` ever calls `setInverted` or `toggleInverted`**;
  `SleepActivity`'s `invertScreen()` is `GfxRenderer::invertScreen()`, which
  flips framebuffer bits and never touches the facade. So it cannot fire today --
  but anyone who wires inversion up later silently turns every window on this
  board into a 37-pass whole-panel clean.
- **`MapActivity::windowRefreshAffordable` is guarding against the wrong
  driver.** It refuses a window when `(w/8 + 1) * h + 4 kB` will not fit the
  largest free heap block (`src/activities/map/MapActivity.cpp:4234-4247`), and
  its own comment says why: `Ssd1677Driver::displayWindow` allocates per window.
  `LgfxEpdDriver` allocates nothing on that path. Today the gate is merely
  pointless -- it swaps a whole-panel refresh for a whole-panel refresh. **After
  T-271 it would refuse real windows for a reason that does not exist on this
  board.**
- **The private shadow.** See 2b. Either keep filling the sprite whole or delete
  it; never scope the fill and keep the sprite.
- **A window cannot clear the glass.** `FreeInkDisplay::displayWindow` already
  refuses to spend a pending `requestCleanNextFrame()`
  ([`refresh-modes.md`](refresh-modes.md)), and the same logic holds here: only
  an eraser-prefixed mode resyncs the model to the panel.

## 7. Status

Sections 1, 2, 5 and 6 are **read off the source**, cited above, at
`release/lilygo-t5-s3-pro` `aa831fcb` with M5GFX 0.2.28 as vendored in
`.pio/libdeps/t5s3pro`, on 2026-09-07, and every claim in them was re-checked by
an adversarial pass the same day. IDF facts (the i80 clock tree, the shared S3
data cache) are read off the pinned ESP-IDF 5.5.2 on disk; the TRM sentence that
would state the shared cache directly is **not** on disk and is inferred from
the register map.

Section 3's bus arithmetic is **derived and checked**. Its PSRAM bandwidth
range, per-row ISR overhead and 210-500 ms floor were estimates with nothing
behind them; **they are now superseded by measurement** -- 34.16 ms per pass,
zero fixed cost inside the scan, `blit_dmabuf` at 82-89 % of it, 23 frames on a
named T5 S3 Pro on 2026-09-09, instrument and archived binary named in that
section. The prep/scan split is **measured**, pairing the pass timer against
`PowerTelemetry`'s wall clock on the same refresh. The CPU-clock explanation
for the prep half is **consistent with the counters but not isolated**, and the
scan half of the 2x is **`[open]`**: `blit_dmabuf` barely moves between the
fast and slow populations, so a core-clock story does not fit it. Section 3b's panel-duty table
is **measured** off `docs/power-runs/run6-2026-09-04.csv`, seven boots of build
`0.2.0-t5s3pro`; the failed drain fit in the same section is **measured and
negative**, which is why no per-refresh energy figure appears anywhere here; the
rail-cycle cost is an **estimate**. The 1,081 / 1,117 ms pair is **measured on
the T5 S3 Pro** (one 4 h 36 min walk, 2,608 window requests, both averagings
named in section 1).

Section 1b is **read off the source and recomputed off `power.csv`**, on
2026-09-09, with **no hardware involved**. Its two population tables are
measured (run8, the card's whole history, n=998 and n=73 isolated rows, and the
method reproduces `develop`'s earlier run7 figures exactly). Its three source
findings are **verified off the code**: `LgfxEpdDriver` overrides neither
`displayWindow` nor `supportsAsyncDisplay`; the async path billed 0 ms for a
refresh that completed inline; the grayscale panel path is uncounted entirely.
Its per-pass arithmetic (52 ms per pass, 455 ms fixed, 2,390 ms predicted for a
37-pass frame) is **derived from two observed modes and nothing else** -- it is
a hypothesis the section 3 measurements settle, and one low sample already sits
below its floor. **The 2x itself remains `[open]`**, now for the stronger reason
that no mechanism of the right sign exists in the source.

Nothing added in section 1b has run on a device. The instrument branch
`t5s3-refresh-instr` builds clean for `t5s3pro` and is **unflashed**.

Section 4's EPD_Painter subsections are **read off upstream source** at
EPD_Painter `6335a14` and OpenTrailPaper `981ef4a` on 2026-09-09, cloned and
read here. Their 7-slot fast train, its 4-level-only constraint, its absent
inter-pass delay and the 7-13.3 ms row loop are **theirs, measured or stated by
them**; their ~125 ms whole-panel frame and the 52 ms settling term inside it are
**measured by them with `setPaintProfile()`**, and that is the path they ship
(preset default `QUALITY_NORMAL`, never overridden, four levels); the implied
5.6 ms per pass is **ours, derived from those two**; their deleted 16-grey
clean at ~950 ms is **their comment**; and
two of their own figures are **contradicted by their own code** (HIGH at 20
slots against a `[3][13]` declaration, and a 475 ms clear obtained by dividing
a comment's 1.9 s by four). The comparison table in that section pairs their
measured per-pass against **our fitted** one, which is `[derived]` on our side.

The rest of section 4 is **read off upstream source and upstream docs** at
FastEPD `9113bdd` and OpenTrailPaper `19ea78c`, observed 2026-09-07. Nothing in it is measured by
us, its two numbers (32 ms, 518,400) are a source comment and an argument
respectively, and the OpenTrailPaper items marked epidy-era are **history, not
their shipping behaviour**.

Nothing in section 5 has been built.

## 8. Vendor waveform: epdiy ED047TC1 phases and grey level encoding

**2026-09-14.** The ED047TC1 waveform file from the LilyGo vendor's own repository (branch H752-01) defines five distinct waveform modes, structured around phase counts and their timing. This section documents what the file actually specifies for full vs partial update phases, the timing implications, and how 16 grey levels are encoded.

**Source file**: `lib/epdiy/src/waveforms/epdiy_ED047TC1.h` from `Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO` repository (branch H752-01), raw URL `https://raw.githubusercontent.com/Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO/H752-01/lib/epdiy/src/waveforms/epdiy_ED047TC1.h`, read 2026-09-14.

### 8a. Five waveform modes and their phase structure

The waveform defines five EpdWaveformMode structures (line 27 in the vendor file), spanning three operational regimes:

**Corrected 2026-09-14, against the fetched file directly** (`epd_wp_epdiy_ED047TC1_*_times[]` arrays, summed with Python): the first commit of this section stated 1650 ms for modes 2, 5 and 16 alike, and 588 ms for mode 17. Only mode 2/5's sum is close (1570 ms, not 1650); mode 16 and mode 17 were each computed wrong, and mode 16 is not the "same total, sparser LUT" claim originally written here -- it is a genuinely shorter waveform.

| Mode | Type | Phases | Phase times (ms) | Total time (`sum(phase_times)`) | Purpose |
|------|------|--------|------------------|------------|---------|
| 1 | GC16_FAST | 5 | [1000,1000,1000,1000,1000] | 5000 ms | Full refresh, 1-bit (fastest) |
| 2 | GC16 | 30 | [30,30,20,20,30,30,30,40,40,50,50,50,100,200,300,10,10,8,8,8,8,8,10,10,15,15,20,20,100,300] | **1570 ms** | Full refresh, 16-grey (standard) |
| 5 | GC16_PARTIAL | 30 | [30,30,20,20,30,30,30,40,40,50,50,50,100,200,300,10,10,8,8,8,8,8,10,10,15,15,20,20,100,300] | **1570 ms** | Partial refresh, 16-grey |
| 16 | GC16 (optimized) | 15 | [30,30,20,20,30,30,30,40,40,50,50,50,100,200,300] | **1020 ms** | Full refresh, 16-grey (fewer phases) |
| 17 | GC16_PARTIAL (sparse) | 15 | [10,10,8,8,8,8,8,10,10,15,15,20,20,100,300] | **550 ms** | Partial refresh, sparse (fastest partial) |

**Specification temperature range**: 20-30°C (line 26, `EpdWaveformTempInterval`), a single range for all modes. LilyGo's own statement (docs/panel-ed047tc1.md, vendor mail 2026-09-04) confirms the firmware is "fixed at 20°C" and does not include temperature compensation, so waveform fidelity degrades outside this range.

### 8b. Phase count: full vs partial refresh

**Full refresh (16-grey, GC16 or optimized GC16)**:
- Mode 2 (standard GC16): **30 phases**, 1570 ms total
- Mode 16 (optimized): **15 phases**, **1020 ms total** -- genuinely shorter, not the same length with a sparser LUT

**Partial refresh (GC16_PARTIAL)**:
- Mode 5 (matching full, 30 phases): **30 phases**, 1570 ms total
- Mode 17 (sparse, optimized): **15 phases**, 550 ms total

**Key insight, verified element-by-element in Python against the fetched arrays**: Modes 2 and 5 are **identical in phase count and timing** (both 30 phases, same 30-element `phase_times[]` array, 1570 ms) -- the vendor's waveform distinguishes full vs partial refresh at the LUT data level (which pixels are updated), not at the phase-count or timing level for 16-grey updates. Two levels down from that, modes 16 and 17 are not independent waveforms either: **mode 16's 15-element array is exactly the first 15 elements of mode 2/5's 30-element array, and mode 17's 15-element array is exactly the last 15 elements of the same array** (`m2[:15] == m16` and `m2[15:] == m17`, both `True`). So the file encodes one 30-phase timing sequence for full 16-grey precision, and modes 16/17 are that same sequence split into its first half (a faster full-refresh option) and second half (the fast partial option) -- not two separately authored waveforms.

### 8c. 16 grey levels: phase-by-phase encoding

The panel achieves 16 grey levels (0-15, where 0=white, 15=black) through the LUT structure:
- **LUT dimensions**: Each mode has one LUT table with **16 rows** (one per grey level) and **16 columns** (one per drive command code, pre-indexed for the four voltage bytes per phase line).
- **Per phase**: Each of the 30 or 15 phase rows contains a full LUT copy, allowing grey-level-dependent drive sequencing.
- **Encoding**: The four bytes per LUT entry encode 16 2-bit drive codes (4 bits per byte = 2 codes; 4 bytes = 16 codes total), one code per possible grey level. At each phase, a pixel reads its grey value from the framebuffer (0-15), indexes the corresponding LUT row and current phase, and applies the 2-bit drive code at that index.

**Sparse encoding (Mode 17)**: The partial-refresh sparse mode uses predominantly `0x80` (a single 1-bit set, meaning "drive to one polarity") across most LUT entries, suggesting that partial refreshes can tolerate less granular grey sequencing than full refreshes and still achieve visible 16-level output.

### 8d. Refresh time implications

**Full refresh, 16-grey**: 1570 ms nominal for the full 30-phase sequence (mode 2/5), or 1020 ms for the 15-phase fast option (mode 16). Against the X4's datasheet claim of 4 s for full refresh, the ED047TC1 on the LilyGo board is nominally 2.5x to 3.9x faster on paper, depending on which mode drives it. However, measured times on hardware (section 1 of this doc, and refresh-modes.md) diverge from vendor nominal figures, so this datasheet value is **an upper bound, not a measured result on our board**.

**Partial refresh, fast**: 550 ms nominal (Mode 17, the second half of the 30-phase sequence), the vendor's `<200 ms` reference in docs/panel-ed047tc1.md appears to be achievable only with a more aggressively sparse waveform than Mode 17, or with windowed (actually scoped) refresh that does not clock every row of the panel. Since epdiy and our own M5GFX `Panel_EPD` do clock every row (section 2c), the 550 ms partial-phase timing is more realistic for actual hardware on this board.

**Nominal vs measured**: A 1570 ms full refresh and 550 ms partial-phase time assume the panel clocks out every phase without intervention. On the ESP32-S3's LCD peripheral (section 2c), PSRAM bandwidth, DMA stalling, or ISR latency may extend these. Section 3's 1,081-1,117 ms measured window cost sits between Mode 17's 550 ms (if only partial-phase cost) and Mode 2/5's 1570 ms (if full-phase cost), and lands close to Mode 16's 1020 ms -- consistent with the hypothesis that measured times include the gate-scan loop overhead and other fixed per-refresh costs not itemized in phase timing alone, on top of something closer to the 15-phase sequence than the full 30-phase one.

### 8e. What is open

- **Actual measured phase times on this hardware**, using the method in section 3: an unchanged-frame subtraction, or a timer bracket around `blit_dmabuf`, would answer whether the vendor's phase timings match the panel's actual response. The 550 ms / 1570 ms figures are untested on our board.
- **Grey-level fidelity in the sparse Mode 17 encoding vs Mode 5's full encoding**: whether the reduced LUT precision in Mode 17 is visible on the panel or purely an implementation detail.
- **Cold performance**: Modes 1-5 specify temperature range 20-30°C, so Mode 17's 550 ms and Mode 2's 1570 ms both degrade below 20°C, per vendor statement in docs/panel-ed047tc1.md. No measurement exists.
