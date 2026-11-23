#include "PatternLockActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "TouchPolicy.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "PATTERNLOCK";

// Geometry is derived from the panel, never written in X4 pixels: the same screen
// has to read right on 480x800 and on 540x960 (CLAUDE.md, "Styles must be
// universal"). Everything below is a fraction of the grid pitch the panel allows.
//
// The grid is a lattice of small solid dots, and the cursor is one of those dots
// inside a ring. The first cut drew big hollow rings with a filled blob for the
// cursor; on the T5 S3 Pro, 2026-11-23, that read as sixteen empty holes with one
// of them swallowed. A ring says "you are here" without making the cursor a
// different kind of point from the one it is standing on.
constexpr int kNodeRadiusDiv = 9;       // node dot radius = pitch / 9
constexpr int kMinNodeRadius = 4;
// The grid takes a pitch of span/(cols+2) rather than span/cols, so its ink is
// about half the panel's width instead of seven tenths. A lock grid is a thing to
// aim at, not a wall: the first cut filled the screen with it and read as heavy
// (T5 S3 Pro, 2026-11-23). Two extra pitches of air is the whole difference.
constexpr int kPitchSpanDiv = patternlock::kCols + 2;
constexpr int kCursorRingMul = 26;      // ring radius = node radius * 2.6
constexpr int kCursorRingDiv = 10;
constexpr int kCursorRingWidth = 3;     // thin enough to read, thick enough to survive a FAST refresh

// How far along the entry is: a segmented meter, one segment per possible point,
// filled left to right. Same job as the wallet PIN's dots -- count without the
// path -- and deliberately NOT the same mark.
//
// A row of dots under a lattice of dots reads as a fifth row of the lattice, and
// a box around those dots only makes it a tray of them. Changing the shape is
// what separates the two at a glance; the box is what makes it look like a
// control rather than a stray graphic, and it matches the hint boxes the theme
// already draws. Decided on the panel, 2026-11-23, against three alternatives.
//
// The meter is exactly as wide as the grid above it, and every size here comes
// off the grid's pitch, so a different panel moves both together.
constexpr int kMeterHeightMul = 3;      // meter height = node radius * 3
constexpr int kMeterBorder = 2;
constexpr int kMeterInsetDiv = 3;       // a filled segment's inset = height / 3

bool inside(const int x, const int y, const int left, const int top, const int width, const int height) {
  return x >= left && x < left + width && y >= top && y < top + height;
}

}  // namespace

PatternLockActivity::PatternLockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Config config,
                                         Verifier verifier)
    : Activity("PatternLock", renderer, mappedInput),
      config_(config),
      verifier_(std::move(verifier)),
      entry_(config.startCell) {}

void PatternLockActivity::onEnter() {
  Activity::onEnter();
  entry_.wipe();
  lastAttemptWrong_ = false;
  speculativeOutstanding_ = false;
  lockedOut_ = config_.isLockedOut && config_.isLockedOut();
  if (lockedOut_) LOG_ERR(kLogTag, "entered locked out: the caller is refusing attempts");
  // A rule that is load-bearing at runtime and invisible when broken is a wish
  // (CLAUDE.md, "Code comments answer WHY"). This one is the whole rate limiter:
  // with auto-submit on, every guess arrives speculative and onEntryEnded is the
  // only thing a caller can count.
  if (config_.autoSubmit && !config_.onEntryEnded) {
    LOG_ERR(kLogTag, "auto-submit with no onEntryEnded: this caller cannot count a failed attempt");
  }
  redraw(HalDisplay::HALF_REFRESH);
}

void PatternLockActivity::onExit() {
  Activity::onExit();
  // A walk that is still owed to the caller's counter is settled before the
  // screen dies, so leaving mid-entry is not a free set of guesses.
  settleEntry();
  // The path dies with the screen, whichever way the screen ended.
  entry_.wipe();
}

