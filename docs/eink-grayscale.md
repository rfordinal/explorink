# E-ink grayscale on X4

The X4 panel does **4 grey levels**, not 1 bit. Black, dark grey, light grey,
white. This doc is the mechanism, because the mechanism is not obvious and the
wrong mental model ("panel is monochrome") is written into several comments and
docs across both repos.

Everything below marked **verified** is read off the code. Everything marked
**open** needs a hardware measurement and must not be built on until measured.

## The panel

X4 is an SSD1677. X3 is a UC8253. Both drivers are linked into one binary; the
panel is picked at runtime, not by `#ifdef` — `lib/hal/HalDisplay.cpp:15-17`
calls `setDisplayX3()` only when `HalGPIO::deviceIsX3()`.

`supportsStripGrayscale()` is `true` on both
(`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.h:87`,
`.../Uc8253X3Driver.h:70`). Base class default is `false`
(`.../PanelDriver.h:103`) — that is what other panels get.

X4 needs no preconditioning pass. X3 does: `preconditionGrayscale()` is an OEM
"AA-pre-BW(mid)" settle that leaves particles receptive to the weak grey nudge
(`.../Uc8253X3Driver.cpp:323-330`). X4 does not override it, so it falls to the
no-op default (`.../PanelDriver.h:117-119`). X4's grey LUT carries its own drive
phases instead.

## A grey pixel is a black pixel that got nudged lighter

**Verified.** Grey is not a value written to a pixel. It is a BW base frame plus
a weak differential waveform applied through two extra bit planes.

`lib/GfxRenderer/GfxRenderer.cpp:446-459` is the encoder:

```cpp
// 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

if (renderMode == GfxRenderer::BW && bmpVal < 3) {
  renderer.drawPixel(screenX, screenY, pixelState);
} else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
  // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
  renderer.drawPixel(screenX, screenY, false);
} else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
  renderer.drawPixel(screenX, screenY, false);
}
```

BW mode draws when `bmpVal < 3`. So black **and both greys** are black in the BW
base. The planes then lighten them.

Consequence that bites: **lose the grey and the pixel reads full black, not
white.** Contrast collapses, it does not wash out.

Per-pixel encoding:

| pixel | LSB plane | MSB plane | LUT slot | result |
|---|---|---|---|---|
| black or white | 0 | 0 | `00` | no drive — keeps BW base |
| light grey | 0 | 1 | `10` | mid nudge |
| dark grey | 1 | 1 | `11` | stronger nudge |
| unused | 1 | 0 | `01` | — |

Naming trap: "LSB" is BW RAM (`0x24`), "MSB" is RED RAM (`0x26`). The names are
about the grey-level bit, not the RAM plane. MSB is the superset — set for both
greys.

## LUT slot 00 does not drive. Grey is an overlay.

**Verified.** `freeink-sdk/libs/display/FreeInkDisplay/src/lut/Ssd1677Luts.h:12-13`:

```c
// 00 black/white
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
```

Zero volts. A pixel with both plane bits clear does not move during a grey
refresh. It holds whatever is physically on the glass.

This is the load-bearing fact for partial-update work: a grey refresh is not
"repaint the screen". It drives **only the pixels the planes mark**. Zero the
planes outside a region and a nominally full-screen grey refresh changes only
that region. No driver change needed for that.

The grey waveform is short: 3 active TP/RP groups of 1 frame each
(`Ssd1677Luts.h:24-26`), frame rate `0x8F`. Compare the factory absolute LUTs at
60 and 50 frames (`Ssd1677Luts.h:91-92`). Documented BW baselines for reference:
FULL ~1800 ms, PART/fast ~500 ms (`Ssd1677Driver.cpp:48-50`).

## `_inGrayscaleMode` is a landmine for windowed updates

**Verified.** After a differential grey refresh, RED RAM holds the grey MSB
plane, not the previous frame. So the differential compare has no valid
baseline. `displayGray` sets the flag (`Ssd1677Driver.cpp:510`).

Two things then happen automatically, and both are expensive:

`Ssd1677Driver.cpp:434-437` — a windowed update **throws the window away**:

```cpp
if (_inGrayscaleMode) {
    displayImpl(bus, fb, nullptr, RefreshMode::Half, turnOff, /*async=*/false);
    return;
}
```

Full-frame HALF (~1720 ms) painted from the BW framebuffer. Every grey on screen
is gone, whole panel, and you paid the slowest refresh to lose it.

`Ssd1677Driver.cpp:389-394` — a FAST full-frame update is promoted to HALF for
the same reason.

**Always call `cleanupGrayscaleBuffers(bwFrame)` after a grey render.**
`Ssd1677Driver.cpp:539-548` writes the BW frame back into RED RAM as the clean
differential baseline and clears the flag. That is the whole cleanup — stock
parity, the OEM firmware has no revert waveform at all (`Ssd1677Luts.h:44-46`).

