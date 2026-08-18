#include "WalletCodeActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* kLogTag = "WALLETCODE";

// The label sits this far above the bottom edge of the logical screen, and the
// band checked for ink is this much taller than the text on each side. Small on
// purpose: every pixel spent here is a pixel not available to the code, and the
// code is why the screen exists.
constexpr int kLabelBottomMargin = 10;
constexpr int kLabelBandPad = 4;

}  // namespace

WalletCodeActivity::WalletCodeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int itemIndex,
                                       const char* title, const int codeIndex)
    : Activity("WalletCode", renderer, mappedInput), itemIndex_(itemIndex), codeIndex_(codeIndex) {
  if (title != nullptr) {
    std::strncpy(title_, title, sizeof(title_) - 1);
    title_[sizeof(title_) - 1] = '\0';
  }
}

void WalletCodeActivity::onEnter() {
  Activity::onEnter();
  showCurrent();
}

void WalletCodeActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // LEFT and RIGHT walk the ring in manifest order. Nothing else does anything --
  // see the button table in the header for why CONFIRM and the side pair are
  // deliberately inert here.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (stepCode(+1)) showCurrent();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (stepCode(-1)) showCurrent();
    return;
  }
}

bool WalletCodeActivity::stepCode(const int delta) {
  const int count = codeCount_ > 0 ? codeCount_ : 1;
  if (count <= 1) return false;
  int next = codeIndex_ + delta;
  // Cycling, not clamping. A document's codes are a set the rider flips through
  // at a gate, not a surface with edges -- unlike a page, where a clamp is what
  // paper does.
  while (next < 0) next += count;
  next %= count;
  if (next == codeIndex_) return false;
  codeIndex_ = next;
  return true;
}

bool WalletCodeActivity::drawLabel() {
  // ## Where the label may go
  //
  // Not "the bottom of the screen": the bottom of the screen may be part of the
  // code. The manifest states codeWidthPx / codeHeightPx, and the generator
  // centres the code on the canvas -- but that is an assumption about a tool in
  // another repo, guarding the one thing on this screen that must not be drawn
  // over. So the framebuffer is asked instead: if the band is not blank, there is
  // no label. A tall symbology that fills the panel simply gets none.
  //
  // The quiet zone is inside "not blank" only by accident -- it is white, so a
  // band overlapping it reads as blank. That is why the band is at the very bottom
  // edge and no larger than the text: the further from the code, the smaller the
  // chance of landing in its quiet zone. **Open** -- nothing has been scanned off
  // the panel yet, so "far enough" is reasoned, not measured
  // (docs/wallet-viewer.md).
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int height = renderer.getScreenHeight();
  const int y = height - kLabelBottomMargin - lineHeight;
  if (y <= 0) return false;

  if (renderer.getOrientation() != GfxRenderer::Portrait) {
    // logicalBandIsBlank() knows one logical-to-panel mapping, the Portrait one
    // the wallet's assets are generated for (presentation = 1). In any other
    // orientation the band cannot be located, so nothing is drawn on top of the
    // code at all.
    LOG_INF(kLogTag, "orientation %u is not Portrait; no label", static_cast<unsigned>(renderer.getOrientation()));
    return false;
  }
  const wallet::PanelGeometry live = wallet::livePanel(renderer);
  if (!wallet::logicalBandIsBlank(renderer.getFrameBuffer(), live, y - kLabelBandPad, y + lineHeight + kLabelBandPad)) {
    LOG_INF(kLogTag, "code %s reaches the label band; no label", code_.id);
    return false;
  }

  char symbology[wallet::kSymbologyBufBytes];
  wallet::symbologyLabel(code_.symbology, symbology, sizeof(symbology));
  char label[96];
  if (code_.verified) {
    snprintf(label, sizeof(label), "%s", symbology);
  } else {
    // The marker is the whole reason an unverified code may be shown at all, so it
    // names the consequence ("may not scan"), not just the state.
    snprintf(label, sizeof(label), "%s - %s", symbology, tr(STR_WALLET_CODE_UNVERIFIED));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, label, true,
                            code_.verified ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD);
  return true;
}