bool PatternLockActivity::handleHomeGesture() {
  // Consumed, deliberately: the alternative is ActivityManager::goHome() tearing
  // down this screen and the caller behind it on one bottom-edge swipe.
  return true;
}

void PatternLockActivity::render(RenderLock&& lock) {
  // The manager already holds the lock; draw into the frame it is offering.
  draw(HalDisplay::FAST_REFRESH);
}

void PatternLockActivity::redraw(const HalDisplay::RefreshMode mode) {
  RenderLock lock;
  draw(mode);
}

void PatternLockActivity::loop() {
  Activity::loop();
  if (finished_) return;

  if (handleTouch()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (lockedOut_) {
    // Every other button inert, so the screen cannot be walked into a state whose
    // status line says something untrue.
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (entry_.longEnough()) submit(false);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    onStep(patternlock::Step::Left);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    onStep(patternlock::Step::Right);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    onStep(patternlock::Step::Up);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    onStep(patternlock::Step::Down);
    return;
  }
}

bool PatternLockActivity::handleTouch() {
  // The tap is read before the layout is derived, so a frame with no tap costs
  // nothing: layout() reaches UITheme::getMetrics(), which mutates a shared cache,
  // and running that from the main task on every loop iteration widens a window
  // the render task is also in.
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return false;

  const Layout l = layout();
  if (!l.ownTouch) return false;

  if (inside(x, y, l.backLeft, l.boxTop, l.boxWidth, l.boxHeight)) {
    onBack();
    return true;
  }
  if (lockedOut_) return true;  // the tap landed on the screen and means nothing here
  if (inside(x, y, l.okLeft, l.boxTop, l.boxWidth, l.boxHeight)) {
    if (entry_.longEnough()) submit(false);
    return true;
  }

  // A tap on a cell orthogonally next to the cursor is that step. Not "walk to
  // wherever I tapped": a path is a sequence of steps the rider chose, and
  // inferring the route between two cells would invent points they never entered.
  const int cursorRow = entry_.cursorRow();
  const int cursorCol = entry_.cursorCol();
  for (uint8_t cell = 0; cell < patternlock::kCells; ++cell) {
    const int row = cell / patternlock::kCols;
    const int col = cell % patternlock::kCols;
    const int cx = l.gridLeft + col * l.pitch;
    const int cy = l.gridTop + row * l.pitch;
    if (!inside(x, y, cx - l.pitch / 2, cy - l.pitch / 2, l.pitch, l.pitch)) continue;
    const int dr = row - cursorRow;
    const int dc = col - cursorCol;
    if (dr == 0 && dc == 1) onStep(patternlock::Step::Right);
    else if (dr == 0 && dc == -1) onStep(patternlock::Step::Left);
    else if (dr == 1 && dc == 0) onStep(patternlock::Step::Down);
    else if (dr == -1 && dc == 0) onStep(patternlock::Step::Up);
    return true;
  }
  // A tap on dead space is not this screen's business. Reporting it as handled
  // would swallow whatever hardware button released in the same frame.
  return false;
}

void PatternLockActivity::onBack() {
  if (entry_.undo()) {
    // Undo abandons a candidate that has already been put to the verifier, so the
    // walk is over for accounting even though the screen stays up. Without this
    // an attacker walks to four points (one real check), backs off, walks a
    // different candidate, and repeats for ever -- every guess reaching the real
    // comparison and the caller billed once, when they finally leave. Found in
    // the second review, 2026-11-23: the first one closed the forward walk and
    // this door was still open.
    settleEntry();
    lastAttemptWrong_ = false;
    redraw(HalDisplay::FAST_REFRESH);
    return;
  }
  // Nothing entered and BACK: leave, cancelled. A gate the caller pushed is the
  // caller's to re-push; this screen does not decide that nobody may go back.
  finished_ = true;
  settleEntry();
  setResult(ActivityResult{PatternLockResult{false}});
  finish();
}

void PatternLockActivity::onStep(const patternlock::Step step) {
  // A refused step is a wall or a full path. Nothing changed, so nothing is
  // redrawn: a refresh per refused press reads as a stutter and says nothing.
  if (!entry_.step(step)) return;
  lastAttemptWrong_ = false;

  if (config_.autoSubmit && entry_.longEnough()) {
    submit(true);
    // A hit already finished the screen; drawing the grid again here would be a
    // refresh of a frame nobody will look at.
    if (finished_) return;
    if (entry_.full()) {
      // The eighth point is the end of the walk: the rider cannot step further,
      // so the caller is billed and the screen says what happened. Silence here
      // left a rider staring at "8 of 8 points" with no way to tell that the
      // pattern had already been refused (second review, 2026-11-23).
      settleEntry();
      lastAttemptWrong_ = !lockedOut_;
      entry_.wipe();
      redraw(HalDisplay::HALF_REFRESH);
      return;
    }
    if (lockedOut_) {
      redraw(HalDisplay::HALF_REFRESH);
      return;
    }
  }

  // FAST: one cursor move is a small differential change, and a 1.7 s wait per
  // press would make the screen unusable (the same call the wallet PIN makes).
  redraw(HalDisplay::FAST_REFRESH);
}

void PatternLockActivity::settleEntry() {
  if (!speculativeOutstanding_) return;
  speculativeOutstanding_ = false;
  if (config_.onEntryEnded) config_.onEntryEnded();
}

void PatternLockActivity::submit(const bool speculative) {
  char path[patternlock::kPathBufBytes] = {0};
  entry_.encode(path);

  const Verdict verdict = verifier_ ? verifier_(path, speculative) : Verdict::Wrong;
  // Whatever it answered, the typed path has served its purpose. Through
  // secureWipe, not a plain loop: `path` is dead on the next statement and a
  // release build is entitled to drop stores to it, which is exactly how a secret
  // survives its own zeroing.
  patternlock::secureWipe(path, sizeof(path));

  if (verdict == Verdict::Ok) {
    // No point count here: LOG_INF compiles into gh_release (LOG_LEVEL=1), and
    // the length of the pattern cuts the space an attacker has to search from
    // "4 to 8 points" to exactly N. The detail is a debug line.
    LOG_INF(kLogTag, "unlocked");
    LOG_DBG(kLogTag, "unlocked on %u points%s", static_cast<unsigned>(entry_.points()),
            speculative ? " (auto)" : "");
    speculativeOutstanding_ = false;  // a walk that unlocked costs the caller nothing
    finishUnlocked();
    return;
  }

  lockedOut_ = verdict == Verdict::LockedOut;

  if (speculative) {
    // A probe that missed says nothing on screen. A rider walking toward an
    // 8-point pattern passes through four wrong prefixes, and flashing "wrong" at
    // each of them would be a lie about what they did. The caller hears about the
    // walk once, through settleEntry().
    speculativeOutstanding_ = true;
    return;
  }

  // A CONFIRM is itself the counted attempt, so nothing is owed on top of it.
  speculativeOutstanding_ = false;
  lastAttemptWrong_ = verdict == Verdict::Wrong;
  entry_.wipe();
  LOG_ERR(kLogTag, "attempt refused: %s", lockedOut_ ? "locked out" : "wrong pattern");
  // A real refusal replaces the whole message, so it earns a clean refresh.
  redraw(HalDisplay::HALF_REFRESH);
}

void PatternLockActivity::finishUnlocked() {
  entry_.wipe();
  finished_ = true;
  setResult(ActivityResult{PatternLockResult{true}});
  finish();
}

PatternLockActivity::Layout PatternLockActivity::layout() const {
  Layout l;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // The theme draws its hint boxes only where TouchPolicy::hintsVisible() holds --
  // a board with no digitizer, or a touch board in TOUCH_BUTTONS_ONLY. In
  // TOUCH_ANYWHERE, the default, this screen supplies its own.
  l.ownTouch = TouchPolicy::touchAnywhere();
  l.boxHeight = l.ownTouch ? lineHeight * 2 : 0;

  const int bandTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const Rect hints = GUI.buttonHintsRect(renderer);
  const int hintsTop = hints.height > 0 ? hints.y : pageHeight;
  const int bandBottom =
      (l.ownTouch ? pageHeight - l.boxHeight - metrics.verticalSpacing * 2 : hintsTop) - metrics.verticalSpacing;
  const int band = bandBottom - bandTop;

  // An allowance, not the real content height: the pitch is not known yet and the
  // meter's size depends on it. The exact height is computed below and the
  // centring uses that, so an allowance that is a little off moves nothing.
  const int allowance = lineHeight * 5;
  const int availableWidth = pageWidth - metrics.contentSidePadding * 2;
  const int availableHeight = band - allowance;
  const int span = availableWidth < availableHeight ? availableWidth : availableHeight;
  // A panel too short for the grid would otherwise derive a negative pitch and
  // draw the whole screen inside out. The floor is deliberately ugly rather than
  // clever: if a board ever hits it, the grid looks cramped and says so.
  l.pitch = span > kPitchSpanDiv * 8 ? span / kPitchSpanDiv : 8;

  l.nodeRadius = l.pitch / kNodeRadiusDiv;
  if (l.nodeRadius < kMinNodeRadius) l.nodeRadius = kMinNodeRadius;
  l.ringRadius = l.nodeRadius * kCursorRingMul / kCursorRingDiv;
  // The ring has to clear the dot it surrounds by more than its own width, or the
  // two merge into a blob at the first FAST refresh.
  if (l.ringRadius < l.nodeRadius + kCursorRingWidth * 2) l.ringRadius = l.nodeRadius + kCursorRingWidth * 2;

  l.meterHeight = l.nodeRadius * kMeterHeightMul;
  l.meterWidth = l.pitch * (patternlock::kCols - 1);
  l.meterLeft = (pageWidth - l.meterWidth) / 2;

  // What actually sits under the last grid row: a full pitch of air, the meter,
  // and the status line. Measured rather than assumed, so the block is centred on
  // its real height -- the first cut centred on the allowance and sat low.
  const int belowGrid = l.pitch + l.meterHeight + lineHeight * 2;
  const int contentHeight = l.pitch * (patternlock::kRows - 1) + belowGrid;
  const int slack = band - contentHeight;
  l.gridTop = bandTop + (slack > 0 ? slack / 2 : 0);
  l.gridLeft = (pageWidth - l.pitch * (patternlock::kCols - 1)) / 2;

  // A full pitch of air under the last grid row, not half of one. At half, the
  // meter read as a fifth row of the lattice rather than as a scale beside it --
  // the maintainer's call off the panel, 2026-11-23.
  l.meterTop = l.gridTop + l.pitch * (patternlock::kRows - 1) + l.pitch - l.meterHeight / 2;
  l.statusY = l.meterTop + l.meterHeight + lineHeight * 2;

  if (l.ownTouch) {
    l.boxTop = pageHeight - l.boxHeight - metrics.verticalSpacing;
    l.boxWidth = (pageWidth - metrics.contentSidePadding * 3) / 2;
    l.backLeft = metrics.contentSidePadding;
    l.okLeft = pageWidth - metrics.contentSidePadding - l.boxWidth;
  }
  return l;
}

void PatternLockActivity::draw(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const Layout l = layout();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 config_.title != nullptr ? config_.title : tr(STR_PATTERN_LOCK),
                 config_.hint != nullptr ? config_.hint : tr(STR_PATTERN_LOCK_HINT));

  for (uint8_t cell = 0; cell < patternlock::kCells; ++cell) {
    const int cx = l.gridLeft + (cell % patternlock::kCols) * l.pitch;
    const int cy = l.gridTop + (cell / patternlock::kCols) * l.pitch;
    // Every cell draws its dot, the cursor's included -- the cursor is a position,
    // not a seventeenth kind of point.
    renderer.fillRoundedRect(cx - l.nodeRadius, cy - l.nodeRadius, l.nodeRadius * 2, l.nodeRadius * 2, l.nodeRadius,
                             Color::Black);
    if (cell == entry_.cursor()) {
      renderer.drawRoundedRect(cx - l.ringRadius, cy - l.ringRadius, l.ringRadius * 2, l.ringRadius * 2,
                               kCursorRingWidth, l.ringRadius, true);
    }
  }

  // The meter: how many points are in, and nothing about which ones.
  renderer.drawRoundedRect(l.meterLeft, l.meterTop, l.meterWidth, l.meterHeight, kMeterBorder, l.meterHeight / 2,
                           true);
  const int segInset = l.meterHeight / kMeterInsetDiv;
  const int innerLeft = l.meterLeft + kMeterBorder + segInset / 2;
  const int innerWidth = l.meterWidth - (kMeterBorder + segInset / 2) * 2;
  for (size_t i = 0; i < patternlock::kMaxPoints; ++i) {
    const int x0 = innerLeft + static_cast<int>(i) * innerWidth / static_cast<int>(patternlock::kMaxPoints);
    const int x1 = innerLeft + static_cast<int>(i + 1) * innerWidth / static_cast<int>(patternlock::kMaxPoints);
    if (i < entry_.points()) {
      renderer.fillRect(x0, l.meterTop + segInset, x1 - x0 - kMeterBorder, l.meterHeight - segInset * 2, true);
    } else if (i + 1 < patternlock::kMaxPoints) {
      // A hairline between the slots still to come, so the scale is readable when
      // it is nearly empty. No tick after the last one: that is the border.
      renderer.drawLine(x1, l.meterTop + segInset, x1, l.meterTop + l.meterHeight - segInset, true);
    }
  }

  char line[96];
  const uint32_t wait = config_.waitSecondsLeft ? config_.waitSecondsLeft() : 0;
  if (lockedOut_) {
    snprintf(line, sizeof(line), "%s", tr(STR_PATTERN_LOCK_LOCKED_OUT));
  } else if (wait > 0) {
    // Read on every draw, so a backoff counts down instead of showing the number
    // it had when the screen opened.
    snprintf(line, sizeof(line), tr(STR_PATTERN_LOCK_WAIT), static_cast<int>(wait));
  } else if (lastAttemptWrong_) {
    snprintf(line, sizeof(line), "%s", tr(STR_PATTERN_LOCK_WRONG));
  } else if (!entry_.longEnough()) {
    snprintf(line, sizeof(line), tr(STR_PATTERN_LOCK_SHORT), static_cast<int>(patternlock::kMinPoints));
  } else {
    snprintf(line, sizeof(line), tr(STR_PATTERN_LOCK_PROMPT), static_cast<int>(entry_.points()),
             static_cast<int>(patternlock::kMaxPoints));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, l.statusY, line, true,
                            (lockedOut_ || lastAttemptWrong_) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  if (l.ownTouch) {
    // This screen's own BACK and OK, drawn only where the theme draws none.
    renderer.drawRoundedRect(l.backLeft, l.boxTop, l.boxWidth, l.boxHeight, 2, l.boxHeight / 4, true);
    renderer.drawRoundedRect(l.okLeft, l.boxTop, l.boxWidth, l.boxHeight, 2, l.boxHeight / 4, true);
    const int textY = l.boxTop + (l.boxHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    const char* backLabel = tr(STR_BACK);
    const char* okLabel = tr(STR_OK_BUTTON);
    renderer.drawText(UI_10_FONT_ID, l.backLeft + (l.boxWidth - renderer.getTextWidth(UI_10_FONT_ID, backLabel)) / 2,
                      textY, backLabel);
    renderer.drawText(UI_10_FONT_ID, l.okLeft + (l.boxWidth - renderer.getTextWidth(UI_10_FONT_ID, okLabel)) / 2,
                      textY, okLabel);
  } else {
    // UP/DOWN are the side buttons and they are fixed there, so they get labels --
    // the two directions a rider cannot find by looking at the bottom band.
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(mode);
}
