#include "WalletUnlockActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "WalletCryptoDevice.h"
#include "WalletKeyStore.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "WALLETPIN";
// One dot per entered symbol. Big enough to count at a glance in gloves, and
// spaced so nobody can read a length off a smear.
constexpr int kDotRadius = 7;
constexpr int kDotGap = 26;

}  // namespace

WalletUnlockActivity::WalletUnlockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int openItem,
                                           const int openCode)
    : Activity("WalletUnlock", renderer, mappedInput), openItem_(openItem), openCode_(openCode) {}

void WalletUnlockActivity::onEnter() {
  Activity::onEnter();
  length_ = 0;
  wallet::secureWipe(entry_, sizeof(entry_));
  noWrap_ = !wallet::KeyStore::isProvisioned();
  failures_ = wallet::KeyStore::failures();
  lockedOut_ = wallet::pinIsLockedOut(failures_);
  // The delay the persisted failure count already earned. A power cycle does not buy
  // a fresh set of guesses, so the wait is armed on entry too.
  wallet::Session::instance().armRetryDelay(failures_);
  // Attributable heap: the 10-second MEM line says what was free at some moment,
  // this says what was free with THIS screen up. Asked for by the phase that has to
  // report the wallet's resident heap (docs/wallet-crypto.md).
  LOG_INF(kLogTag, "heap on the unlock screen: %lu free, %lu largest block",
          static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  if (noWrap_) LOG_ERR(kLogTag, "no wrap in NVS: this device has no wallet key");
  if (lockedOut_) LOG_ERR(kLogTag, "locked out after %u failures", static_cast<unsigned>(failures_));
  render(HalDisplay::HALF_REFRESH);
}

void WalletUnlockActivity::onExit() {
  Activity::onExit();
  // The PIN dies with the screen, whichever way the screen ended.
  wallet::secureWipe(entry_, sizeof(entry_));
  length_ = 0;
}

uint32_t WalletUnlockActivity::waitSecondsLeft() const {
  // The gate is the session's, not this screen's: CMD:WALLETUNLOCK arms the same one,
  // so neither path can be used to step around a delay the other is enforcing.
  const uint32_t left = wallet::Session::instance().retryWaitMs();
  return left == 0 ? 0 : (left + 999) / 1000;
}

void WalletUnlockActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (length_ > 0) {
      backspace();
      return;
    }
    // Empty and BACK: leave. An encrypted wallet with no key is not a screen to be
    // stuck on.
    finish();
    return;
  }
  if (lockedOut_ || noWrap_) {
    // Nothing to enter. Every other button is inert so the screen cannot be walked
    // into a state that says something untrue.
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    submit();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    append(wallet::PinSymbol::Left);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    append(wallet::PinSymbol::Right);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    append(wallet::PinSymbol::Up);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    append(wallet::PinSymbol::Down);
    return;
  }
}

void WalletUnlockActivity::append(const wallet::PinSymbol symbol) {
  if (length_ >= wallet::kPinMaxLen) return;
  entry_[length_++] = wallet::pinSymbolChar(symbol);
  entry_[length_] = '\0';
  lastAttemptWrong_ = false;
  // FAST: one more dot is a small differential change, and this is the one place in
  // the wallet where a 1.7 s wait per press would make the screen unusable.
  render(HalDisplay::FAST_REFRESH);
}

void WalletUnlockActivity::backspace() {
  if (length_ == 0) return;
  entry_[--length_] = '\0';
  lastAttemptWrong_ = false;
  render(HalDisplay::FAST_REFRESH);
}

void WalletUnlockActivity::submit() {
  // One attempt, through the one shared path -- the same KeyStore::tryUnlock() that
  // CMD:WALLETUNLOCK drives, so what a host verifies is what a rider gets.
  uint32_t unwrapMicros = 0;
  uint32_t waitMs = 0;
  const wallet::UnlockResult result = wallet::KeyStore::tryUnlock(entry_, unwrapMicros, failures_, waitMs);
  // Whatever happened, the typed PIN is done with.
  wallet::secureWipe(entry_, sizeof(entry_));
  length_ = 0;

  lockedOut_ = result == wallet::UnlockResult::LockedOut;
  noWrap_ = result == wallet::UnlockResult::NotProvisioned;
  lastAttemptWrong_ = result == wallet::UnlockResult::BadPin;

  if (result == wallet::UnlockResult::Ok) {
    LOG_INF(kLogTag, "unlocked in %lu us; opening the wallet (item %d code %d)",
            static_cast<unsigned long>(unwrapMicros), openItem_, openCode_);
    // Replace, not push: the PIN screen has no business staying on the stack behind
    // an unlocked wallet, and BACK out of the browse list should go home.
    activityManager.goToWallet(openItem_, openCode_);
    return;
  }

  LOG_ERR(kLogTag, "unlock refused: %s (%u failures)", wallet::unlockResultName(result),
          static_cast<unsigned>(failures_));
  // Malformed and Waiting are cheap answers to a half-finished entry, so they get a
  // FAST redraw; a real refusal replaces the screen's message and gets a clean one.
  render(result == wallet::UnlockResult::Malformed || result == wallet::UnlockResult::Waiting
             ? HalDisplay::FAST_REFRESH
             : HalDisplay::HALF_REFRESH);
}

void WalletUnlockActivity::render(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  RenderLock lock;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WALLET_UNLOCK),
                 tr(STR_WALLET_UNLOCK_HINT));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;

  // The dots. One per symbol, and nothing that says WHICH symbol -- a shoulder
  // looking at the panel learns the length and no more (brief section 17).
  const int dotsWidth = static_cast<int>(wallet::kPinMaxLen - 1) * kDotGap;
  const int dotsLeft = (pageWidth - dotsWidth) / 2;
  for (size_t i = 0; i < wallet::kPinMaxLen; ++i) {
    const int cx = dotsLeft + static_cast<int>(i) * kDotGap;
    if (i < length_) {
      renderer.fillRoundedRect(cx - kDotRadius, y - kDotRadius, kDotRadius * 2, kDotRadius * 2, kDotRadius,
                               Color::Black);
    } else {
      // An empty slot is an outline, so the length of the PIN a rider is used to is
      // visible as they go without spelling anything out.
      renderer.drawRoundedRect(cx - 3, y - 3, 6, 6, 1, 3, true);
    }
  }
  y += lineHeight * 3;

  char line[96];
  if (noWrap_) {
    snprintf(line, sizeof(line), "%s", tr(STR_WALLET_UNLOCK_NO_WRAP));
  } else if (lockedOut_) {
    snprintf(line, sizeof(line), "%s", tr(STR_WALLET_UNLOCK_LOCKED_OUT));
  } else if (const uint32_t wait = waitSecondsLeft(); wait > 0) {
    snprintf(line, sizeof(line), tr(STR_WALLET_UNLOCK_WAIT), static_cast<int>(wait));
  } else if (lastAttemptWrong_) {
    snprintf(line, sizeof(line), "%s", tr(STR_WALLET_UNLOCK_WRONG));
  } else if (length_ < wallet::kPinMinLen) {
    snprintf(line, sizeof(line), tr(STR_WALLET_UNLOCK_SHORT), static_cast<int>(wallet::kPinMinLen));
  } else {
    snprintf(line, sizeof(line), tr(STR_WALLET_UNLOCK_PROMPT), static_cast<int>(length_),
             static_cast<int>(wallet::kPinMaxLen));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, line, true,
                            (lockedOut_ || lastAttemptWrong_) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(mode);
}
