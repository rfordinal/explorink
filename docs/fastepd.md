# FastEPD: what bitbank2's ESP32 parallel e-ink library taught us

Detailed findings from a research pass, 2026-09-01, into
<https://github.com/bitbank2/FastEPD> (bitbank2 / Larry Bank, Apache 2.0).
`watched-upstream-libraries.md` is the one-paragraph pointer to this doc;
everything long-form lives here, per this repo's one-topic-per-file rule.

Read at commit `4ab7155` (2026-09-01) plus the project's two-page GitHub
Wiki. All `FastEPD.inl:<line>` citations below are paths inside that
upstream repo, not this one -- **the library moves, re-read before citing
it as current**.

Why this library specifically: it names our reference board directly,
`BB_PANEL_LILYGO_T5PRO` in `src/FastEPD.h:93` is the LilyGo T5 S3 4.7"
Pro. It is actively maintained (a commit landed the same day as this
research). None of what follows is a recommendation to depend on it or
replace our own driver -- it is prior art, read for what it can tell us
about our own open problems.

## 1. BUG-022 (sunlight refresh failure): the fix already exists upstream, just not in FastEPD

FastEPD itself has **zero** mentions of light, sunlight, UV or ambient
brightness -- verified by grepping `src/`, `README.md`, both wiki pages and
all 16 of its GitHub issues. The material below comes from
`crosspoint-reader/crosspoint-reader` (our upstream) and
`open-x4-epaper/community-sdk`, not from FastEPD, surfaced while looking
for prior art next to it.

### 1a. Root cause debate, unresolved, two competing theories

`crosspoint-reader#561`, "Potential mitigation for screen fading issue" --
the same bug we found (`eink-refresh-degradation.md`), reported with a
video on the X4. Quote: *"It does not happen if I cover the space center
above the lower-screen buttons with my thumb"* -- our own thumb-stencil
observation, independently reproduced by another X4 owner.

Two theories, both with supporting evidence, disagreeing with each other:

