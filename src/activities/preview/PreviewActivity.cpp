#include "PreviewActivity.h"

#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int kMargin = 16;
constexpr int kMarkerSteps = 12;
constexpr int kMarkerSize = 28;  // white halo box; the black core is half of it
constexpr int kRegionNudges = 2;

// Page order and titles. Index order must match PreviewActivity::Page.
constexpr StrId kPageTitles[] = {StrId::STR_PREVIEW_SCALE, StrId::STR_PREVIEW_MARKER, StrId::STR_PREVIEW_DRIFT,
                                 StrId::STR_PREVIEW_REGION, StrId::STR_PREVIEW_DITHER};

constexpr GrayShade kBars[] = {GrayShade::White, GrayShade::LightGray, GrayShade::DarkGray, GrayShade::Black};
constexpr StrId kBarLabels[] = {StrId::STR_PREVIEW_WHITE, StrId::STR_PREVIEW_LIGHT_GREY, StrId::STR_PREVIEW_DARK_GREY,
                                StrId::STR_PREVIEW_BLACK};

// Text drawn on a shade has to stay readable: black on the two light levels,
// white on the two dark ones. Grey-on-grey is the failure this bench is for.
GrayShade labelShadeOn(const GrayShade background) {
  return (background == GrayShade::White || background == GrayShade::LightGray) ? GrayShade::Black : GrayShade::White;
}

}  // namespace

void PreviewActivity::onEnter() {
  Activity::onEnter();
  update_ = Update::Full;
  requestUpdate();
}

int PreviewActivity::contentTop() const { return 56; }

int PreviewActivity::contentBottom() const { return renderer.getScreenHeight() - 100; }

void PreviewActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::PREVIEW);
    return;
  }

  const auto stepPage = [this](const int delta) {
    const int count = static_cast<int>(Page::Count);
    int index = static_cast<int>(page_) + delta;
    if (index < 0) index = count - 1;
    if (index >= count) index = 0;
    page_ = static_cast<Page>(index);
    // Every page starts from its own step 0: a page's meaning depends on how
    // many times it has been nudged, so carrying a count across pages would
    // mislabel what is on the glass.
    markerStep_ = 0;
    markerPrevStep_ = 0;
    nudgeCount_ = 1;
    regionStep_ = 0;
    windowMs_ = 0;
    update_ = Update::Full;
    requestUpdate();
  };

  // Logical buttons only -- the front four are remappable and the mapping is
  // orientation-aware.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    stepPage(-1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    stepPage(+1);
    return;
  }

  // Either side button repaints the page from scratch. After a page has been
  // nudged a few times this is the way back to a known base state.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    markerStep_ = 0;
    markerPrevStep_ = 0;
    nudgeCount_ = 1;
    regionStep_ = 0;
    update_ = Update::Full;
    requestUpdate();
    return;
  }

  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;

  switch (page_) {
    case Page::Marker:
      markerPrevStep_ = markerStep_;
      markerStep_ = (markerStep_ + 1) % kMarkerSteps;
      update_ = Update::MarkerWindow;
      break;
    case Page::Drift:
      ++nudgeCount_;
      update_ = Update::Nudge;
      break;
    case Page::Region:
      // Two nudges, then back to a clean base frame.
      regionStep_ = (regionStep_ + 1) % (kRegionNudges + 1);
      update_ = regionStep_ == 0 ? Update::Full : Update::Nudge;
      break;
    default:
      update_ = Update::Full;
      break;
  }
  requestUpdate();
}

