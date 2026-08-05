#include "GrayscaleFrame.h"

#include <Logging.h>
#include <Memory.h>

void GrayPainter::pixel(const int x, const int y, const GrayShade shade) const {
  renderer_.drawPixel(x, y, stateFor(shade));
}

void GrayPainter::line(const int x1, const int y1, const int x2, const int y2, const GrayShade shade) const {
  renderer_.drawLine(x1, y1, x2, y2, stateFor(shade));
}

void GrayPainter::line(const int x1, const int y1, const int x2, const int y2, const int lineWidth,
                       const GrayShade shade) const {
  renderer_.drawLine(x1, y1, x2, y2, lineWidth, stateFor(shade));
}

void GrayPainter::rect(const int x, const int y, const int w, const int h, const int lineWidth,
                       const GrayShade shade) const {
  renderer_.drawRect(x, y, w, h, lineWidth, stateFor(shade));
}

void GrayPainter::fillRect(const int x, const int y, const int w, const int h, const GrayShade shade) const {
  renderer_.fillRect(x, y, w, h, stateFor(shade));
}

void GrayPainter::polygon(const int* xPoints, const int* yPoints, const int numPoints, const GrayShade shade) const {
  renderer_.fillPolygon(xPoints, yPoints, numPoints, stateFor(shade));
}

// Glyphs are decoded, not plotted, and the decoder drops everything but its own
// anti-aliasing levels while renderMode is GRAYSCALE_* (GfxRenderer.cpp:448-458)
// -- a 1-bit font would put no pixels in a plane at all. Force BW decode for the
// duration so the whole glyph lands in the plane, then restore the mode the
// plane loop set. Geometry is unaffected either way: renderMode does not reach
// drawPixel.
void GrayPainter::text(const int fontId, const int x, const int y, const char* text, const GrayShade shade,
                       const EpdFontFamily::Style style) const {
  const auto previousMode = renderer_.getRenderMode();
  if (previousMode != GfxRenderer::BW) renderer_.setRenderMode(GfxRenderer::BW);
  renderer_.drawText(fontId, x, y, text, stateFor(shade), style);
  if (previousMode != GfxRenderer::BW) renderer_.setRenderMode(previousMode);
}

void GrayPainter::centeredText(const int fontId, const int y, const char* text, const GrayShade shade,
                               const EpdFontFamily::Style style) const {
  const auto previousMode = renderer_.getRenderMode();
  if (previousMode != GfxRenderer::BW) renderer_.setRenderMode(GfxRenderer::BW);
  renderer_.drawCenteredText(fontId, y, text, stateFor(shade), style);
  if (previousMode != GfxRenderer::BW) renderer_.setRenderMode(previousMode);
}

bool GrayscaleFrame::writePlanes(GfxRenderer& renderer, const GrayDrawCallback& draw) {
  const int panelRows = renderer.getDisplayHeight();
  const int rowBytes = renderer.getDisplayWidthBytes();
  const size_t scratchBytes = static_cast<size_t>(rowBytes) * STRIP_ROWS;

  auto scratch = makeUniqueNoThrow<uint8_t[]>(scratchBytes);
  if (!scratch) {
    LOG_ERR("GRAY", "OOM: grayscale strip scratch (%u bytes)", static_cast<unsigned>(scratchBytes));
    return false;
  }

  // The strip writes below talk to the controller directly, so nothing may be
  // in flight (no-op on blocking panels).
  renderer.waitRefreshComplete();

  for (int plane = 0; plane < 2; ++plane) {
    const bool lsbPlane = (plane == 0);
    renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
    const GrayPainter painter(renderer, lsbPlane ? GrayPainter::Pass::Lsb : GrayPainter::Pass::Msb);

    for (int y = 0; y < panelRows; y += STRIP_ROWS) {
      const int bandRows = (panelRows - y < STRIP_ROWS) ? (panelRows - y) : STRIP_ROWS;
      renderer.beginStripTarget(scratch.get(), y, bandRows);
      renderer.clearScreen(0x00);  // 0 = leave this pixel alone
      draw.fn(draw.ctx, painter);
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsbPlane, scratch.get(), y, bandRows);
    }
  }

  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

GrayscaleFrame::Timings GrayscaleFrame::render(GfxRenderer& renderer, const GrayDrawCallback& draw,
                                               const HalDisplay::RefreshMode baseMode) {
  Timings timings;
  if (draw.fn == nullptr) return timings;

  const uint32_t tStart = millis();

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  draw.fn(draw.ctx, GrayPainter(renderer, GrayPainter::Pass::Base));
  const uint32_t tDrawn = millis();
  timings.baseDrawMs = static_cast<uint16_t>(tDrawn - tStart);

  if (!supported(renderer)) {
    // No grey available: the base frame is all this panel gets, and every grey
    // in it reads BLACK, because grey shares the base frame's ink.
    renderer.displayBuffer(baseMode);
    timings.baseDisplayMs = static_cast<uint16_t>(millis() - tDrawn);
    timings.totalMs = static_cast<uint16_t>(millis() - tStart);
    return timings;
  }

  renderer.displayGrayscaleBase(baseMode);
  const uint32_t tBase = millis();
  timings.baseDisplayMs = static_cast<uint16_t>(tBase - tDrawn);

  // OEM settle pass that leaves particles receptive to the weak nudge. Real on
  // X3, a no-op on X4 (PanelDriver.h:117-119).
  renderer.preconditionGrayscale();

  if (!writePlanes(renderer, draw)) {
    // Base frame is on the panel and the controller's RED plane still holds it,
    // so nothing needs undoing -- there is just no grey this frame.
    timings.planesMs = static_cast<uint16_t>(millis() - tBase);
    timings.totalMs = static_cast<uint16_t>(millis() - tStart);
    return timings;
  }
  const uint32_t tPlanes = millis();
  timings.planesMs = static_cast<uint16_t>(tPlanes - tBase);

  renderer.displayGrayBuffer();
  const uint32_t tGray = millis();
  timings.grayDisplayMs = static_cast<uint16_t>(tGray - tPlanes);

  // Not optional. RED RAM now holds the MSB plane instead of the previous
  // frame, and any windowed or fast update in that state is silently promoted
  // to a full-frame HALF that wipes every grey on the panel
  // (Ssd1677Driver.cpp:434-437). The BW framebuffer is intact, so it is the
  // correct new baseline.
  renderer.cleanupGrayscaleWithFrameBuffer();
  timings.cleanupMs = static_cast<uint16_t>(millis() - tGray);

  timings.grayscale = true;
  timings.totalMs = static_cast<uint16_t>(millis() - tStart);
  return timings;
}

GrayscaleFrame::Timings GrayscaleFrame::nudge(GfxRenderer& renderer, const GrayDrawCallback& draw) {
  Timings timings;
  if (draw.fn == nullptr || !supported(renderer)) return timings;

  const uint32_t tStart = millis();

  if (!writePlanes(renderer, draw)) {
    timings.planesMs = static_cast<uint16_t>(millis() - tStart);
    timings.totalMs = timings.planesMs;
    return timings;
  }
  const uint32_t tPlanes = millis();
  timings.planesMs = static_cast<uint16_t>(tPlanes - tStart);

  renderer.displayGrayBuffer();
  const uint32_t tGray = millis();
  timings.grayDisplayMs = static_cast<uint16_t>(tGray - tPlanes);

  renderer.cleanupGrayscaleWithFrameBuffer();
  timings.cleanupMs = static_cast<uint16_t>(millis() - tGray);

  timings.grayscale = true;
  timings.totalMs = static_cast<uint16_t>(millis() - tStart);
  return timings;
}