- **Bare-die driver IC.** @allgoewer (issue #561): the X4's SSD1677 is
  listed in its datasheet ordering information as a *gold bump die* -- an
  unpackaged chip with no resin over the silicon -- so photocurrent in the
  driver IC's own junctions is disrupted by light, not the panel's
  backplane. Would explain why covering the button area (near the driver)
  helped in that report.
- **The panel itself, IR rather than UV.** Issue `#2030`: @drinkingcoffee
  reports covering the *button area* changes nothing, but covering half
  the *screen* protects exactly that half; @pieroxy reports the same with
  the fix from 1b turned ON. Points at an Arduino-forum thread where the
  effect disappears behind an IR filter film. Sunscreen on the glass had
  no effect (rules out UV specifically).

**Our own stencil evidence sits with the second theory, not the first**:
the cardboard/thumb stencil in `eink-refresh-degradation.md` was placed on
the panel's viewing area, not over the driver IC or the button row, and
the protected shape tracked the stencil exactly. This is community
reporting, not vendor documentation -- unverified against the SSD1677
datasheet's actual die-packaging claim.

### 1b. The merged mitigation is already in our own tree, and it defaults off

`crosspoint-reader#603` (merged 2026-02-05) plus
`open-x4-epaper/community-sdk#15` (merged 2026-02-01). From #603's own
description: *"When set to ON, we will disable the display's analog
supply voltage after every update and turn it back on before the next
update."*

**Confirmed present in `firmware/explorink` today**, not a hypothetical
port:

- `src/CrossPointSettings.h:376` -- `uint8_t fadingFix = 0;` (comment:
  "Sunlight fading compensation"). **Default is off.**
- `src/SettingsList.h:233` -- exposed as a toggle, Settings -> Display ->
  "Sunlight Fading Fix".
- Threaded as `turnOffScreen` through `GfxRenderer::displayBuffer` ->
  `HalDisplay::displayBuffer` -> `FreeInkDisplay::displayBuffer` (e.g.
  `freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:500`) ->
  the panel driver's `refreshDisplay`.
- `lib/GfxRenderer/GfxRenderer.cpp:1575` notes the async refresh path has
  no turn-off-screen hook, so a `fadingFix` user is kept on the blocking
  path deliberately.

**This means BUG-022's stated verdict may be incomplete.** Whether the
original thumb-stencil field observation was taken with `fadingFix` on or
off is unrecorded. If it was off, the observation says nothing about
whether the existing mitigation works -- it has simply never been tried.
Tracked as **T-584** in `docs/TODO.md` (parent repo): repeat the stencil
protocol from `eink-refresh-degradation.md` with the setting in both
positions.

Caveat, so this is not oversold: upstream issue `#2030` has reports of
`fadingFix` staying ON and the panel still fading. A partial mitigation at
best, not a proven fix.

### 1c. An unmerged, more specific theory: under-driven pixels are the vulnerable state

`crosspoint-reader#2399`, "fix(x3): use `_normal` LUTs and settle delay
for sunlight fading fix" -- **not merged**, `merged_at` is null, a
proposal only. Its stated mechanism:

> `_fast` turbo LUTs have a short 4-frame drive phase -- particles don't
> reach stable B/W minima before POF cuts the charge pump, and sunlight
> nudges the shallow-state particles toward grey

Proposed fix: switch to the `_normal` 6-frame LUTs when the fading fix is
active, plus a 20 ms settle delay before the power-off command.

This ties the sunlight failure to something we actually control (drive
frame count and power-off timing), rather than only to light exposure
itself. It is compatible with, not contradictory to, the photoconductive
a-Si leakage model in `eink-refresh-degradation.md`: a pixel already left
in a shallow, not-fully-committed state by a short waveform has less
margin before light-induced leakage tips it back toward white.

**Independent corroboration inside FastEPD**, arrived at for unrelated
reasons: every one of its display update paths clocks out a full neutral
pass before cutting the DC/DC converter, and the comment is explicit that
skipping it breaks things --

```
// This clear to neutral step is necessary; do not remove
```

at `FastEPD.inl:3644` and `:3522`, exercised on every path
(`FastEPD.inl:2811`, `:3190`, `:3523`, `:3645`). FastEPD's public API also
defaults `bKeepOn = false` (`FastEPD.h:370-373`) -- it already powers the
panel down after every update by default, which is exactly what
`crosspoint-reader#603` had to add as an opt-in feature.

Tracked as **T-585** in `docs/TODO.md`, depending on T-584: only worth
building if the plain `fadingFix` toggle turns out insufficient on its
own.

## 2. Waveform / gray-matrix design -- prior art for a tunable grayscale loop

Context: `eink-grayscale.md` covers our own X4/X3 grayscale mechanism in
depth (BW base frame plus two overlay planes, `pGrayMatrix`-equivalent is
our `Ssd1677Luts.h`). This section is what FastEPD does differently and
what of it looks portable as a technique, not as code.

### 2a. No temperature compensation, and the wiki explains why that matters

`FastEPD.inl:58`: `// 8 columns by 16 rows. From white (15) to each gray
(0-black to 15-white) at 20C`. That "at 20C" is the only mention of
temperature anywhere in the ESP32 path (grep confirms nothing reads a
thermometer or scales pass count against one).

The wiki's "How does parallel eink work?" page (`Home.md`, around line
186) is the clearest short explanation of the physical mechanism found in
this pass, worth reading whole if we ever write our own from scratch.
Key claims, read directly off that page:

- Row data is 2 bits per pixel: `00 = floating/no-change, 01 = make
  darker, 10 = make lighter, 11 = skip`.
- 5 to 10 "pushes" move a pixel fully between black and white on an
  average panel.
- *"the temperature affects the viscosity of the oil. The colder the
  ambient temperature, the slower the granules move. This means that it
  will take more 'pushes'."*
- E Ink itself publishes a matrix per temperature and per gray-to-gray
  transition, *"sometimes more than 40 steps per color change"*.
- A vocabulary correction worth adopting in our own docs: *"This set of
  steps has mistakenly been called a 'waveform'. There's no waveform
  involved in eink displays -- it's a purely digital device with several
  fixed states."*

**Inferred, not claimed by FastEPD**: direct sun heats a panel well past
20 C, which is exactly the condition a fixed-temperature matrix handles
worst -- this is a plausible second contributing factor to BUG-022,
separate from and additive to the leakage mechanism, but nothing in
FastEPD says so; this is our own inference from reading their code
against our bug.

