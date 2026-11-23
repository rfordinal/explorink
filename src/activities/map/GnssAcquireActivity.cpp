#include "GnssAcquireActivity.h"

#ifdef ENABLE_GNSS_CMD

#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "GnssAccess.h"
#include "GnssFakeSky.h"
#include "MapGnssBars.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Mountains.h"

namespace {

constexpr const char* kLogTag = "GNSSACQ";

// Never redraw faster than this. A windowed refresh on the T5 S3 Pro costs
// 1,081 ms measured (../docs/t5s3-partial-refresh.md), so a screen that
// redrew per fix would hold the panel busy for a third of every second of a
// ten-minute wait -- for a number that changes by one satellite.
constexpr uint32_t kMinRedrawMs = 5000;
// The clock's own resolution, matching the floor above: a coarse timer that
// moves when the screen redraws anyway costs nothing, and a fine one would
// force a redraw with nothing new in it.
constexpr uint32_t kClockStepMs = 5000;

// ## The type ladder
//
// Four steps, biggest at the top, following the maintainer's mockup
// (2026-09-10). The hierarchy is the reading order: what the screen is, what it
// is waiting for, how long it has been, and then the numbers behind it.
//
// **Built from the faces this build already has, not from new ones.** The
// mockup's title is about 18 pt and the UI family stops at 12, so the obvious
// move was to register NotoSans 14/16/18 -- which costs **813 kB of flash** as
// full families, or 251 kB as the four rezes actually drawn (both measured on
// t5s3pro, 2026-09-10, against 3,896,147 bytes). The maintainer's call: not for
// a title. So the ladder is 12 pt bold, 12 pt, 10 pt and 8 pt, and it carries
// the hierarchy with weight and spacing where it runs out of size.
//
// NotoSerif 14 is linked in every build and is the one genuinely larger face
// available for free. It is deliberately not used here: every other screen on
// this device is sans, and a serif title would read as a different device.
constexpr int kTitleFont = UI_12_FONT_ID;     // bold
constexpr int kSubtitleFont = UI_10_FONT_ID;  // bold
constexpr int kClockFont = UI_12_FONT_ID;     // regular
constexpr int kHeadlineFont = UI_12_FONT_ID;  // the readout's first line
constexpr int kBodyFont = UI_10_FONT_ID;      // the signal line
constexpr int kSmallFont = SMALL_FONT_ID;     // the countdown and the hint

// Five slots for four C/N0 rungs -- see drawReadout() for why the fifth is a
// state and not a spare.
constexpr int kMeterSlots = 5;

// The Settings row's four values, in minutes, index 0 meaning no limit
// (CrossPointSettings::mapGnssWaitLimit, SettingsList.h). Kept next to the
// screen that enforces them rather than in the settings struct, because the
// struct stores an index and only this screen knows what the index means.
constexpr uint16_t kWaitLimitMinutes[] = {0, 2, 5, 10};
constexpr uint8_t kWaitLimitCount = 4;

// The cardinal ticks under the sky. Drawn as bare letters, not translated, the
// same choice the map's compass makes for its "N" (MapActivity::drawCompass()):
// it is a symbol on an instrument rather than a sentence.
constexpr const char* kCardinals[] = {"S", "W", "N", "E", "S"};
constexpr int kCardinalCount = 5;

}  // namespace

GnssAcquireActivity::GnssAcquireActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* routePath)
    : Activity("GnssAcquire", renderer, mappedInput) {
  if (routePath != nullptr && routePath[0] != '\0') {
    // Same refusal as MapActivity's: a truncated path opens the wrong file or
    // none, so it is dropped rather than shortened.
    const size_t len = std::strlen(routePath);
    if (len < sizeof(routePath_)) {
      std::memcpy(routePath_, routePath, len + 1);
    } else {
      LOG_ERR(kLogTag, "route path too long, ignored: %s", routePath);
    }
  }
}

