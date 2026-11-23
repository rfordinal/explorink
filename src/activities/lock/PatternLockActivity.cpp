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
constexpr int kNodeRadiusDiv = 6;   // node ring radius = pitch / 6
constexpr int kCursorRadiusDiv = 4; // the filled cursor is bigger than a node ring
constexpr int kMinNodeRadius = 4;

// One dot per entered point, under the grid. Same role as the wallet PIN's dots:
// a rider counts how far they are without the path being on screen.
constexpr int kDotRadius = 6;
constexpr int kDotGap = 22;

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

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (entry_.undo()) {
      lastAttemptWrong_ = false;
      render(HalDisplay::FAST_REFRESH);
      return;
    }
    // Nothing entered and BACK: leave, cancelled. A gate the caller pushed is the
    // caller's to re-push; this screen does not decide that nobody may go back.
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
    // submit() either left the screen or decided nothing is to be said about a
    // speculative miss. Either way the cursor moved, so draw it.
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

  const int gridTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  // What the grid may have: the panel minus the header above and minus the dots,
  // the status line and the hint band below. Asking the theme for the hint band
  // rather than guessing it is the cheap half of a lesson the map screen paid for
  // (BaseTheme.h, sideButtonHintsRect()).
  const Rect hints = GUI.buttonHintsRect(renderer);
  const int bottomLimit = (hints.height > 0 ? hints.y : pageHeight) - lineHeight * 3 - kDotRadius * 3;
  const int availableHeight = bottomLimit - gridTop;
  const int availableWidth = pageWidth - metrics.contentSidePadding * 2;
  const int span = availableWidth < availableHeight ? availableWidth : availableHeight;
  const int pitch = span / patternlock::kCols;

  int nodeRadius = pitch / kNodeRadiusDiv;
  if (nodeRadius < kMinNodeRadius) nodeRadius = kMinNodeRadius;
  int cursorRadius = pitch / kCursorRadiusDiv;
  if (cursorRadius <= nodeRadius) cursorRadius = nodeRadius + 2;

  // Half a pitch of inset, so the grid's ink is centred rather than its cell boxes.
  const int gridLeft = (pageWidth - pitch * (patternlock::kCols - 1)) / 2;
  for (uint8_t cell = 0; cell < patternlock::kCells; ++cell) {
    const int cx = gridLeft + (cell % patternlock::kCols) * pitch;
    const int cy = gridTop + (cell / patternlock::kCols) * pitch;
    if (cell == entry_.cursor()) {
      // The cursor is the only thing on the grid that moves, and the only thing a
      // rider has to find in gloves: filled, and bigger than a node.
      renderer.fillRoundedRect(cx - cursorRadius, cy - cursorRadius, cursorRadius * 2, cursorRadius * 2, cursorRadius,
                               Color::Black);
    } else {
      renderer.drawRoundedRect(cx - nodeRadius, cy - nodeRadius, nodeRadius * 2, nodeRadius * 2, 1, nodeRadius, true);
    }
  }

  int y = gridTop + pitch * (patternlock::kRows - 1) + pitch / 2 + kDotRadius * 2;

  // The dots: how many points are in, and nothing about which ones.
  const int dotsWidth = static_cast<int>(patternlock::kMaxPoints - 1) * kDotGap;
  const int dotsLeft = (pageWidth - dotsWidth) / 2;
  for (size_t i = 0; i < patternlock::kMaxPoints; ++i) {
    const int cx = dotsLeft + static_cast<int>(i) * kDotGap;
    if (i < entry_.points()) {
      renderer.fillRoundedRect(cx - kDotRadius, y - kDotRadius, kDotRadius * 2, kDotRadius * 2, kDotRadius,
                               Color::Black);
    } else {
      // An outline for a slot not reached yet, so the length a rider is used to is
      // visible as they go without the pattern being spelled out.
      renderer.drawRoundedRect(cx - 3, y - 3, 6, 6, 1, 3, true);
    }
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