### 2b. The gray-matrix format itself: small, tunable, live-editable

Format: a 16 x N array of 2-bit codes, one row per gray level (0 = black
... 15 = white), one column per pass, values `0/1/2` meaning
neutral/darken/lighten. Pass count is derived from array size, not
declared separately: `iPasses = iMatrixSize / 16`
(`FastEPD.inl:1880`, `:2774`, `:3139`). `u8M5Matrix`
(`FastEPD.inl:173-190`, the table used for `BB_PANEL_LILYGO_T5PRO`) is 128
bytes for 8 passes.

At init the matrix is expanded into two 256-entry-per-pass lookup tables
that turn a whole packed source byte straight into output codes
(`FastEPD.inl:1891-1901`), so the per-row inner loop is four table lookups
per 4 output bytes (`:3162-3167`). `setCustomMatrix()`
(`FastEPD.inl:2578`) frees and rebuilds the tables, so the matrix is
swappable at runtime with no rebuild.

`examples/Arduino/gray_matrix_editor/gray_matrix_editor.ino` is a serial
CLI (`LIST/SHOW/COPY/SWAP/EDIT/UNDO/CODE/IMAGE`) that edits the matrix
live on the device and dumps C source. Its own header comment: *"A more
efficient way to interactively edit the 16-gray level table to achieve 16
good looking gray values on each EInk panel (they are all slightly
different)."*

**This is the closest prior art to `tools/style_watch.py`'s edit loop,
applied to a waveform instead of a style file.** If we ever build a
tunable-waveform iteration loop for our own grayscale work, this tool's
shape -- live edit, live redraw, dump source at the end -- is the one to
copy. Nothing here says we need to; it is a pattern reference.

### 2c. RAM and flash cost, scaled to our own 480x800

Scaled from `bbepSetPanelSize` (`FastEPD.inl:1843-1887`) and file-scope
statics (`FastEPD.inl:320-327`):

| buffer | where | cost at 480x800 |
|---|---|---|
| `pCurrent` (4bpp) | PSRAM, 16-aligned | 192 kB |
| `pPrevious` | aliased inside `pCurrent` | 0 extra |
| `pTemp` (2-bit output codes) | PSRAM, 16-aligned | 96 kB |
| `dma_buf` (ping-pong pair) | internal, `MALLOC_CAP_DMA` | ~272 B |
| `pGrayLower` + `pGrayUpper` | internal heap | 4 kB (at 8 passes) |
| `LUTW/B/BW_16` | `.bss` | 1.5 kB |
| `u8Cache` | `.bss` | 1 kB |
| gray matrix | flash | 128 B |

Internal RAM cost is under 7 kB total; the large buffers are PSRAM-only,
and PSRAM is mandatory for FastEPD -- `FastEPD.inl:29-31` is
`#error "Please enable PSRAM support"` if it is missing. Relevant data
point for the planned firmware memory audit
([[project_firmware_memory_audit_planned]]): a 16-level tunable waveform
itself costs almost nothing (128 B flash, ~4 kB derived RAM); the expense
in a design like this is the PSRAM framebuffer copies, not the waveform
table.

Two cost-relevant commits, both instructive regardless of whether we ever
touch this code: `9acf80b` "Reduced gray table sizes by 75%" (the gray
LUTs were `uint32_t`, became `uint8_t` -- the same shrink may apply
wherever we store similar tables), and `b248b07` "Added SIMD code, but
disabled it. It doesn't speed up operations since PSRAM latency overwhelms
the benefit" -- hand-written ESP32-S3 SIMD assembly (`src/s3_diff.S`, 150
lines) that was measured and abandoned. **Do not spend time on SIMD for a
PSRAM-resident framebuffer** -- someone already measured this exact
trade-off and it lost.

### 2d. No dithering in FastEPD -- the author's dithering answer is a different library

