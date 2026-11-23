#include "PatternLockActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
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

}  // namespace

PatternLockActivity::PatternLockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Config config,
                                         Verifier verifier)
    : Activity("PatternLock", renderer, mappedInput),
      config_(config),
      verifier_(std::move(verifier)),
      entry_(config.startCell) {}

void PatternLockActivity::onEnter() {
  Activity::onEnter();
  entry_.clear();
  lastAttemptWrong_ = false;
  lockedOut_ = config_.isLockedOut && config_.isLockedOut();
  if (lockedOut_) LOG_ERR(kLogTag, "entered locked out: the caller is refusing attempts");
  render(HalDisplay::HALF_REFRESH);
}

void PatternLockActivity::onExit() {
  Activity::onExit();
  // The path dies with the screen, whichever way the screen ended.
  entry_.wipe();
}

void PatternLockActivity::loop() {
  Activity::loop();
  if (finished_) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (entry_.undo()) {
      lastAttemptWrong_ = false;
      render(HalDisplay::FAST_REFRESH);
      return;
    }
    // Nothing entered and BACK: leave, cancelled. A gate the caller pushed is the
    // caller's to re-push; this screen does not decide that nobody may go back.
    finished_ = true;
    setResult(ActivityResult{PatternLockResult{false}});
    finish();
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
    if (lockedOut_) {
      render(HalDisplay::HALF_REFRESH);
      return;
    }
  }

  // FAST: one cursor move is a small differential change, and a 1.7 s wait per
  // press would make the screen unusable (the same call the wallet PIN makes).
  render(HalDisplay::FAST_REFRESH);
}

void PatternLockActivity::submit(const bool speculative) {
  char path[patternlock::kPathBufBytes] = {0};
  entry_.encode(path);

  const Verdict verdict = verifier_ ? verifier_(path, speculative) : Verdict::Wrong;
  // Whatever it answered, the typed path has served its purpose.
  for (size_t i = 0; i < sizeof(path); ++i) path[i] = 0;

  if (verdict == Verdict::Ok) {
    LOG_INF(kLogTag, "unlocked on %u points%s", static_cast<unsigned>(entry_.points()),
            speculative ? " (auto)" : "");
    finishUnlocked();
    return;
  }

  lockedOut_ = verdict == Verdict::LockedOut;

  if (speculative) {
    // A probe that missed says nothing on screen. A rider walking toward an
    // 8-point pattern passes through four wrong prefixes, and flashing "wrong" at
    // each of them would be a lie about what they did.
    return;
  }

  lastAttemptWrong_ = verdict == Verdict::Wrong;
  entry_.wipe();
  LOG_ERR(kLogTag, "attempt refused: %s", lockedOut_ ? "locked out" : "wrong pattern");
  // A real refusal replaces the whole message, so it earns a clean refresh.
  render(HalDisplay::HALF_REFRESH);
}

void PatternLockActivity::finishUnlocked() {
  entry_.wipe();
  finished_ = true;
  setResult(ActivityResult{PatternLockResult{true}});
  finish();
}

