#include "WalletPairActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "SyncCryptoDevice.h"
#include "SyncPairingStore.h"
#include "WalletCryptoDevice.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {

constexpr const char* kLogTag = "WALLETPAIR";

}  // namespace

WalletPairActivity::WalletPairActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("WalletPair", renderer, mappedInput) {}

void WalletPairActivity::onEnter() {
  Activity::onEnter();
  channel_ = makeUniqueNoThrow<syncchannel::Channel>();
  if (!channel_) {
    LOG_ERR(kLogTag, "OOM: channel (%u bytes)", static_cast<unsigned>(sizeof(syncchannel::Channel)));
  } else {
    uint8_t key[syncnoise::kDhLen];
    syncchannel::PairedPhones table;
    syncstore::loadTable(table);
    ready_ =
        syncstore::loadOrCreateStaticKey(key) && channel_->begin(&syncdevice::crypto(), key, table, syncstore::sink());
    syncnoise::wipe(key, sizeof(key));
    if (!ready_) LOG_ERR(kLogTag, "channel did not start");
  }
  LOG_INF(kLogTag, "heap with the pairing screen up: %lu free, %lu largest block",
          static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  issue();
  renderScreen(HalDisplay::HALF_REFRESH);
}

void WalletPairActivity::onExit() {
  // The code, the static key and any half-done handshake die here, not at the
  // next screen's leisure: a code that outlives its screen is a code nobody is
  // watching (SyncChannel.h, "The code is one attempt, short-lived").
  if (channel_) channel_->shutdown();
  channel_.reset();
  ready_ = false;
  Activity::onExit();
}

void WalletPairActivity::issue() {
  showingCode_ = ready_ && channel_->issueCode(millis());
  if (ready_ && !showingCode_) LOG_INF(kLogTag, "no code: %u phones paired", channel_->table().count);
}

void WalletPairActivity::loop() {
  Activity::loop();

  auto& session = wallet::Session::instance();
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) session.touch();
  if (session.expireIfIdle()) {
    onGoHome(HomeMenuItem::WALLET);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    forgetArmed_ = false;
    issue();
    renderScreen(HalDisplay::HALF_REFRESH);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && ready_ && channel_->table().count > 0) {
    if (!forgetArmed_) {
      forgetArmed_ = true;
      renderScreen(HalDisplay::FAST_REFRESH);
      return;
    }
    forgetArmed_ = false;
    while (channel_->table().count > 0) channel_->forgetPhone(0);
    if (!syncstore::saveTable(channel_->table())) LOG_ERR(kLogTag, "forget not stored");
    issue();
    renderScreen(HalDisplay::HALF_REFRESH);
    return;
  }

  // The code died on the clock (or was spent by a pairing attempt): take it off
  // the panel. CMD:SCREENSHOT is ungated (T-2081), so a dead code left on the
  // glass is harmless but a live one is readable by any cable -- the reason the
  // lifetime is short at all.
  if (showingCode_ && !channel_->codeAlive(millis())) {
    showingCode_ = false;
    renderScreen(HalDisplay::HALF_REFRESH);
  }
}

void WalletPairActivity::renderScreen(const HalDisplay::RefreshMode mode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYNC_PAIR_TITLE),
                 nullptr);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const uint8_t paired = ready_ ? channel_->table().count : 0;
  char line[96];
  snprintf(line, sizeof(line), tr(STR_SYNC_PAIRED_COUNT), static_cast<int>(paired));
  renderer.drawCenteredText(UI_10_FONT_ID, y, line, true);
  y += lineHeight + metrics.verticalSpacing;

  char qr[syncchannel::kQrTextLen + 1];
  if (!ready_) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SYNC_PAIR_FAILED), true, EpdFontFamily::BOLD);
  } else if (showingCode_ && channel_->qrText(millis(), qr, sizeof(qr))) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SYNC_PAIR_SCAN), true, EpdFontFamily::BOLD);
    y += lineHeight + metrics.verticalSpacing;
    // As large as the width allows and no larger than the space above the
    // hints: a phone camera at arm's length reads a big code first time, and
    // e-ink glare costs contrast that module size buys back.
    const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2 - lineHeight * 2;
    int side = pageWidth - metrics.contentSidePadding * 2;
    if (side > bottom - y) side = bottom - y;
    // QrUtils takes a std::string, so the secret makes one heap copy that is
    // freed unwiped. Accepted: the same secret sits in the framebuffer for the
    // code's whole life anyway, and the framebuffer is what CMD:SCREENSHOT
    // reads. Wiping the stack copy is still free.
    QrUtils::drawQrCode(renderer, Rect((pageWidth - side) / 2, y, side, side), std::string(qr));
    syncnoise::wipe(qr, sizeof(qr));
    y += side + metrics.verticalSpacing;
    snprintf(line, sizeof(line), tr(STR_SYNC_PAIR_ONCE), static_cast<int>(syncchannel::kCodeLifetimeMs / 1000));
    renderer.drawCenteredText(UI_10_FONT_ID, y, line, true);
  } else if (paired >= syncchannel::kMaxPhones) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SYNC_PAIR_FULL), true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SYNC_PAIR_EXPIRED), true, EpdFontFamily::BOLD);
  }

  if (forgetArmed_) {
    snprintf(line, sizeof(line), tr(STR_SYNC_FORGET_CONFIRM), static_cast<int>(paired));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - lineHeight * 2, line, true,
                              EpdFontFamily::BOLD);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SYNC_NEW_CODE), paired > 0 ? tr(STR_SYNC_FORGET) : "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(mode);
}
