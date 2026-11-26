#include "WalletUnlockActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "WalletCryptoDevice.h"
#include "WalletKeyStore.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/lock/PatternLockActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "WALLETGATE";

}  // namespace

WalletUnlockActivity::WalletUnlockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int openItem,
                                           const int openCode)
    : Activity("WalletUnlock", renderer, mappedInput), openItem_(openItem), openCode_(openCode) {}

void WalletUnlockActivity::onEnter() {
  Activity::onEnter();
  noWrap_ = !wallet::KeyStore::isProvisioned();
  // A wrap with no start cell is the pre-gate wallet, not a missing one. The
  // gate cannot open on a cell nobody stored, and re-provisioning is the only
  // way forward -- so say that rather than claiming the key is absent.
  noPattern_ = !noWrap_ && wallet::KeyStore::patternStart() == wallet::kPatternNoStart;
  failures_ = wallet::KeyStore::failures();
  lockedOut_ = wallet::isLockedOut(failures_);
  // The delay the persisted failure count already earned. A power cycle does not buy
  // a fresh set of guesses, so the wait is armed before the gate is even drawn.
  wallet::Session::instance().armRetryDelay(failures_);
  // Attributable heap: the 10-second MEM line says what was free at some moment,
  // this says what was free with THIS screen up. Asked for by the phase that has to
  // report the wallet's resident heap (docs/wallet-crypto.md).
  LOG_INF(kLogTag, "heap on the gate: %lu free, %lu largest block", static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  if (noWrap_) LOG_ERR(kLogTag, "no wrap in NVS: this device has no wallet key");
  if (noPattern_) LOG_ERR(kLogTag, "a wrap with no start cell: provisioned before the pattern gate");
  if (lockedOut_) LOG_ERR(kLogTag, "locked out after %u failures", static_cast<unsigned>(failures_));

  if (noWrap_ || noPattern_ || lockedOut_) {
    // A dead end draws its own reason. No gate: a grid that cannot succeed
    // whatever a rider walks is a screen that lies.
    //
    // HALF, never FULL. `docs/refresh-modes.md`: both clear the panel absolutely
    // and both reseed the differential baseline, and FULL only adds a
    // multi-flash on the way. A sparse frame replacing a dense one needs the
    // clean, which HALF already is.
    draw(HalDisplay::HALF_REFRESH);
    return;
  }
  openGate();
}

void WalletUnlockActivity::openGate() {
  if (gateOpened_) return;
  gateOpened_ = true;

  PatternLockActivity::Config config;
  config.title = tr(STR_WALLET_UNLOCK);
  config.hint = tr(STR_WALLET_UNLOCK_HINT);
  // The start cell the wrap was made against. Read before the gate is built,
  // because a gate opened on any other cell refuses every pattern a rider can
  // walk (PatternLock.h, isWellFormed).
  config.startCell = wallet::KeyStore::patternStart();
  // The wallet's own range, not the gate's default of 4 to 8. A 4-point path is
  // 46 walks; the dot PIN this replaced had a floor of 4,096
  // (WalletCrypto.h, "Why 8 to 12 and not the gate's own 4 to 8").
  config.minPoints = wallet::kPatternMinPoints;
  config.maxPoints = wallet::kPatternMaxPoints;
  // Off, and not negotiable here: auto-submit probes the verifier after every
  // point, and this verifier runs a 905 ms PBKDF2 per call. Four probes on the
  // way to a 12-point pattern would be four seconds of a frozen panel, and the
  // watchdog panics at five (docs/wallet-plan.md).
  config.autoSubmit = false;
  config.isLockedOut = [] { return wallet::isLockedOut(wallet::KeyStore::failures()); };
  // Re-read per draw so the wait counts down on the panel. The session's timer,
  // not this screen's, because CMD:WALLETUNLOCK arms the same one -- neither path
  // can be used to step around a delay the other is enforcing.
  config.waitSecondsLeft = [] {
    const uint32_t left = wallet::Session::instance().retryWaitMs();
    return left == 0 ? 0u : (left + 999) / 1000;
  };
  // No onEntryEnded: that callback exists to charge one attempt per walk when
  // auto-submit fires probes the verifier must not count. With auto-submit off
  // every call to the verifier is a CONFIRM, and tryUnlock() counts it there.

  startActivityForResult(
      std::make_unique<PatternLockActivity>(
          renderer, mappedInput, config,
          [](const char* path, const bool speculative) {
            (void)speculative;  // always false: autoSubmit is off above
            // One attempt, through the one shared path -- the same
            // KeyStore::tryUnlock() that CMD:WALLETUNLOCK drives, so what a host
            // verifies is what a rider gets. It installs K in the session itself
            // on success; this lambda keeps nothing.
            uint32_t unwrapMicros = 0;
            uint32_t waitMs = 0;
            uint8_t failures = 0;
            const wallet::UnlockResult result = wallet::KeyStore::tryUnlock(path, unwrapMicros, failures, waitMs);
            if (result == wallet::UnlockResult::Ok) {
              LOG_INF(kLogTag, "unlocked in %lu us", static_cast<unsigned long>(unwrapMicros));
              return PatternLockActivity::Verdict::Ok;
            }
            LOG_ERR(kLogTag, "unlock refused: %s (%u failures)", wallet::unlockResultName(result),
                    static_cast<unsigned>(failures));
            // LockedOut is its own verdict so the gate stops taking presses and
            // says why; everything else reads as a wrong pattern on the panel,
            // which is what it is from a rider's side.
            return result == wallet::UnlockResult::LockedOut ? PatternLockActivity::Verdict::LockedOut
                                                             : PatternLockActivity::Verdict::Wrong;
          }),
      [this](const ActivityResult& result) {
        const auto* gate = std::get_if<PatternLockResult>(&result.data);
        onGateResult(gate != nullptr && gate->unlocked);
      });
}

void WalletUnlockActivity::onGateResult(const bool unlocked) {
  gateOpened_ = false;
  if (!unlocked) {
    // The rider walked out of the gate. The stack is empty behind this screen
    // (goToWallet replaces), so the manager takes it Home.
    LOG_INF(kLogTag, "gate left without unlocking");
    finish();
    return;
  }
  LOG_INF(kLogTag, "opening the wallet (item %d code %d)", openItem_, openCode_);
  // Replace, not push: this screen has no business staying on the stack behind an
  // unlocked wallet, and BACK out of the browse list should go home. The session
  // has K now, so goToWallet() goes straight to the list this time.
  activityManager.goToWallet(openItem_, openCode_);
}

void WalletUnlockActivity::loop() {
  Activity::loop();
  // Only the dead-end screens reach here with no gate in front: BACK leaves, and
  // every other button is inert so the screen cannot be walked into a state that
  // says something untrue.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
}

void WalletUnlockActivity::render(RenderLock&& lock) {
  // The manager already holds the lock; draw into the frame it is offering.
  draw(HalDisplay::HALF_REFRESH);
}

void WalletUnlockActivity::draw(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WALLET_UNLOCK),
                 tr(STR_WALLET_UNLOCK_HINT));

  const int y = metrics.topPadding + metrics.headerHeight + lineHeight * 4;
  // Two lines, bold label over plain detail, mirroring RouteEmptyActivity --
  // the house shape for an empty-state explanation. One line was not a style
  // choice, it was a bug: "This wallet predates the pattern lock. Set it up
  // again." is 52 characters and `drawCenteredText` clipped it at BOTH ends on
  // the X4 Pro's 480 px panel while fitting the X3's 528 px (2026-11-26). A
  // message sized against one panel is the defect CLAUDE.md's "Styles must be
  // universal" names; splitting at the sentence keeps every line short enough
  // that no panel in the line has to be measured again.
  const char* label = noWrap_      ? tr(STR_WALLET_UNLOCK_NO_WRAP)
                      : noPattern_ ? tr(STR_WALLET_UNLOCK_NO_PATTERN)
                                   : tr(STR_WALLET_UNLOCK_LOCKED_OUT);
  const char* detail = noWrap_      ? tr(STR_WALLET_UNLOCK_NO_WRAP_SUB)
                       : noPattern_ ? tr(STR_WALLET_UNLOCK_NO_PATTERN_SUB)
                                    : tr(STR_WALLET_UNLOCK_LOCKED_OUT_SUB);
  renderer.drawCenteredText(UI_10_FONT_ID, y, label, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight, detail, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(mode);
}