void PatternLockActivity::render(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  RenderLock lock;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 config_.title != nullptr ? config_.title : tr(STR_PATTERN_LOCK),
                 config_.hint != nullptr ? config_.hint : tr(STR_PATTERN_LOCK_HINT));

  // The band the screen owns: below the header, above the hint boxes. Asking the
  // theme for the hint band rather than guessing it is the cheap half of a lesson
  // the map screen paid for (BaseTheme.h, sideButtonHintsRect()).
  const int bandTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const Rect hints = GUI.buttonHintsRect(renderer);
  const int bandBottom = (hints.height > 0 ? hints.y : pageHeight) - metrics.verticalSpacing;
  const int band = bandBottom - bandTop;
  // What the grid does not get: the dots and the status line under it. The dot
  // row's own size depends on the pitch, which is not known yet, so the reserve
  // uses the line height alone and the row is laid out inside it below.
  const int reserved = lineHeight * 5;
  const int availableWidth = pageWidth - metrics.contentSidePadding * 2;
  const int availableHeight = band - reserved;
  const int span = availableWidth < availableHeight ? availableWidth : availableHeight;
  // A panel too short for the grid would otherwise derive a negative pitch and
  // draw the whole screen inside out. The floor is deliberately ugly rather than
  // clever: if a board ever hits it, the grid looks cramped and says so.
  const int pitch = span > kPitchSpanDiv * 8 ? span / kPitchSpanDiv : 8;

  // Centred in the band rather than hung from the header. On a 540x960 panel the
  // grid is limited by the width, which left ~250 px of white under the status
  // line when the block was top-aligned -- seen on the T5 S3 Pro, 2026-11-23.
  const int contentHeight = pitch * (patternlock::kRows - 1) + reserved;
  const int slack = band - contentHeight;
  const int gridTop = bandTop + (slack > 0 ? slack / 2 : 0);

  int nodeRadius = pitch / kNodeRadiusDiv;
  if (nodeRadius < kMinNodeRadius) nodeRadius = kMinNodeRadius;
  int ringRadius = nodeRadius * kCursorRingMul / kCursorRingDiv;
  // The ring has to clear the dot it surrounds by more than its own width, or the
  // two merge into a blob at the first FAST refresh.
  if (ringRadius < nodeRadius + kCursorRingWidth * 2) ringRadius = nodeRadius + kCursorRingWidth * 2;

  // Half a pitch of inset, so the grid's ink is centred rather than its cell boxes.
  const int gridLeft = (pageWidth - pitch * (patternlock::kCols - 1)) / 2;
  for (uint8_t cell = 0; cell < patternlock::kCells; ++cell) {
    const int cx = gridLeft + (cell % patternlock::kCols) * pitch;
    const int cy = gridTop + (cell / patternlock::kCols) * pitch;
    // Every cell draws its dot, the cursor's included -- the cursor is a position,
    // not a seventeenth kind of point.
    renderer.fillRoundedRect(cx - nodeRadius, cy - nodeRadius, nodeRadius * 2, nodeRadius * 2, nodeRadius,
                             Color::Black);
    if (cell == entry_.cursor()) {
      renderer.drawRoundedRect(cx - ringRadius, cy - ringRadius, ringRadius * 2, ringRadius * 2, kCursorRingWidth,
                               ringRadius, true);
    }
  }

  // The meter: how many points are in, and nothing about which ones.
  const int meterWidth = pitch * (patternlock::kCols - 1);
  const int meterHeight = nodeRadius * kMeterHeightMul;
  const int meterLeft = (pageWidth - meterWidth) / 2;
  // A full pitch of air under the last grid row, not half of one. At half, the row
  // read as a fifth row of the lattice rather than as a scale beside it -- the
  // maintainer's call off the panel, 2026-11-23.
  int y = gridTop + pitch * (patternlock::kRows - 1) + pitch;
  const int meterTop = y - meterHeight / 2;
  renderer.drawRoundedRect(meterLeft, meterTop, meterWidth, meterHeight, kMeterBorder, meterHeight / 2, true);

  const int segInset = meterHeight / kMeterInsetDiv;
  const int innerLeft = meterLeft + kMeterBorder + segInset / 2;
  const int innerWidth = meterWidth - (kMeterBorder + segInset / 2) * 2;
  for (size_t i = 0; i < patternlock::kMaxPoints; ++i) {
    const int x0 = innerLeft + static_cast<int>(i) * innerWidth / static_cast<int>(patternlock::kMaxPoints);
    const int x1 = innerLeft + static_cast<int>(i + 1) * innerWidth / static_cast<int>(patternlock::kMaxPoints);
    if (i < entry_.points()) {
      renderer.fillRect(x0, meterTop + segInset, x1 - x0 - kMeterBorder, meterHeight - segInset * 2, true);
    } else if (i + 1 < patternlock::kMaxPoints) {
      // A hairline between the slots still to come, so the scale is readable when
      // it is nearly empty. No tick after the last one: that is the border.
      renderer.drawLine(x1, meterTop + segInset, x1, meterTop + meterHeight - segInset, true);
    }
  }
  y = meterTop + meterHeight;
  y += lineHeight * 2;

  char line[96];
  if (lockedOut_) {
    snprintf(line, sizeof(line), "%s", tr(STR_PATTERN_LOCK_LOCKED_OUT));
  } else if (lastAttemptWrong_) {
    snprintf(line, sizeof(line), "%s", tr(STR_PATTERN_LOCK_WRONG));
  } else if (!entry_.longEnough()) {
    snprintf(line, sizeof(line), tr(STR_PATTERN_LOCK_SHORT), static_cast<int>(patternlock::kMinPoints));
  } else {
    snprintf(line, sizeof(line), tr(STR_PATTERN_LOCK_PROMPT), static_cast<int>(entry_.points()),
             static_cast<int>(patternlock::kMaxPoints));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, line, true,
                            (lockedOut_ || lastAttemptWrong_) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  // UP/DOWN are the side buttons and they are fixed there, so they get labels --
  // the two directions a rider cannot find by looking at the bottom band.
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(mode);
}
