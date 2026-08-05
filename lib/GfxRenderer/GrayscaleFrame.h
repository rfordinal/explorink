#pragma once

#include <HalDisplay.h>

#include <cstdint>

#include "GfxRenderer.h"
#include "GrayShade.h"

// Four-level grey on the X4 panel, packaged so drawing code never has to know
// the plane encoding. docs/eink-grayscale.md is the mechanism; this is the API.
//
// The three facts this hides, all of which are easy to get wrong by hand:
//
// 1. A grey pixel is a BLACK pixel nudged lighter. Black, dark grey and light
//    grey are all ink in the BW base frame; two extra bit planes then lighten
//    the grey ones. Draw a shade and forget the base frame is involved.
// 2. Plane bits are inverted relative to the framebuffer: in a plane, a SET bit
//    means "nudge this pixel", so plane passes clear to 0x00 and draw with
//    state=false. GrayPainter picks the right state per pass.
// 3. renderMode only changes glyph/bitmap DECODE, not geometry
//    (GfxRenderer.cpp:448-458). Lines and fills are mode-blind, so grey
//    geometry is the caller's job — which is exactly what the shade argument on
//    every GrayPainter method takes off the caller's hands.
//
// Cost: one 8,000 byte scratch band, allocated and freed inside render(). Not
// the 96,000 bytes two whole planes would take.

// GrayShade, GrayPass and the encoding itself live in GrayShade.h -- no
// renderer, no panel, host-testable (test/grayscale_shades).

// Handed to the draw callback once per pass. Every method takes a GrayShade and
// translates it into the right pixel state for the pass in flight, so the same
// callback body produces the BW base frame and both grey planes with no
// branching in the caller.
//
// Shades paint opaquely, like normal drawing: a later shape overwrites an
// earlier one in every pass, including White, which clears both plane bits and
// the base ink. So a white halo under a marker works the way it does in BW code.
class GrayPainter {
 public:
  using Pass = GrayPass;

  GrayPainter(GfxRenderer& renderer, Pass pass) : renderer_(renderer), pass_(pass) {}

  Pass pass() const { return pass_; }

  // Does `shade` lay down ink (base pass) or a nudge flag (plane passes) here?
  // Informational — the draw methods handle both answers. Useful to skip work
  // that would draw nothing visible.
  bool inks(const GrayShade shade) const { return grayInks(shade, pass_); }

  // The pixel state to draw `shade` with in this pass. See GrayShade.h: plane
  // passes are inverted relative to the framebuffer.
  bool stateFor(const GrayShade shade) const { return grayPixelState(shade, pass_); }