void PreviewActivity::render(RenderLock&&) {
  switch (update_) {
    case Update::Full:
      timings_ = GrayscaleFrame::render(renderer, callback());
      windowMs_ = 0;
      LOG_DBG("PRV", "page %d full: base=%ums planes=%ums gray=%ums clean=%ums total=%ums grey=%d",
              static_cast<int>(page_), timings_.baseDrawMs + timings_.baseDisplayMs, timings_.planesMs,
              timings_.grayDisplayMs, timings_.cleanupMs, timings_.totalMs, timings_.grayscale ? 1 : 0);
      // The chrome above was drawn before these numbers existed. One windowed
      // update puts the real ones on the panel without repainting the page.
      drawStatsLine();
      break;

    case Update::MarkerWindow: {
      // Redraw the whole frame in the framebuffer (CPU only, no panel time),
      // then refresh only the rectangle that covers the marker's old and new
      // positions. Everything outside is never addressed, so the grey backdrop
      // stays on the glass; inside, the pixels that changed are driven to pure
      // black or white and lose their grey. That contrast is the measurement.
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.clearScreen();
      draw(GrayPainter(renderer, GrayPainter::Pass::Base));

      int ax, ay, aw, ah, bx, by, bw, bh;
      markerRect(markerPrevStep_, ax, ay, aw, ah);
      markerRect(markerStep_, bx, by, bw, bh);
      const int x0 = ax < bx ? ax : bx;
      const int y0 = ay < by ? ay : by;
      const int x1 = (ax + aw) > (bx + bw) ? (ax + aw) : (bx + bw);
      const int y1 = (ay + ah) > (by + bh) ? (ay + ah) : (by + bh);

      const uint32_t start = millis();
      const bool shown = renderer.displayBufferWindow(x0, y0, x1 - x0, y1 - y0);
      windowMs_ = static_cast<uint16_t>(millis() - start);
      LOG_DBG("PRV", "marker step %d: window %d,%d %dx%d in %ums (shown=%d)", markerStep_, x0, y0, x1 - x0, y1 - y0,
              windowMs_, shown ? 1 : 0);
      break;
    }

    case Update::Nudge: {
      drawKind_ = DrawKind::NudgeRegion;
      timings_ = GrayscaleFrame::nudge(renderer, callback());
      drawKind_ = DrawKind::Frame;
      LOG_DBG("PRV", "page %d nudge %d: planes=%ums gray=%ums clean=%ums", static_cast<int>(page_),
              page_ == Page::Drift ? nudgeCount_ : regionStep_, timings_.planesMs, timings_.grayDisplayMs,
              timings_.cleanupMs);
      // The nudge left the framebuffer alone, so the counter on screen is one
      // press behind. A windowed BW update of the counter strip fixes it
      // without repainting -- and without disturbing the patches being judged.
      if (page_ == Page::Drift) drawCounterLine();
      drawStatsLine();
      break;
    }

    case Update::Counter:
      drawCounterLine();
      break;
  }
}

void PreviewActivity::drawTrampoline(void* ctx, const GrayPainter& painter) {
  static_cast<PreviewActivity*>(ctx)->draw(painter);
}

void PreviewActivity::draw(const GrayPainter& painter) const {
  if (drawKind_ == DrawKind::Frame) drawChrome(painter);

  switch (page_) {
    case Page::Scale:
      drawScalePage(painter);
      break;
    case Page::Marker:
      drawMarkerPage(painter);
      break;
    case Page::Drift:
      drawDriftPage(painter);
      break;
    case Page::Region:
      drawRegionPage(painter);
      break;
    case Page::Dither:
      drawDitherPage(painter);
      break;
    default:
      break;
  }
}