void GnssAcquireActivity::onEnter() {
  Activity::onEnter();

  enteredMs_ = millis();
  lastRedrawMs_ = enteredMs_;

  // Whoever starts it, owns it. A receiver a host `CMD:GNSS ON` already brought
  // up is read here and left alone on the way out -- the same ownership rule
  // MapActivity::onEnter() follows, and the reason the handover into the map
  // carries a flag at all.
  if (gnss.running()) {
    LOG_INF(kLogTag, "gnss: already running, not started here");
  } else if (gnssStart()) {
    gnssStartedHere_ = true;
    LOG_INF(kLogTag, "gnss: started, rx ring %lu bytes", static_cast<unsigned long>(gnss.rxBufferSize()));
  } else {
    startFailed_ = true;
    LOG_ERR(kLogTag, "gnss: start failed, power rail or expander unavailable");
  }

  renderScreen();
}

void GnssAcquireActivity::onExit() {
  // Deliberately does NOT stop the receiver. Both exits decide that for
  // themselves: openMap(false) hands it to the map, openMap(true) and the Back
  // path drop it before leaving. Stopping it here would undo the handover a
  // moment after making it, and onExit() runs on every one of those paths.
  Activity::onExit();
}

void GnssAcquireActivity::loop() {
  Activity::loop();

  // The fix is what this screen is waiting for, so it is checked before input:
  // a rider whose fix lands while their thumb is moving should get the map, not
  // whatever row the thumb was on.
  //
  // Same acceptance test as the map's own (MapActivity::pollGnssFix): `valid`
  // latches on the first solution and never clears, so it says "has ever had
  // one"; `quality` is what says the receiver still has satellites, with 0 for
  // none and 6 for dead reckoning with nothing behind it.
  const GnssFix& fix = gnss.fix();
  if (gnss.running() && fix.valid && fix.quality != 0 && fix.quality != 6) {
    LOG_INF(kLogTag, "fix after %lu ms, %u sats used, quality %u", static_cast<unsigned long>(millis() - enteredMs_),
            static_cast<unsigned>(fix.satsUsed), static_cast<unsigned>(fix.quality));
    openMap(false);
    return;
  }

  // The limit the rider set in Settings, if any. The map is useful without a
  // fix -- it draws from the persisted last position and the receiver carries on
  // searching behind it -- so a wait with no end would hold them out of their
  // own map for nothing (CrossPointSettings::mapGnssWaitLimit).
  const uint32_t limitMs = waitLimitMs();
  if (limitMs > 0 && millis() - enteredMs_ >= limitMs) {
    LOG_INF(kLogTag, "wait limit of %lu ms reached with no fix, opening the map", static_cast<unsigned long>(limitMs));
    openMap(false);
    return;
  }

  bool selectionMoved = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (selected_ != Action::OpenMap) {
      selected_ = Action::OpenMap;
      selectionMoved = true;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (selected_ != Action::UsePhone) {
      selected_ = Action::UsePhone;
      selectionMoved = true;
    }
  }

  // A direct tap on a row, which is the only input this board has in the
  // default touch mode: it draws no hint boxes, so Up/Down do not exist there
  // and the rows themselves have to be the buttons (../docs/touch-modes.md).
  for (int index = 0; index < kActionCount; ++index) {
    int x, y, w, h;
    actionRect(index, x, y, w, h);
    if (mappedInput.wasTapInRect(x, y, w, h)) {
      activate(static_cast<Action>(index));
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selected_);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Home, not the map. This screen is on the way to the map, and Back that
    // silently continued forwards would mean something different here than
    // everywhere else -- the same reasoning as the trip picker's Back.
    if (gnssStartedHere_) {
      gnss.end();
      gnssStartedHere_ = false;
      LOG_INF(kLogTag, "gnss: stopped, leaving for home");
    }
    onGoHome(HomeMenuItem::MAP);
    return;
  }

  if (selectionMoved) {
    drawActions();
    int x, y, w, h;
    actionsBand(x, y, w, h);
    refreshBand(x, y, w, h);
    return;
  }

  if (millis() - lastRedrawMs_ < kMinRedrawMs) return;
  if (!skyChanged()) return;
  lastRedrawMs_ = millis();
  drawClock();
  drawSky();
  drawReadout();
  drawn_ = currentDrawn();
  int x, y, w, h;
  skyBand(x, y, w, h);
  refreshBand(x, y, w, h);
}