  void pixel(int x, int y, GrayShade shade) const;
  void line(int x1, int y1, int x2, int y2, GrayShade shade) const;
  void line(int x1, int y1, int x2, int y2, int lineWidth, GrayShade shade) const;
  void rect(int x, int y, int w, int h, int lineWidth, GrayShade shade) const;
  void fillRect(int x, int y, int w, int h, GrayShade shade) const;
  void polygon(const int* xPoints, const int* yPoints, int numPoints, GrayShade shade) const;
  // Text at a uniform shade. In a plane pass the glyph is decoded as BW (see
  // the .cpp) so the whole glyph lands in the plane; an anti-aliased font's
  // edge pixels take the same shade as its body rather than their own level.
  void text(int fontId, int x, int y, const char* text, GrayShade shade,
            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void centeredText(int fontId, int y, const char* text, GrayShade shade,
                    EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Escape hatch for anything this wrapper does not cover. Callers that use it
  // must handle inks()/inkState() themselves.
  GfxRenderer& raw() const { return renderer_; }

 private:
  GfxRenderer& renderer_;
  Pass pass_;
};

// Plain function pointer plus context, not std::function: this is library code
// on the render path and CLAUDE.md bans the ~2-4 KB per signature std::function
// costs. Use a static member or a file-local function as `fn` and `this` as
// `ctx`.
struct GrayDrawCallback {
  void* ctx = nullptr;
  void (*fn)(void* ctx, const GrayPainter& painter) = nullptr;
};

// Where a plane band goes instead of the controller. `rows` holds
// panelWidthBytes * numRows bytes for physical rows [yStart, yStart + numRows),
// same layout as the framebuffer, valid only for the duration of the call.
// Bands arrive in y order, LSB plane first, then MSB. Used by the screenshot
// channel to get grey off the device; see GrayscaleFrame::replayPlanes.
struct GrayPlaneSink {
  void* ctx = nullptr;
  void (*fn)(void* ctx, bool lsbPlane, const uint8_t* rows, int yStart, int numRows) = nullptr;
};

class GrayscaleFrame {
 public:
  // One band of physical rows at a time: 100 bytes/row * 80 = 8,000 bytes.
  static constexpr int STRIP_ROWS = 80;

  struct Timings {
    uint16_t baseDrawMs = 0;     // callback, Base pass
    uint16_t baseDisplayMs = 0;  // BW base refresh (HALF, ~1720 ms on X4)
    uint16_t planesMs = 0;       // callback x (bands x 2 planes) + streaming to RAM
    uint16_t grayDisplayMs = 0;  // the grey nudge itself
    uint16_t cleanupMs = 0;      // RED RAM resync, not optional
    uint16_t totalMs = 0;
    bool grayscale = false;  // false = panel cannot do strip grey, BW frame only
  };

  static bool supported(const GfxRenderer& renderer) { return renderer.supportsStripGrayscale(); }

  // Full frame: BW base + both planes + the nudge + cleanup. The callback runs
  // once for the Base pass and once per band per plane (13 times on a 480-row
  // panel), so it must be cheap and must draw the same picture every time.
  //
  // When the panel cannot do strip grayscale (X3 in inverted mode, other
  // panels), this degrades to a plain BW frame: every grey reads BLACK, not
  // white, because grey shares the base frame's ink. Timings.grayscale says
  // which happened.
  //
  // Leaves the controller's differential baseline clean, so the next windowed
  // or fast update behaves normally.
  static Timings render(GfxRenderer& renderer, const GrayDrawCallback& draw,
                        HalDisplay::RefreshMode baseMode = HalDisplay::HALF_REFRESH);

  // Planes only: no new BW base, no framebuffer touch. Nudges the pixels the
  // callback marks on top of whatever is already on the glass, and leaves the
  // rest of the panel alone (plane bit 0 selects LUT slot 00, which is zero
  // volts — Ssd1677Luts.h:12-13). Use to add grey to a region without
  // repainting the screen.
  //
  // OPEN, unmeasured: what a second nudge does to an already-nudged pixel. The
  // grey LUT is differential and calibrated against a base state, and nothing
  // in the driver says the nudge is idempotent. The Preview activity's Drift
  // page exists to answer this on glass; until it does, treat repeat nudges of
  // the same pixel as unknown.
  static Timings nudge(GfxRenderer& renderer, const GrayDrawCallback& draw);

  // --- Getting grey off the device ---------------------------------------
  //
  // The panel's grey lives in controller RAM and in the physical particles, not
  // in any buffer the firmware keeps: the planes are streamed out band by band
  // and the scratch is freed. `CMD:SCREENSHOT` therefore only ever saw the BW
  // frame, where a grey pixel is black.
  //
  // Rather than keep a 96,000 byte shadow of two planes, the last full frame's
  // draw callback is remembered (8 bytes) and its planes are re-rendered on
  // demand into whatever sink asks for them. Same 8 KB band scratch, same
  // callback, so the planes are bit-identical to what was sent to the panel.

  // True when a full grey frame has been rendered and its source is still
  // valid, i.e. replayPlanes() has something to replay.
  static bool hasSource();
  // False when nudge() has run since the last full frame: the panel then carries
  // grey that no single callback reproduces, so a replay is a subset of what is
  // on the glass. Meaningless unless hasSource().
  static bool sourceIsExact();
  // Forget the remembered callback. MUST be called when the object behind its
  // ctx pointer dies -- ActivityManager::exitActivity does this for activities.
  static void clearSource();

  // Re-render the last full frame's planes into `sink` instead of the
  // controller. Does not touch the framebuffer, the panel, or controller RAM.
  // Returns false when there is no source or the band scratch cannot be
  // allocated.
  //
  // The caller must hold the render lock: this drives the renderer's strip
  // target, which the render task also uses.
  static bool replayPlanes(GfxRenderer& renderer, const GrayPlaneSink& sink);

 private:
  // Shared plane loop: renders both planes band by band. Each band goes to the
  // controller, or to `sink` when one is given (nothing reaches the panel
  // then). Returns false on scratch OOM.
  static bool writePlanes(GfxRenderer& renderer, const GrayDrawCallback& draw, const GrayPlaneSink* sink = nullptr);
};