// Title, stats and button hints. Base pass only: this is plain black-on-white
// chrome, and drawing it into the planes would nudge pixels that are supposed
// to stay crisp.
void PreviewActivity::drawChrome(const GrayPainter& painter) const {
  if (painter.pass() != GrayPainter::Pass::Base) return;

  char line[80];
  const int pageIndex = static_cast<int>(page_);
  snprintf(line, sizeof(line), tr(STR_PREVIEW_PAGE_FORMAT), I18N.get(kPageTitles[pageIndex]), pageIndex + 1,
           static_cast<int>(Page::Count));
  // Two lines, spaced by the font's own line height: 22 px overlapped them on
  // hardware (UI_12 is taller than that).
  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, kMargin, 6, tr(STR_PREVIEW), true);
  renderer.drawText(UI_12_FONT_ID, kMargin, 6 + titleHeight, line, true);
  renderer.drawLine(kMargin, contentTop() - 8, renderer.getScreenWidth() - kMargin, contentTop() - 8, true);

  int statsX, statsY, statsW, statsH;
  statsRect(statsX, statsY, statsW, statsH);
  statsY += 6;  // same baselines drawStatsLine() uses
  if (!GrayscaleFrame::supported(renderer)) {
    renderer.drawText(UI_10_FONT_ID, kMargin, statsY, tr(STR_PREVIEW_NO_GRAY), true);
  } else {
    snprintf(line, sizeof(line), tr(STR_PREVIEW_TIMES_FORMAT), static_cast<unsigned>(timings_.baseDisplayMs),
             static_cast<unsigned>(timings_.planesMs), static_cast<unsigned>(timings_.grayDisplayMs),
             static_cast<unsigned>(timings_.cleanupMs));
    renderer.drawText(UI_10_FONT_ID, kMargin, statsY, line, true);
  }
  snprintf(line, sizeof(line), tr(STR_PREVIEW_STATE_FORMAT), static_cast<unsigned>(windowMs_),
           static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  renderer.drawText(UI_10_FONT_ID, kMargin, statsY + 18, line, true);

  const auto labels =
      mappedInput.mapLabels(tr(STR_EXIT), tr(STR_SELECT), tr(STR_PREVIEW_PAGE_PREV), tr(STR_PREVIEW_PAGE_NEXT));
  GUI.drawButtonHints(painter.raw(), labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// All four levels as fills, as text and as lines. If dark grey and light grey
// are not clearly apart here, nothing else on this bench means anything.
void PreviewActivity::drawScalePage(const GrayPainter& painter) const {
  const int width = renderer.getScreenWidth() - 2 * kMargin;
  const int barHeight = 56;
  int y = contentTop();

  for (int i = 0; i < 4; ++i) {
    const GrayShade shade = kBars[i];
    painter.fillRect(kMargin, y, width, barHeight, shade);
    painter.rect(kMargin, y, width, barHeight, 1, GrayShade::Black);
    painter.text(UI_12_FONT_ID, kMargin + 10, y + barHeight / 2 - 8, I18N.get(kBarLabels[i]), labelShadeOn(shade));
    y += barHeight;
  }

  // Light and dark grey side by side with no white between them: the hardest
  // pair to tell apart, and the one per-class map styling would lean on.
  y += 16;
  const int cell = width / 12;
  for (int i = 0; i < 12; ++i) {
    painter.fillRect(kMargin + i * cell, y, cell, 40, (i % 2 == 0) ? GrayShade::LightGray : GrayShade::DarkGray);
  }
  y += 56;

  // Text at each level. Glyphs are thin, so this is where a level that looks
  // fine as a fill can still read as mush.
  const GrayShade textShades[] = {GrayShade::Black, GrayShade::DarkGray, GrayShade::LightGray};
  for (const GrayShade shade : textShades) {
    painter.text(UI_12_FONT_ID, kMargin, y, tr(STR_PREVIEW_SAMPLE), shade);
    y += 26;
  }

  // Line widths 1..4 px at each level: the map renderer's whole vocabulary.
  y += 8;
  note(painter, kMargin, y, tr(STR_PREVIEW_WIDTHS), GrayShade::Black);
  y += 20;
  for (const GrayShade shade : textShades) {
    for (int lineWidth = 1; lineWidth <= 4; ++lineWidth) {
      painter.line(kMargin, y, kMargin + width, y, lineWidth, shade);
      y += lineWidth + 8;
    }
    y += 6;
  }
}

void PreviewActivity::note(const GrayPainter& painter, const int x, const int y, const char* text,
                           const GrayShade shade) const {
  const int maxWidth = renderer.getScreenWidth() - x - kMargin;
  painter.text(UI_10_FONT_ID, x, y, renderer.truncatedText(UI_10_FONT_ID, text, maxWidth).c_str(), shade);
}

void PreviewActivity::statsRect(int& x, int& y, int& w, int& h) const {
  x = kMargin;
  y = contentBottom() + 2;
  w = renderer.getScreenWidth() - 2 * kMargin;
  h = 40;  // both stats lines
}

void PreviewActivity::drawStatsLine() {
  int x, y, w, h;
  statsRect(x, y, w, h);

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.fillRect(x, y, w, h, false);  // white: no grey lives in this strip

  char line[80];
  if (!GrayscaleFrame::supported(renderer)) {
    renderer.drawText(UI_10_FONT_ID, x, y + 6, tr(STR_PREVIEW_NO_GRAY), true);
  } else {
    snprintf(line, sizeof(line), tr(STR_PREVIEW_TIMES_FORMAT), static_cast<unsigned>(timings_.baseDisplayMs),
             static_cast<unsigned>(timings_.planesMs), static_cast<unsigned>(timings_.grayDisplayMs),
             static_cast<unsigned>(timings_.cleanupMs));
    renderer.drawText(UI_10_FONT_ID, x, y + 6, line, true);
  }
  snprintf(line, sizeof(line), tr(STR_PREVIEW_STATE_FORMAT), static_cast<unsigned>(windowMs_),
           static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  renderer.drawText(UI_10_FONT_ID, x, y + 24, line, true);

  renderer.displayBufferWindow(x, y, w, h);
}

void PreviewActivity::markerRect(const int step, int& x, int& y, int& w, int& h) const {
  const int span = renderer.getScreenWidth() - 2 * kMargin - kMarkerSize;
  const int centreX = kMargin + kMarkerSize / 2 + (span * step) / (kMarkerSteps - 1);
  const int centreY = contentTop() + 60 + step * 22;
  x = centreX - kMarkerSize / 2;
  y = centreY - kMarkerSize / 2;
  w = kMarkerSize;
  h = kMarkerSize;
}

void PreviewActivity::drawMarker(const GrayPainter& painter, const int step) const {
  int x, y, w, h;
  markerRect(step, x, y, w, h);
  // White halo first, then the black core: both are pure BW, so a windowed
  // update can move the marker without needing any grey of its own.
  painter.fillRect(x, y, w, h, GrayShade::White);
  painter.rect(x, y, w, h, 1, GrayShade::Black);
  painter.fillRect(x + w / 4, y + h / 4, w / 2, h / 2, GrayShade::Black);
}

// A grey backdrop plus a BW marker that moves. Read it as: does grey survive a
// windowed update outside the window, and what exactly happens inside it.
void PreviewActivity::drawMarkerPage(const GrayPainter& painter) const {
  const int width = renderer.getScreenWidth() - 2 * kMargin;
  const int top = contentTop();
  const int bottom = contentBottom() - 40;

  // Light grey grid, dark grey blocks: something for the marker to pass over
  // and something to compare against after it has passed.
  for (int gridY = top; gridY < bottom; gridY += 40) {
    painter.line(kMargin, gridY, kMargin + width, gridY, 1, GrayShade::LightGray);
  }
  for (int gridX = kMargin; gridX <= kMargin + width; gridX += 40) {
    painter.line(gridX, top, gridX, bottom, 1, GrayShade::LightGray);
  }
  painter.fillRect(kMargin + 40, top + 120, 120, 90, GrayShade::DarkGray);
  painter.fillRect(kMargin + width - 160, top + 300, 140, 110, GrayShade::DarkGray);

  // The route the marker walks, in black, so the marker has real ink under it.
  for (int step = 0; step + 1 < kMarkerSteps; ++step) {
    int ax, ay, aw, ah, bx, by, bw, bh;
    markerRect(step, ax, ay, aw, ah);
    markerRect(step + 1, bx, by, bw, bh);
    painter.line(ax + aw / 2, ay + ah / 2, bx + bw / 2, by + bh / 2, 3, GrayShade::Black);
  }

  drawMarker(painter, markerStep_);

  char line[64];
  snprintf(line, sizeof(line), tr(STR_PREVIEW_MARKER_STEP_FORMAT), markerStep_ + 1, kMarkerSteps);
  note(painter, kMargin, bottom + 6, line, GrayShade::Black);
  note(painter, kMargin, bottom + 24, tr(STR_PREVIEW_MARKER_HINT), GrayShade::Black);
}

void PreviewActivity::counterRect(int& x, int& y, int& w, int& h) const {
  x = kMargin;
  y = contentBottom() - 60;
  w = renderer.getScreenWidth() - 2 * kMargin;
  h = 24;
}

void PreviewActivity::drawCounterLine() {
  int x, y, w, h;
  counterRect(x, y, w, h);

  char line[64];
  snprintf(line, sizeof(line), tr(STR_PREVIEW_DRIFT_AGAIN_FORMAT), nudgeCount_);

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.fillRect(x, y, w, h, false);  // white: this strip carries no grey
  renderer.drawText(UI_12_FONT_ID, x, y + 4, line, true);

  const uint32_t start = millis();
  renderer.displayBufferWindow(x, y, w, h);
  windowMs_ = static_cast<uint16_t>(millis() - start);
}

// Left patch is nudged once and never again. Right patch is nudged again on
// every press. If a repeat nudge drifts, the two stop matching, and the
// difference is what "partial grey" would cost on the map.
void PreviewActivity::drawDriftPage(const GrayPainter& painter) const {
  const int patch = 180;
  const int top = contentTop() + 60;
  const int rightX = renderer.getScreenWidth() - kMargin - patch;

  if (drawKind_ == DrawKind::NudgeRegion) {
    // Planes-only pass: mark the right patch and nothing else, so the left
    // patch keeps the single nudge it got when the page was drawn.
    painter.fillRect(rightX, top, patch, patch, GrayShade::DarkGray);
    return;
  }

  note(painter, kMargin, top - 22, tr(STR_PREVIEW_DRIFT_ONCE), GrayShade::Black);
  painter.fillRect(kMargin, top, patch, patch, GrayShade::DarkGray);
  painter.fillRect(rightX, top, patch, patch, GrayShade::DarkGray);
  painter.rect(kMargin, top, patch, patch, 1, GrayShade::Black);
  painter.rect(rightX, top, patch, patch, 1, GrayShade::Black);

  int cx, cy, cw, ch;
  counterRect(cx, cy, cw, ch);
  char line[64];
  snprintf(line, sizeof(line), tr(STR_PREVIEW_DRIFT_AGAIN_FORMAT), nudgeCount_);
  painter.text(UI_12_FONT_ID, cx, cy + 4, line, GrayShade::Black);
  note(painter, kMargin, cy + 32, tr(STR_PREVIEW_DRIFT_HINT), GrayShade::Black);
}

// A nudge that marks one region only. Plane bit 0 selects a LUT slot that is
// all zeros -- no drive -- so the rest of the black field must come back
// untouched. This is the claim partial grey on the map rests on.
void PreviewActivity::drawRegionPage(const GrayPainter& painter) const {
  const int width = renderer.getScreenWidth() - 2 * kMargin;
  const int top = contentTop() + 40;
  const int fieldHeight = 360;
  const int outerX = kMargin + width / 4;
  const int outerY = top + 80;
  const int outerW = width / 2;
  const int outerH = 180;

  if (drawKind_ == DrawKind::NudgeRegion) {
    if (regionStep_ == 1) {
      painter.fillRect(outerX, outerY, outerW, outerH, GrayShade::LightGray);
    } else if (regionStep_ == 2) {
      painter.fillRect(outerX + outerW / 4, outerY + outerH / 4, outerW / 2, outerH / 2, GrayShade::DarkGray);
    }
    return;
  }

  // Base frame: a black field, no grey at all. Every grey that appears later
  // came from a nudge, which is what makes the region test readable.
  painter.fillRect(kMargin, top, width, fieldHeight, GrayShade::Black);
  painter.rect(outerX, outerY, outerW, outerH, 1, GrayShade::White);

  char line[64];
  snprintf(line, sizeof(line), tr(STR_PREVIEW_REGION_STEP_FORMAT), regionStep_, kRegionNudges);
  painter.text(UI_12_FONT_ID, kMargin + 8, top + 8, line, GrayShade::White);
  note(painter, kMargin, top + fieldHeight + 12, tr(STR_PREVIEW_REGION_HINT), GrayShade::Black);
}

// Real four-level grey against the 2x2 checkerboard that already exists. Same
// sizes, same page: fills are a close call, and a 1 px line is not.
void PreviewActivity::drawDitherPage(const GrayPainter& painter) const {
  const int patchW = (renderer.getScreenWidth() - 3 * kMargin) / 2;
  const int patchH = 110;
  const int leftX = kMargin;
  const int rightX = kMargin * 2 + patchW;
  int y = contentTop() + 24;

  if (painter.pass() == GrayPainter::Pass::Base) {
    note(painter, leftX, y - 20, tr(STR_PREVIEW_REAL_GREY), GrayShade::Black);
    note(painter, rightX, y - 20, tr(STR_PREVIEW_DITHER_2X2), GrayShade::Black);
  }

  const GrayShade shades[] = {GrayShade::LightGray, GrayShade::DarkGray};
  const Color ditherColors[] = {Color::LightGray, Color::DarkGray};

  for (int i = 0; i < 2; ++i) {
    painter.fillRect(leftX, y, patchW, patchH, shades[i]);
    painter.rect(leftX, y, patchW, patchH, 1, GrayShade::Black);
    // Dither is BW pixels in a pattern -- no planes, no nudge, no cleanup. It
    // belongs to the base frame only, and must not be marked in a plane or it
    // would get nudged on top of the pattern.
    if (painter.pass() == GrayPainter::Pass::Base) {
      renderer.fillRectDither(rightX, y, patchW, patchH, ditherColors[i]);
      renderer.drawRect(rightX, y, patchW, patchH, 1, true);
    }
    y += patchH + 20;
  }

  // The 1 px case. Real grey holds the line; a checkerboard turns it into
  // dashes, which is why dither cannot carry thin map features.
  if (painter.pass() == GrayPainter::Pass::Base) {
    note(painter, leftX, y, tr(STR_PREVIEW_HAIRLINE), GrayShade::Black);
  }
  y += 24;
  for (int i = 0; i < 2; ++i) {
    painter.line(leftX, y, leftX + patchW, y, 1, shades[i]);
    if (painter.pass() == GrayPainter::Pass::Base) {
      renderer.fillRectDither(rightX, y, patchW, 1, ditherColors[i]);
    }
    y += 24;
  }
}
