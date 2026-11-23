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
constexpr int kNodeRadiusDiv = 12;      // node dot radius = pitch / 12
constexpr int kMinNodeRadius = 4;
constexpr int kCursorRingMul = 26;      // ring radius = node radius * 2.6
constexpr int kCursorRingDiv = 10;
constexpr int kCursorRingWidth = 3;     // thin enough to read, thick enough to survive a FAST refresh

// One dot per entered point, under the grid. Same role as the wallet PIN's dots:
// a rider counts how far they are without the path being on screen. The gap comes
// off the panel width rather than a pixel count, for the same reason the grid does.
constexpr int kDotGapDiv = 24;
constexpr int kDotRadiusDiv = 4;        // filled dot radius = gap / 4
constexpr int kEmptyDotRadius = 2;

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
  // What the grid does not get: the dots and the status line under it.
  const int dotGap = pageWidth / kDotGapDiv;
  const int dotRadius = dotGap / kDotRadiusDiv;
  const int reserved = lineHeight * 3 + dotRadius * 3;
  const int availableWidth = pageWidth - metrics.contentSidePadding * 2;
  const int availableHeight = band - reserved;
  const int span = availableWidth < availableHeight ? availableWidth : availableHeight;
  // A panel too short for the grid would otherwise derive a negative pitch and
  // draw the whole screen inside out. The floor is deliberately ugly rather than
  // clever: if a board ever hits it, the grid looks cramped and says so.
  const int pitch = span > patternlock::kCols * 8 ? span / patternlock::kCols : 8;

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

  int y = gridTop + pitch * (patternlock::kRows - 1) + pitch / 2 + dotRadius * 2;

  // The dots: how many points are in, and nothing about which ones.
  const int dotsWidth = static_cast<int>(patternlock::kMaxPoints - 1) * dotGap;
  const int dotsLeft = (pageWidth - dotsWidth) / 2;
  for (size_t i = 0; i < patternlock::kMaxPoints; ++i) {
    const int cx = dotsLeft + static_cast<int>(i) * dotGap;
    // A slot not reached yet is a smaller dot of the same shape, not an outline:
    // the row then reads as one scale filling up rather than as two kinds of mark.
    const int r = i < entry_.points() ? dotRadius : kEmptyDotRadius;
    renderer.fillRoundedRect(cx - r, y - r, r * 2, r * 2, r, Color::Black);
  }
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