Nothing in `src/` dithers (grep for dither/floyd/bayer/halftone/ordered
returns nothing). Grayscale comes only from the 4bpp path plus the gray
matrix above. Dithering in FastEPD's own examples comes from bitbank2's
separate **JPEGDEC** library: Floyd-Steinberg error diffusion to 1, 2 or 4
bpp via `setPixelType(FOUR_BIT_DITHERED)` + `decodeDither()`, using a
16-line rolling error buffer
(`examples/esp_idf/www-image/main/jpgdec-render.cpp:152-154`, allocated at
`:414` as `width * 16` bytes in PSRAM). At our panel width that is a 7.7
kB rolling buffer. **If we ever want dithering prior art, JPEGDEC is the
thing to read, not FastEPD** -- our own dithering today is the 2x2
checkerboard in `GfxRenderer.cpp:863-870` (`eink-grayscale.md`, "There is
dithered grey too").

## 3. Bus contention and silent corruption -- relevant to T-576

Context: T-576 suspects SPI bus contention during GNSS polling silently
corrupts tile rendering (hatch/broken geometry, no crash) on the T5 S3
Pro. FastEPD is on a different bus (parallel, not SPI) so nothing here
transfers directly, but two things in it are useful precedent for the
*class* of failure.

### 3a. A documented ESP32-S3 hardware erratum that corrupts the start of every line

`FastEPD.inl:1711-1720`:

```c
// Optionally use bit banging for parallel I/O to get around a bug in
// the ESP32-S3's LCD hardware which generates a spurious clock cycle
// before the data is ready, causing a skipped or corrupted set of pixels
// at the start of each line
```

Added in commit `3783215` (2026-08-03), worked around by an opt-in
bit-banged mode (`bus_speed = BB_SPEED_BITBANG`). This is a silent,
per-line pixel corruption on the S3, with no crash -- confirmed by another
project, on the same MCU family we build for, that a peripheral erratum
rather than application logic can produce exactly T-576's symptom shape.
Does not confirm or explain T-576 itself; it establishes the symptom
class has real precedent on our silicon.

### 3b. No locking anywhere -- a cautionary pattern, not one to copy

There is no mutex, no `portENTER_CRITICAL`, no spinlock, no interrupt
disable anywhere in FastEPD's `src/` (verified by grep). Bus discipline is
a single `volatile bool dma_is_done` (`FastEPD.inl:326`), cleared before a
transmit (`:1648`), set by the DMA-done ISR callback (`:340`, `:349`), and
busy-waited at the top of the next row (`:1618-1620`,
`while (!dma_is_done) delayMicroseconds(1);`). Hot-path functions carry
`IRAM_ATTR` but nothing preemption-protected.

Worse specifically on the T5 S3 Pro's own code path: `LilyGoRowControl`
(`FastEPD.inl:724-756`) bit-bangs an 8-bit shift register over SDA/SCL/STR
(`bbepSendShiftData`, `:642`) for STV and latch **inside the per-row
loop**, with hard `delayMicroseconds(7/8/10/18)` timing and no critical
section anywhere. Any task that preempts mid-row stretches a CKV high
time unpredictably.

**This is the pattern to not copy, and it is a concrete argument for
wrapping our own scan loop against preemption** -- our T-576 contention
suspect (GNSS polling) is exactly the kind of preemption this design has
no defence against. It does not prove our bug has the same cause; it
shows a design of this shape is fragile to it.

## 4. Partial refresh: a real bug found, and a calibration for our own expectations

### 4a. Row-ranged partial refresh saves data, not scan time

`bbepPartialUpdate` (`FastEPD.inl:3624-3641`) still clocks out every row
at full width regardless of the requested `iStartRow`/`iEndRow` --
`for (i = 0; i < pState->native_height; i++)` runs unconditionally
(`:3624`, `:3636`). The row range only controls which rows receive a
non-neutral drive pattern; the panel's gate scan itself cannot be
shortened. The wiki claims *"this can increase the speed of the update"*
(`Home.md:84`), which read against the code is at best the memcpy cost
and at worst simply wrong. **Calibration for us**: budget any row-ranged
partial refresh design as a full-screen-duration operation, not a
proportionally faster one.

Timing constants, for reference: each pass is about 32 ms
(`FastEPD.inl:3614`), default 4 partial passes / 5 full passes
(`FastEPD.inl:1703-1704`), `setPasses()` clamps to 1..14
(`FastEPD.cpp:358-366`).

### 4b. A real bug in the ping-pong buffer, worth knowing if this design is ever ported

Same function, `FastEPD.inl:3624-3641`. `dma_buf` ping-pongs between two
halves (`iDMAOff ^= native_width/4`, line 3640), but rows outside the
requested band are zeroed only on the *first* skipped row
(`if (iSkipped == 0) memset(d, 0, ...)`, lines 3633-3635). Trace: a
content row writes half A; the first skipped row zeroes and writes half
B; the second skipped row reuses half A, which still holds the earlier
content row's diff pattern, and clocks it out unmodified. Every
subsequent skipped row alternates between a stale pattern and neutral
instead of staying neutral throughout.

This only bites when `iStartRow`/`iEndRow` actually narrow the update --
the feature we would specifically want if we ported this design. **If we
ever build a row-ranged partial refresh modeled on this one, do not copy
this loop as written.**

## 5. License and provenance

`LICENSE` is Apache-2.0, every source file carries the notice. Depending
on it, or porting a snippet with attribution, has no copyleft obligation
and is compatible with closed firmware.

**One open item**: GitHub issue `#5` (Jan 2025) is a formal LGPL-3.0
violation complaint from the founder of Soldered Electronics, alleging
FastEPD copied code from the Inkplate-Arduino-Library. bitbank2 disputed
most specifics as misidentified and said he would make changes so no
Inkplate attribution would be necessary; the issue was closed with no
license change recorded. Unresolved by assertion, not adjudicated.

**Practical consequence for us**: the specifically Inkplate-derived code
paths are the ones under that cloud -- the `Inkplate6Plus*` /
`Inkplate5V2*` / `Inkplate10*` power and row-control functions
(`FastEPD.inl:1043-1143`, `:1277-1329`, `:1440-1516`, `:1517-1602`). The
parts that are actually interesting to us -- the gray-matrix expansion,
the pass loops, the diff/LUT partial-update logic, and the `LilyGo*`
functions -- are not what issue #5 named. If we ever port anything from
here rather than just reading it, prefer the non-Inkplate files, or
reimplement the documented mechanism from scratch instead of lifting an
Inkplate-shaped file.

## 6. Two smaller findings

- **A known crash on 960x540 in 4bpp mode, fixed, confirmed on our exact
  reference board.** Issue `#29` (closed): `bbepBackupPlane` copied
  `(width/2) * height` bytes into `pPrevious`, which is aliased at offset
  `(width/4) * height` inside `pCurrent` -- a 129,600-byte overrun on a
  960x540 panel, producing both a repeated vertical strip on the right
  edge and heap corruption that surfaced only after Wi-Fi activity.
  Reported and confirmed fixed **on LilyGo T5 4.7" S3 Pro hardware** --
  our own reference board, under a different owner. Current guard:
  `FastEPD.inl:3666`. Two lessons regardless of our own driver: the
  `pPrevious`-aliased-inside-`pCurrent` trick is a trap for any
  size-derived buffer math, and a visual artifact plus heap corruption
  can be the same underlying bug wearing two faces.
- **VCOM is not software-controlled on our board type.**
  `bbepSetPanelSize` takes an `iVCOM` parameter, and the TPS65186-based
  boards write it over I2C (`FastEPD.inl:841-846`, `:904-908`), but
  `LilyGoEinkPower` (`:663-703`) does no I2C at all -- it only toggles
  shift-register bits. The `-1600` VCOM value in the T5PRO panel
  definition (`FastEPD.inl:226-227`) is therefore inert on this board.
  **If we ever want VCOM as a temperature-compensation knob** (relevant
  to 2a above), it is not reachable through this hardware path on the T5
  S3 Pro.

## What FastEPD does not have, checked and confirmed absent

No dithering algorithm of its own, no LUT/waveform file format beyond the
16xN gray matrix above, no temperature sensing anywhere in the ESP32
path, no light or optical handling of any kind, and no issue in its own
tracker mentioning white/blank refreshes outdoors. Its wiki is two pages:
the API reference and a page of two hand-tuned matrices for the ED115OC1
and ED054TC4 panels -- neither is ours. Everything about the sunlight
problem in section 1 came from `crosspoint-reader` and
`open-x4-epaper/community-sdk`, not from this library.