## Windowed BW updates preserve grey

**Verified from the differential model, not yet measured on glass.**

After cleanup, a windowed update takes the normal path
(`Ssd1677Driver.cpp:439-470`). `refresh(Fast)` is differential — it drives only
pixels where BW RAM differs from RED RAM. `Ssd1677Driver.cpp:356-358` states it
plainly: *"a partial only drives pixels that differ from the RED 'old' plane, so
it can't clear what is physically on the panel at boot"*.

So:

- outside the window — not addressed, grey untouched
- inside the window, pixel unchanged in the BW frame — diff 0, not driven, grey
  survives
- inside the window, pixel changed — driven to pure black or white, grey lost at
  exactly those pixels

Grey does not vanish when you move something. It degrades to black only on the
pixels the BW frame actually changed.

Bench: **Marker move** page, via `GfxRenderer::displayBufferWindow`. Grey
backdrop, BW marker, one window refresh per step.

Window constraint: `x` and `w` must be multiples of 8 (`Ssd1677Driver.cpp:427`).

## Call sequences

Simplest correct grey render — `src/activities/boot_sleep/SleepActivity.cpp:226-251`:

```
draw BW into framebuffer
displayGrayscaleBase(HALF_REFRESH)
clearScreen(0x00); setRenderMode(GRAYSCALE_LSB); draw; copyGrayscaleLsbBuffers()
clearScreen(0x00); setRenderMode(GRAYSCALE_MSB); draw; copyGrayscaleMsbBuffers()
displayGrayBuffer()
setRenderMode(BW)
```

SleepActivity draws the planes into the framebuffer, destroying the BW frame, and
never cleans up. Fine there — device is going to sleep. **Do not copy that.**

Memory-lean version, keeps the BW framebuffer intact —
`src/activities/reader/EpubReaderActivity.cpp:1660-1700`:

```
setRenderMode(GRAYSCALE_LSB);
for (y = 0; y < h; y += STRIP_ROWS) {
  beginStripTarget(scratch, y, rows); clearScreen(0x00); drawPass(); endStripTarget();
  writeGrayscalePlaneStrip(true, scratch, y, rows);
}
setRenderMode(GRAYSCALE_MSB);   // same loop, writeGrayscalePlaneStrip(false, ...)
setRenderMode(BW);
displayGrayBuffer();
cleanupGrayscaleWithFrameBuffer();
```

RAM cost, three tiers, no PSRAM on ESP32-C3 so all internal DRAM:

| tier | extra DRAM |
|---|---|
| two whole planes | 96,000 B |
| one plane, reused | 48,000 B |
| banded strips (`STRIP_ROWS = 80`) | 8,000 B |

Whole-plane tiers exist only to overlap plane rendering with an async BW refresh,
and are gated on free heap (`EpubReaderActivity.cpp:1611-1621`). **Grey on X4
costs 8 KB if you band it.** Prefer banding.

## The support layer — draw a shade, not a plane

**Verified by construction; the encoding is unit-tested on the host
(`test/grayscale_shades/GrayShadeTest.cpp`), the panel behaviour is not.**

Nothing above needs to be in a caller's head any more. Three files:

| file | what it is |
|---|---|
| `lib/GfxRenderer/GrayShade.h` | `GrayShade`, `GrayPass`, the encoding as `constexpr`. No renderer, no panel, host-testable. |
| `lib/GfxRenderer/GrayscaleFrame.h` | `GrayPainter` (shade-aware drawing) and `GrayscaleFrame` (the call sequence). |
| `lib/GfxRenderer/GrayscaleFrame.cpp` | the banded plane loop, the cleanup, the timings. |

`GrayShade` is `White`, `LightGray`, `DarkGray`, `Black`. Draw with it:

```cpp
static void drawFn(void* ctx, const GrayPainter& g) {
  g.fillRect(20, 20, 200, 80, GrayShade::DarkGray);
  g.line(0, 120, 480, 120, 2, GrayShade::LightGray);
  g.text(UI_12_FONT_ID, 20, 140, "label", GrayShade::Black);
}
GrayscaleFrame::render(renderer, GrayDrawCallback{ctx, &drawFn});
```

The callback runs once for the base frame and once per band per plane — 13 times
on a 480-row panel — and `GrayPainter` maps the shade to the right pixel state
each time (`GrayShade.h`, `grayPixelState`). It must draw the same picture every
call and it must be cheap.

What the layer guarantees:

- **Banded.** 8,000 bytes of scratch, allocated and freed inside the call
  (`GrayscaleFrame.cpp`, `STRIP_ROWS = 80`). Never 96,000.
- **Cleanup is not optional and not the caller's job.**
  `cleanupGrayscaleWithFrameBuffer()` runs before `render()`/`nudge()` return, so
  the next windowed or fast update is not silently promoted to a full HALF.