void GnssAcquireActivity::activate(Action action) {
  switch (action) {
    case Action::OpenMap:
      LOG_INF(kLogTag, "rider opened the map with the receiver still searching");
      openMap(false);
      return;
    case Action::UsePhone:
      LOG_INF(kLogTag, "rider chose the phone, dropping the receiver for this session");
      openMap(true);
      return;
  }
}

void GnssAcquireActivity::openMap(bool usePhone) {
  if (usePhone && gnssStartedHere_) {
    // The map session runs BLE, and one radio per session means this receiver
    // has no reader for as long as that map is up. Power it down here rather
    // than leaving a rail on with nobody listening -- the rail also feeds the
    // LoRa radio (main.cpp's gnssPowerEnable()).
    gnss.end();
    gnssStartedHere_ = false;
  }
  const char* route = routePath_[0] != '\0' ? routePath_ : nullptr;
  activityManager.goToMap(route, false, !usePhone && gnssStartedHere_, usePhone);
}

// ## Layout
//
// Bottom-anchored, then top-anchored, and the sky takes whatever is left. Every
// number below comes from the theme's metrics or the panel's own size: this
// screen has to hold on a 480x800 X4 and a 540x960 T5 S3 Pro, and a layout
// written against either one's pixels would be a defect on the other (parent
// repo's CLAUDE.md, "Styles must be universal").

// Three lines, three sizes: the state in one sentence, the signal with its
// meter, and the line of advice. The clock is in the head block.
int GnssAcquireActivity::readoutHeight() const {
  return renderer.getLineHeight(kHeadlineFont) + renderer.getLineHeight(kBodyFont) + renderer.getLineHeight(kSmallFont);
}

int GnssAcquireActivity::readoutTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int ax, ay, aw, ah;
  actionRect(0, ax, ay, aw, ah);
  (void)ax;
  (void)aw;
  (void)ah;
  return ay - metrics.verticalSpacing - readoutHeight();
}

void GnssAcquireActivity::actionRect(int index, int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  h = metrics.menuRowHeight;
  x = metrics.contentSidePadding;
  w = pageWidth - metrics.contentSidePadding * 2;
  const int blockBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int blockTop = blockBottom - (h * kActionCount + metrics.menuSpacing * (kActionCount - 1));
  y = blockTop + index * (h + metrics.menuSpacing);
}

GnssSkyView::Box GnssAcquireActivity::skyBox() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GnssSkyView::Box box;
  // Full panel width, not the content inset every other screen uses: a horizon
  // that stops short of the edges is not a horizon. The cardinal ticks under it
  // clamp themselves back inside.
  box.x = 0;
  box.w = pageWidth;

  // Under the head block: title, subtitle, the clock and its countdown.
  const int top = clockTop() + clockHeight() + metrics.verticalSpacing;
  // One line for the cardinal ticks under the horizon.
  const int bottom = readoutTop() - metrics.verticalSpacing - lineHeight;

  box.y = top;
  box.h = bottom - top;
  return box;
}

void GnssAcquireActivity::skyBand(int& x, int& y, int& w, int& h) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const GnssSkyView::Box box = skyBox();
  int ax, ay, aw, ah;
  actionRect(0, ax, ay, aw, ah);
  (void)ax;
  (void)aw;
  (void)ah;
  // The sky, its ticks and the readout under it, as one rectangle: they change
  // together on every satellite update, and two windows cost two refreshes.
  // From the clock down: the clock, the sky, its ticks and the readout change on
  // the same tick, and two windows would cost two refreshes of ~1,081 ms each.
  x = 0;
  w = renderer.getScreenWidth();
  y = clockTop();
  h = ay - metrics.verticalSpacing - y;
  (void)box;
}

void GnssAcquireActivity::actionsBand(int& x, int& y, int& w, int& h) const {
  int x0, y0, w0, h0;
  int x1, y1, w1, h1;
  actionRect(0, x0, y0, w0, h0);
  actionRect(1, x1, y1, w1, h1);
  x = 0;
  w = renderer.getScreenWidth();
  y = y0;
  h = (y1 + h1) - y0;
}

