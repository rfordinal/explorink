#pragma once

#include <cstdint>

#include "WalletCrypto.h"
#include "WalletStore.h"
#include "activities/Activity.h"

// The PIN screen: the only way into an encrypted wallet.
//
// ## Why a direction PIN
//
// The device has six buttons and no keyboard. Four of them are directions, and a
// sequence of directions is something a rider can enter in gloves, in the dark,
// without looking -- which is the situation this whole product is for. A digit PIN
// would need a picker per digit.
//
// It is short by construction: 6 to 10 symbols is 4^6 to 4^10, 2^12 to 2^20. **The
// PIN is not the strength.** What makes the wrap hard to attack is the 32-byte
// device secret it is mixed with, and what makes guessing impractical is the rate
// limiter (../../../docs/wallet-crypto.md, "What the PIN is for").
//
// ## What is on screen
//
// Dots, one per symbol entered, and never the directions themselves -- a shoulder
// looking at the panel must learn nothing. Brief section 17.
//
//   LEFT / RIGHT / UP / DOWN   append that symbol
//   CONFIRM                    try it
//   BACK                       one symbol back, or leave the screen when empty
//
// UP/DOWN are the side buttons here, so all four directions are enterable and the
// PIN's alphabet matches what the hardware actually has.
class WalletUnlockActivity final : public Activity {
 public:
  // `openItem` / `openCode` are carried through the unlock so `CMD:GOTO_WALLET 0 3`
  // still lands where it asked to once the PIN is in.
  WalletUnlockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int openItem = -1, int openCode = -1);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void render(HalDisplay::RefreshMode mode);
  void append(wallet::PinSymbol symbol);
  void backspace();
  void submit();
  // Seconds still to wait before the next attempt, 0 when it can go now.
  uint32_t waitSecondsLeft() const;

  const int openItem_;
  const int openCode_;
  // The entered PIN. Wiped in onExit() and after every attempt -- it is the one
  // secret a rider types, and it has no business outliving the screen.
  char entry_[wallet::kPinBufBytes] = {0};
  size_t length_ = 0;
  uint8_t failures_ = 0;
  bool lockedOut_ = false;
  bool noWrap_ = false;
  bool lastAttemptWrong_ = false;
};
