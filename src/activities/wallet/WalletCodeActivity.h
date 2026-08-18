#pragma once

#include <cstdint>

#include "WalletStore.h"
#include "activities/Activity.h"

// One machine-readable code, fullscreen, meant to be read by a scanner off the
// glass rather than by a person.
//
// ## Why this screen is stricter than the document viewer
//
// A wrong pixel on a passport scan is cosmetic. A wrong pixel in a barcode is a
// rider at a gate with a pass that will not scan, and no way to tell why. So,
// unlike WalletViewActivity:
//
//   * the payload's sha256 is checked before anything reaches the panel
//     (Store::loadCodeIntoFrameBuffer);
//   * an unverified code is drawn only if it can be *marked* unverified;
//   * the refresh is never FAST -- see the refresh note in the .cpp.
//
// The asset itself is an ordinary full-screen panel-native tile, so the read path
// is the one that already existed.
//
// ## Buttons
//
//   LEFT / RIGHT   previous / next code of this document, cycling
//   BACK           back to the browse list
//   CONFIRM        nothing. There is no zoom for a code: it is already drawn as
//                  large as the panel allows, and a scanner needs the quiet zone
//                  more than the rider needs a bigger picture.
//   UP / DOWN      nothing. A code is one screen; there is nothing to pan and no
//                  page to turn -- the codes of every page are already in this
//                  one walk.
//
// ## What is on the screen
//
// The code, and one line naming the symbology -- placed only where the
// framebuffer proves there is no ink (WalletAsset.h, logicalBandIsBlank). The
// quiet zone is part of the code, so nothing may be drawn near it: a status line
// across a quiet zone breaks a scan exactly as a mark across a module does.
//
// Read-only: no write path of any kind.
class WalletCodeActivity final : public Activity {
 public:
  WalletCodeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int itemIndex, const char* title,
                     int codeIndex);

  void onEnter() override;
  void loop() override;

 private:
  void showCurrent();
  void drawFailure();
  // Cycles: the codes of one document are a ring, so RIGHT off the last one comes
  // back to the first. A single code has nowhere to step and returns false.
  bool stepCode(int delta);
  // Draws the symbology label, and the unverified marker when there is one, into
  // the bottom margin. False when the band that would hold it is not blank -- see
  // the placement note in the .cpp.
  bool drawLabel();

  const int itemIndex_;
  char title_[wallet::kTitleBufBytes] = {0};
  int codeIndex_ = 0;
  uint16_t codeCount_ = 0;
  wallet::CodeEntry code_;
  wallet::Error error_ = wallet::Error::None;
};