void GnssAcquireActivity::refreshBand(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  // The driver refuses a window in some states and promotes it to a whole
  // frame in others (GfxRenderer::displayBufferWindow). A refusal is a false
  // return, and the panel would otherwise keep the old picture.
  if (!renderer.displayBufferWindow(x, y, w, h)) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void GnssAcquireActivity::renderScreen() {
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  // No wordmark and no logo up here. The device does not need to introduce
  // itself on a screen the rider reached by pressing Explore on it, and every
  // pixel spent on branding is a pixel of sky (maintainer's call, 2026-09-10 --
  // it replaced the home header art, which had put a second mountain range
  // ABOVE the sky and made the sky read as underground).
  renderer.drawCenteredText(kTitleFont, metrics.topPadding, tr(STR_GNSS_ACQ_TITLE), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(kSubtitleFont, metrics.topPadding + renderer.getLineHeight(kTitleFont),
                            tr(STR_GNSS_ACQ_SUB), true, EpdFontFamily::BOLD);

  drawClock();
  drawSky();
  drawReadout();
  drawActions();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  drawn_ = currentDrawn();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// ## The five reads the sky is drawn from, in one place
//
// The bench's synthetic sky (GnssFakeSky.h) substitutes here and nowhere else,
// so **the drawing code cannot tell the two apart** -- which is the only way a
// screenshot taken with a fake sky says anything about the real one. A branch
// inside drawSky() would have been a second renderer to keep in step.
uint8_t GnssAcquireActivity::skyCount() const { return FAKE_SKY.enabled() ? FAKE_SKY.count() : gnss.satelliteCount(); }

const GnssSatellite& GnssAcquireActivity::skySatellite(uint8_t index) const {
  return FAKE_SKY.enabled() ? FAKE_SKY.satellite(index) : gnss.satellite(index);
}

uint8_t GnssAcquireActivity::skyInView() const {
  return FAKE_SKY.enabled() ? FAKE_SKY.satsInView() : gnss.satsInView();
}

uint8_t GnssAcquireActivity::skyHeard() const {
  return FAKE_SKY.enabled() ? FAKE_SKY.satsWithSignal() : gnss.satsWithSignal();
}

uint8_t GnssAcquireActivity::skyBestSnr() const { return FAKE_SKY.enabled() ? FAKE_SKY.bestSnr() : gnss.bestSnr(); }

uint32_t GnssAcquireActivity::waitLimitMs() const {
  const uint8_t index = SETTINGS.mapGnssWaitLimit < kWaitLimitCount ? SETTINGS.mapGnssWaitLimit : 0;
  return static_cast<uint32_t>(kWaitLimitMinutes[index]) * 60u * 1000u;
}

// Where the head block ends: the title, its subtitle, the elapsed clock and,
// when a limit is set, the line that says the map opens by itself. Both the
// clock and the sky measure from this, so they cannot drift apart.
int GnssAcquireActivity::clockTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + renderer.getLineHeight(kTitleFont) + renderer.getLineHeight(kSubtitleFont);
}

int GnssAcquireActivity::clockHeight() const {
  // Two lines when a limit is running, one when it is not.
  return renderer.getLineHeight(kClockFont) + (waitLimitMs() > 0 ? renderer.getLineHeight(kSmallFont) : 0);
}

// The elapsed wait, big and directly under the subtitle rather than buried in
// the readout at the bottom: it is the number the rider is actually watching
// (maintainer's call, 2026-09-10). Under it, when the wait has a limit, what
// happens when it runs out -- stated while it runs, so the jump into the map is
// never something that happens to them without warning.
void GnssAcquireActivity::drawClock() {
  const int pageWidth = renderer.getScreenWidth();
  const int y = clockTop();
  renderer.fillRect(0, y, pageWidth, clockHeight(), false);

  const uint32_t waitedS = (millis() - enteredMs_) / 1000;
  char line[96];
  snprintf(line, sizeof(line), tr(STR_GNSS_ACQ_WAITED), static_cast<int>(waitedS / 60), static_cast<int>(waitedS % 60));
  renderer.drawCenteredText(kClockFont, y, line, true);

  const uint32_t limitMs = waitLimitMs();
  if (limitMs == 0) return;
  const uint32_t elapsedMs = millis() - enteredMs_;
  const uint32_t leftS = elapsedMs >= limitMs ? 0 : (limitMs - elapsedMs) / 1000;
  snprintf(line, sizeof(line), tr(STR_GNSS_ACQ_AUTO_IN), static_cast<int>(leftS / 60), static_cast<int>(leftS % 60));
  renderer.drawCenteredText(kSmallFont, y + renderer.getLineHeight(kClockFont), line, true);
}

// A blit that clips instead of complaining. GfxRenderer::drawMono1bpp() goes
// through drawPixel(), which LOG_ERRs every pixel outside the panel rather than
// dropping it -- and this asset is deliberately wider than the panel and seated
// so its bottom runs past the horizon. Blitting it straight would be tens of
// thousands of serial lines per frame.
void GnssAcquireActivity::drawRidgeClipped(int x, int y, int clipTop, int clipBottom) {
  const int rowBytes = (MOUNTAINS_WIDTH + 7) / 8;
  const int pageWidth = renderer.getScreenWidth();
  for (int row = 0; row < MOUNTAINS_HEIGHT; ++row) {
    const int py = y + row;
    if (py < clipTop || py > clipBottom) continue;
    for (int col = 0; col < MOUNTAINS_WIDTH; ++col) {
      const int px = x + col;
      if (px < 0 || px >= pageWidth) continue;
      const uint8_t byte = Mountains[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) renderer.drawPixel(px, py, true);
    }
  }
}

void GnssAcquireActivity::drawSky() {
  const GnssSkyView::Box box = skyBox();
  if (box.w <= 0 || box.h <= 0) return;

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int horizon = box.y + box.h - 1;

  // Repaint the band first: this runs on a redraw as well as on the first
  // frame, and a satellite that moved has to leave nothing behind.
  renderer.fillRect(box.x, box.y, box.w, box.h + lineHeight, false);

  // The mountain line art, 1:1 and never scaled (parent repo's CLAUDE.md, "Map
  // rendering"), centred on a panel narrower than it is and seated kRidgeCrop
  // below the horizon so its bottom is cut off. Clipped to the box, top and
  // bottom, by our own blit -- see drawRidgeClipped().
  drawRidgeClipped(box.x + GnssSkyView::ridgeOffset(box), horizon - MOUNTAINS_HEIGHT + GnssSkyView::kRidgeCrop, box.y,
                   horizon);
  // Ground level, edge to edge, under the cut. Not needed to carry the horizon
  // any more -- the asset is wider than every panel here -- but it is what makes
  // the crop read as ground rather than as art that ran out of pixels
  // (maintainer's call, 2026-09-10, after seeing it without).
  renderer.drawLine(box.x, horizon, box.x + box.w - 1, horizon, true);

  // The satellites. Diamonds rather than circles because the renderer has no
  // circle primitive, and a diamond reads as a mark on an instrument rather
  // than as a map dot -- which matters on a device whose other screen is a map.
  const uint8_t count = skyCount();
  for (uint8_t i = 0; i < count; ++i) {
    const GnssSatellite& sat = skySatellite(i);
    // A satellite the receiver has an almanac for but has not located carries
    // no elevation or azimuth, and (0,0) is due north on the horizon -- a real
    // position, and the worst one there is. Those are counted in the readout
    // and not drawn.
    if (!sat.hasPosition) continue;

    const GnssSkyView::Dot dot = GnssSkyView::plot(GnssSkyView::plotArea(box), sat.elevation, sat.azimuth, sat.snr);
    const int r = dot.radius;
    // A mark landing in the terrain gets the ground rubbed out behind it. The
    // ridge is line art, not a filled silhouette, so a white mark would
    // disappear into its white interior and a black one would read as another
    // ridge line. The halo keeps it a mark -- and it has to stay drawn, because
    // a satellite low in a blocked direction is the finding this screen exists
    // to report.
    if (GnssSkyView::behindRidge(box, dot)) {
      renderer.fillRect(dot.x - r - 1, dot.y - r - 1, r * 2 + 3, r * 2 + 3, false);
    }
    const int xs[4] = {dot.x, dot.x + r, dot.x, dot.x - r};
    const int ys[4] = {dot.y - r, dot.y, dot.y + r, dot.y};
    if (dot.filled) {
      renderer.fillPolygon(xs, ys, 4, true);
    } else {
      for (int p = 0; p < 4; ++p) {
        const int q = (p + 1) % 4;
        renderer.drawLine(xs[p], ys[p], xs[q], ys[q], true);
      }
    }
  }

  // Cardinal labels under the horizon, so "which way do I move" has an answer,
  // with a tick between each pair. The ticks are what make the row read as a
  // scale rather than as five loose letters (maintainer's mockup, 2026-09-10).
  //
  // Against the inset plot area, not the panel edge: at the edge the two S
  // labels sat half off the screen (seen on the panel 2026-09-10), and the row
  // has to name the azimuth the marks above it are actually plotted against.
  const GnssSkyView::Box plot = GnssSkyView::plotArea(box);
  const int labelTop = horizon + 2;
  const int tickHeight = renderer.getLineHeight(UI_10_FONT_ID) / 2;
  for (int i = 0; i < kCardinalCount; ++i) {
    const int x = plot.x + (plot.w - 1) * i / (kCardinalCount - 1);
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, kCardinals[i]);
    int labelX = x - labelWidth / 2;
    if (labelX < box.x) labelX = box.x;
    if (labelX + labelWidth > box.x + box.w) labelX = box.x + box.w - labelWidth;
    renderer.drawText(UI_10_FONT_ID, labelX, labelTop, kCardinals[i], true);

    if (i + 1 < kCardinalCount) {
      const int next = plot.x + (plot.w - 1) * (i + 1) / (kCardinalCount - 1);
      renderer.drawLine((x + next) / 2, labelTop + 2, (x + next) / 2, labelTop + 2 + tickHeight, true);
    }
  }
}

void GnssAcquireActivity::drawReadout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  int y = readoutTop();

  renderer.fillRect(0, y, pageWidth, readoutHeight(), false);

  const uint8_t heard = skyHeard();
  const uint8_t best = skyBestSnr();

  // A satellite the receiver hears but has not located carries no elevation or
  // azimuth, so the plot cannot draw it -- (0,0) is due north on the horizon,
  // a real position and the worst one there is. Seen on the panel 2026-09-10:
  // "2 heard" over a completely empty sky, which reads as a broken plot. So the
  // readout says it in words instead.
  uint8_t unplaced = 0;
  const uint8_t satellites = skyCount();
  for (uint8_t i = 0; i < satellites; ++i) {
    const GnssSatellite& sat = skySatellite(i);
    if (sat.snr > 0 && !sat.hasPosition) ++unplaced;
  }

  // The first line is the whole state in one sentence: how many satellites the
  // antenna hears, and why that is not a position yet. Satellites HEARD and not
  // satellites in view, deliberately -- in view is the almanac's opinion about
  // what is above the horizon and it reads 19 from indoors, which is the number
  // that would make a rider stand still and wait for nothing.
  char heardText[48];
  char line[128];
  if (startFailed_) {
    snprintf(line, sizeof(line), "%s", tr(STR_GNSS_ACQ_NO_RECEIVER));
  } else if (heard == 0) {
    snprintf(line, sizeof(line), "%s", tr(STR_GNSS_ACQ_SEARCHING));
  } else {
    if (heard == 1) {
      snprintf(heardText, sizeof(heardText), "%s", tr(STR_GNSS_ACQ_HEARD_ONE));
    } else {
      snprintf(heardText, sizeof(heardText), tr(STR_GNSS_ACQ_HEARD_MANY), static_cast<int>(heard));
    }
    // "not located yet" where it applies, because it is the more specific
    // answer and it is the one that explains an empty sky over a non-zero
    // count. Otherwise the general one: there is no fix, and that is why the
    // screen is up.
    if (unplaced > 0) {
      snprintf(line, sizeof(line), tr(STR_GNSS_ACQ_NOT_PLACED), heardText, static_cast<int>(unplaced));
    } else {
      snprintf(line, sizeof(line), tr(STR_GNSS_ACQ_NOT_STABLE), heardText);
    }
  }
  renderer.drawText(kHeadlineFont, metrics.contentSidePadding, y, line, true);
  y += renderer.getLineHeight(kHeadlineFont);

  // The best signal as a number, and next to it five slots: the C/N0 ladder the
  // map header's bars are calibrated against, one lit slot per rung passed
  // (MapGnssBars::kBestSnrForHeightStep -- 26, 31, 36, 40 dB-Hz). So an empty
  // meter means nothing worth hearing, one lit slot is a satellite that cannot
  // read its own ephemeris off the air, and full is open sky.
  //
  // **Five slots for four rungs**, which is what the mockup asks for and what
  // the ladder actually says: the fifth is the "heard, below the first rung"
  // state, the same one GnssSkyView::snrBucket() draws as its smallest mark.
  snprintf(line, sizeof(line), tr(STR_GNSS_ACQ_BEST), static_cast<int>(best));
  renderer.drawText(kBodyFont, metrics.contentSidePadding, y, line, true);

  const int bodyHeight = renderer.getLineHeight(kBodyFont);
  const int slot = bodyHeight * 2 / 3;
  const int gap = slot / 2;
  const int meterX = metrics.contentSidePadding + renderer.getTextWidth(kBodyFont, line) + bodyHeight;
  const int slotTop = y + (bodyHeight - slot) / 2;
  const int lit = MapGnssBars::resolve(heard, best, MapGnssBars::State{}).heightStep;
  for (int i = 0; i < kMeterSlots; ++i) {
    const int sx = meterX + i * (slot + gap);
    renderer.drawRect(sx, slotTop, slot, slot, true);
    // Filled solid rather than part-height: at this size a two-thirds bar
    // inside a box reads as a rendering fault, and the count already carries
    // the value.
    if (i < lit) renderer.fillRect(sx, slotTop, slot, slot, true);
  }
  y += bodyHeight;

  // One line of advice, smallest on the screen: it is the only thing here a
  // rider does not need to read twice.
  renderer.drawText(kSmallFont, metrics.contentSidePadding, y, tr(STR_GNSS_ACQ_HINT), true);
}