void WalletCodeActivity::showCurrent() {
  wallet::CodeLookup found;
  if (!wallet::Store::lookupCode(itemIndex_, codeIndex_, found, error_)) {
    codeCount_ = found.codeCount;
    code_ = wallet::CodeEntry{};
    drawFailure();
    return;
  }
  codeCount_ = found.codeCount;
  code_ = found.code;
  if (found.title[0] != '\0') std::memcpy(title_, found.title, sizeof(title_));

  wallet::AssetHeader header;
  bool loaded = false;
  bool marked = false;
  wallet::CodeVerdict verdict = wallet::CodeVerdict::RefuseAsset;
  {
    // One lock across the read, the label and the refresh. The destination is the
    // framebuffer, which the render task owns too, and a repaint between the read
    // and the refresh would put half a barcode on the glass.
    RenderLock lock;
    loaded = wallet::Store::loadCodeIntoFrameBuffer(code_, renderer, header, error_);
    // The label goes in before the verdict, because the verdict depends on whether
    // it could be placed. Nothing has reached the panel yet either way.
    if (loaded) marked = drawLabel();
    verdict = wallet::codeVerdict(loaded, code_.verified, marked);
    if (verdict == wallet::CodeVerdict::Draw) {
      // ## Refresh: HALF, and never FAST
      //
      // HALF is the strongest clean this panel has. FAST is differential, so it
      // leaves the previous code's modules as ghosts under this one -- fatal for a
      // scanner, which is reading contrast between neighbouring modules and
      // nothing else. FULL is *not* stronger: on the X4 both HALF (0xD7) and FULL
      // (0xF7) rewrite both planes and clear the panel absolutely, and FULL only
      // adds inversion passes on the way (docs/refresh-modes.md). So FULL would
      // buy a flash storm and an untimed wait in front of somebody holding a
      // scanner, and no extra cleanliness.
      //
      // Every code frame pays it -- entry, and every step of the walk. Scanning
      // reliability beats saving a refresh; that is the whole trade this screen
      // makes.
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  switch (verdict) {
    case wallet::CodeVerdict::Draw:
      LOG_INF(kLogTag, "code %d/%u %s %s drawn%s", codeIndex_, static_cast<unsigned>(codeCount_), code_.id,
              code_.symbology, code_.verified ? "" : " UNVERIFIED, marked");
      error_ = wallet::Error::None;
      return;
    case wallet::CodeVerdict::RefuseUnmarked:
      // Drawn but never shown. An unverified code the rider cannot see is
      // unverified is exactly the lie this feature must not tell, so the bitmap is
      // thrown away instead.
      error_ = wallet::Error::CodeUnmarked;
      drawFailure();
      return;
    case wallet::CodeVerdict::RefuseAsset:
      // error_ is whatever loadCodeIntoFrameBuffer set: a bad file, the wrong
      // asset type, or a hash that did not match.
      drawFailure();
      return;
  }
}

void WalletCodeActivity::drawFailure() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  LOG_ERR(kLogTag, "code %d/%u (%s %s) refused: error %u", codeIndex_, static_cast<unsigned>(codeCount_), code_.id,
          code_.symbology, static_cast<unsigned>(error_));

  // Honest, legible, and it names the code -- "this code is damaged" without
  // saying which is useless to a rider holding two boarding passes. Every button
  // still works, so LEFT/RIGHT walk to the next code and BACK leaves.
  RenderLock lock;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 title_[0] != '\0' ? title_ : tr(STR_WALLET), nullptr);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, wallet::errorText(error_), true, EpdFontFamily::BOLD);
  y += lineHeight * 2;

  char which[96];
  char symbology[wallet::kSymbologyBufBytes];
  wallet::symbologyLabel(code_.symbology, symbology, sizeof(symbology));
  snprintf(which, sizeof(which), tr(STR_WALLET_CODE_OF), codeIndex_ + 1,
           static_cast<int>(codeCount_ > 0 ? codeCount_ : 1));
  if (code_.id[0] != '\0') {
    const size_t at = std::strlen(which);
    snprintf(which + at, sizeof(which) - at, " - %s %s", symbology, code_.id);
  }
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, which, true);
  y += lineHeight * 2;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, tr(STR_WALLET_CODE_NOT_SHOWN), true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, tr(STR_WALLET_CODE), tr(STR_WALLET_CODE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // HALF: replacing whatever was on the panel with text.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