- **Shades paint opaquely.** A later shape overwrites an earlier one in every
  pass, `White` included — it clears both plane bits and the base ink. A white
  halo under a marker works exactly as it does in BW code.
- **Text works.** In a plane pass the glyph is decoded as BW on purpose
  (`GrayscaleFrame.cpp`, `GrayPainter::text`), because the grayscale decode path
  drops everything but its own AA levels and a 1-bit font would put nothing in
  the plane at all. An AA font's edge pixels take the body's shade.
- **Timings come back measured**, per stage, in `GrayscaleFrame::Timings`.

`GrayscaleFrame::nudge()` is the planes-only form: no new base frame, no
framebuffer touch. It adds grey to whatever is already on the glass and leaves
everything outside the marked pixels alone, which is what LUT slot 00 buys.
Repeat nudges of the *same* pixel are still unmeasured — see below.

Fallback: when `supportsStripGrayscale()` is false (X3 inverted, other panels),
`render()` displays the base frame and `Timings.grayscale` comes back false.
Every grey then reads **black**, because grey shares the base frame's ink.

### Windowed BW updates are now reachable from the renderer

`GfxRenderer::displayBufferWindow(x, y, w, h)` (`GfxRenderer.cpp`) takes a
**logical** rect, rotates it to panel memory and snaps the memory x extent to a
multiple of 8 via the existing `screenRectToAlignedMemRect`, then calls
`HalDisplay::displayWindow` → `FreeInkDisplay::displayWindow`
(`FreeInkDisplay.cpp:685`). Callers no longer deal with alignment or
orientation. Returns false when the rect is empty or fully offscreen.

The SDK entry point was always there; the renderer's wrapper was commented out
(`GfxRenderer.h`, "EXPERIMENTAL"). It is live now and the Marker page below is
what exercises it over grey.

## The Preview activity — the bench for the open questions

`src/activities/preview/PreviewActivity.{h,cpp}`, reached from **Home →
Preview**. Five pages, `LEFT`/`RIGHT` to page, `CONFIRM` for the page's action,
either side button to repaint from a clean base, `BACK` to leave. Each page
prints its own instruction and the measured stage timings.

| page | what it puts on the glass | what it settles |
|---|---|---|
| Grey scale | all four levels as fills, as text, as 1–4 px lines, plus light/dark adjacent with no white between | are four levels actually distinguishable, and at what stroke width |
| Marker move | grey backdrop, BW marker stepped along a black route by `displayBufferWindow` | does grey survive outside the window, and what exactly the window destroys inside |
| Nudge drift | two identical dark grey patches; the right one re-nudged on every press | does a repeated nudge drift (the main risk for partial grey) |
| Region nudge | black field, then a nudge marking one region, then a smaller one inside it | is the grey overlay really region-limited (LUT slot 00 = no drive) |
| Grey vs dither | real grey next to the 2x2 checkerboard at the same sizes, fills and 1 px lines | which one the map style should use, per feature |

Grey cannot be captured over the screenshot channel (next section), so the
record is a photograph of the panel. Note the page name and the timings line
when you take one.

A detail worth knowing when reading the Marker page: the plane writes window
each band via `setRamArea`, so the RAM area is left at the *last band* before
`displayGrayBuffer()`. If the window bound the grey refresh, only that band
would grey — the whole-page grey on the Grey scale page is therefore already
evidence on open question 1 below, before any button is pressed.

## Open questions — measure before building

**Does the RAM window bound the grey refresh?** `setRamArea` writes `0x44`/`0x45`
(`Ssd1677Driver.cpp:217-227`). `refresh()` never touches them
(`Ssd1677Driver.cpp:243-324`), and `displayGray` does not reset the area
(`FreeInkDisplay.cpp:705-716`). So the window from the last `setRamArea`
survives into the grey refresh — *if* SSD1677 honours it in this mode. Unmeasured.

Bench: **Grey scale** page of the Preview activity. Whole-page grey means the
window does not bound the refresh; grey in one vertical band only (a physical
band is a vertical stripe in portrait) means it does.

Reason to doubt it: the reader's tiled path writes all bands then calls
`displayGrayBuffer()`, leaving the area set to the last band. If the window bound
the refresh, the reader would only refresh its last band, which it visibly does
not. Either the window does not bound the refresh, or something else resets it.
Resolve this before relying on either.

It only affects cost, not correctness — LUT slot 00 already gives region-limited
drive. But it decides whether restoring grey in a small region needs 2 × 48 KB of
plane zeroing per update.

**Does a repeated nudge drift?** The grey LUT is differential and calibrated
against a specific base state. `SleepActivity.cpp:227-231` says a HALF base is
required and a FULL base gives blotchy greys. The reader's image path says the
opposite for its case (`EpubReaderActivity.cpp:1539-1546`) — both are
hardware-observed, so this is empirical, not derivable.