void GnssAcquireActivity::drawActions() {
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (int index = 0; index < kActionCount; ++index) {
    int x, y, w, h;
    actionRect(index, x, y, w, h);
    const bool selected = static_cast<int>(selected_) == index;
    // Filled when selected, outlined otherwise, same as every other list on the
    // device (BaseTheme::drawHomeMenu). The label inverts with it.
    renderer.fillRect(x, y, w, h, selected);
    if (!selected) renderer.drawRect(x, y, w, h, true);

    const char* label =
        index == static_cast<int>(Action::OpenMap) ? tr(STR_GNSS_ACQ_OPEN_MAP) : tr(STR_GNSS_ACQ_USE_PHONE);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    renderer.drawText(UI_10_FONT_ID, x + (w - textWidth) / 2, y + (h - lineHeight) / 2, label, !selected);
  }
}

GnssAcquireActivity::Drawn GnssAcquireActivity::currentDrawn() const {
  Drawn now;
  now.inView = skyInView();
  now.heard = skyHeard();
  now.bestSnr = skyBestSnr();
  now.satellites = skyCount();
  now.waitedSteps = static_cast<uint16_t>((millis() - enteredMs_) / kClockStepMs);
  now.receiverUp = gnss.running();
  return now;
}

bool GnssAcquireActivity::skyChanged() const {
  const Drawn now = currentDrawn();
  return now.inView != drawn_.inView || now.heard != drawn_.heard || now.bestSnr != drawn_.bestSnr ||
         now.satellites != drawn_.satellites || now.waitedSteps != drawn_.waitedSteps ||
         now.receiverUp != drawn_.receiverUp;
}

#endif  // ENABLE_GNSS_CMD