Nothing states what happens when the same pixel is nudged twice without
returning to base. If it drifts darker, any scheme that re-applies grey to a
region must first drive that region back to BW base. Unmeasured, and it is the
main risk for partial grey updates.

Bench: **Nudge drift** page. Two identical dark grey patches, the right one
nudged again on every `CONFIRM`. They match or they do not, and the counter says
how many nudges it took to tell.

**Grey residue ghosts the next frame.** A plain fast diff cannot clear it. The
reader forces the following page onto the HALF cleanup path —
`EpubReaderActivity.cpp:1560-1567`, upstream issue #2190. Any activity that greys
a region needs the same cadence trick.

## Measuring grey needs eyes, not the screenshot channel

`CMD:SCREENSHOT` dumps the 48,000-byte framebuffer. That is the **BW frame**, not
what is on the glass. Grey levels are invisible to it. Verify grey visually or
photographically.

Worse than invisible: in the framebuffer a grey pixel is **black**, so a
screenshot of a grey page reads as a solid black page. Do not file that as a
rendering bug — check the panel.

The Preview activity is the way to get something worth photographing. Its
timings line is the only part of a grey render that *is* machine-readable, and it
goes to the serial log too (`LOG_DBG("PRV", ...)`).

## Gotchas

- **Inverted display mode kills grey silently.** `supportsStripGrayscale()` ANDs
  in `!_inverted` (`FreeInkDisplay.cpp:769-771`), and every grey call early-returns
  when `_inverted` (`FreeInkDisplay.cpp:711, 721, 737, 744, 750, 756, 763`).
  Always gate on `supportsStripGrayscale()`.
- **`renderMode` does not affect geometry.** It only changes glyph and bitmap
  decode (`GfxRenderer.cpp:448-458`, `:1345-1350`). `drawPixel`, `drawLine` and
  `fillPolygon` are mode-blind — in `GRAYSCALE_LSB` a `drawLine(..., true)` sets
  the same bit it would in BW. Drawing grey *geometry* means the caller decides,
  per plane, whether each feature's ink is on. Text and bitmaps get this free;
  lines and polygons do not.
- **Stale comment, do not trust it.**
  `src/activities/reader/XtcReaderActivity.cpp:352` and `:364` say "In LUT: 0 bit
  = apply gray effect, 1 bit = untouched". Inverted. Its own code sets bits on
  grey pixels, matching `GfxRenderer.cpp:454`.
- **X4 has no revert waveform. X3 does.** `grayscaleRevert()` is a real driver
  method (`.../PanelDriver.h:140`) but only X3 overrides it
  (`.../Uc8253X3Driver.cpp:454`), where it scrubs the panel white and resets both
  DTM planes (`.../Uc8253X3Driver.cpp:295-299`). X4 deliberately does not —
  `.../Ssd1677Driver.h:95`: *"No grayscaleRevert override: stock parity — the OEM
  firmware has no revert"*. On X4 the only ways out of grey are the RED resync
  (`cleanupGrayscaleBuffers`) or a single-pass HALF clean. Do not write X4 code
  that assumes a revert exists.
- **Second, absolute grey mode exists.** `lut_factory_fast` / `lut_factory_quality`
  from OEM firmware, via `displayGrayBuffer(..., lut, factoryMode=true)`
  (`Ssd1677Luts.h:89-92`, `Ssd1677Driver.cpp:518-528`). Self-cleaning, sets no
  `_inGrayscaleMode`. Nothing calls it with `factoryMode=true` today. For
  standalone full-screen images, not overlays.

## There is dithered grey too, and it is a different product

`GfxRenderer.h:27` has `enum Color { Clear, White, LightGray, DarkGray, Black }`.
Despite the header claiming a 4x4 Bayer matrix (`GfxRenderer.h:25-26`), the
implementation is a 2x2 checkerboard (`GfxRenderer.cpp:863-870`) with a
`// TODO: maybe find a better pattern?`.

Dither is only BW pixels in a pattern. No planes, no custom LUT, no
`_inGrayscaleMode`, no cleanup, one refresh, zero extra RAM. Windowed updates work
with no caveats at all.

Cost: it needs area. A 1 px line under a 2x2 checker becomes a dashed line, not a
grey line. Works for fills and wide strokes. At 220 PPI the pattern period is
~0.23 mm, which reads as grey.

Blocker if you want dithered geometry: `drawLine` and `fillPolygon` take
`bool state`, not `Color` (`GfxRenderer.h:214, 232`). Only the `fillRect*` /
`fillRoundedRect` / `fillArc` family accepts `Color`.

Pick per use case. Real 4-level grey for still images and rarely-redrawn base
layers. Dither where windowed updates must stay cheap.
