#pragma once

#include <cstdint>

#include "WalletCrypto.h"
#include "WalletStore.h"
#include "activities/Activity.h"

// The wallet's gate: everything about unlocking an encrypted wallet **except**
// the drawing, which `PatternLockActivity` does.
//
// ## Why this still exists after the screen moved out
//
// The gate screen owns no secret on purpose -- it reports one bit and keeps no
// policy (`activities/lock/PatternLockActivity.h`). Somebody has to hold the
// other half: the wrap, the rate limiter, the persisted failure count, the start
// cell the gate must open on, and the `openItem`/`openCode` a
// `CMD:GOTO_WALLET 0 3` asked for before the gate appeared. That is this class,
// and it is why the wallet's policy did not move into a component every screen
// can include.
//
// It is reached only through `ActivityManager::goToWallet()`, which replaces
// rather than pushes, so this activity stands where Home stood and the gate is
// pushed on top of it. The three ways out:
//
//   unlocked      goToWallet() again -- the session now has K, so it lands on
//                 the list, carrying the item and code through
//   gate left     finish(): the stack is empty, so the manager goes Home
//   nothing to    this screen draws the reason itself and waits for BACK
//   unlock
//
// ## Why it draws at all
//
// Three dead ends have no pattern to enter and so no gate to push: this device
// has no wrap at all (a card provisioned elsewhere), it has a wrap but no stored
// start cell, or the failure count is already at the hard stop. Pushing a gate
// for any of them would put a grid on the panel that cannot succeed whatever a
// rider walks. A sentence saying why is the honest screen, and it is the only
// drawing left here.
//
// **The middle one is the upgrade path and it gets its own sentence.** A wrap
// with no `pstart` is a wallet provisioned before this gate existed, under the
// directional PIN -- the key is right there and unwrappable, and nothing can
// enter the PIN any more. Saying "no wallet key on this device" there is false,
// and it was what this screen said until an X4 Pro carrying a 2026-11-23 wallet
// was put in front of it (2026-11-26).
//
// It replaced a six-to-ten-symbol directional PIN on 2026-11-26. The PIN's own
// claim -- "a shoulder looking at the panel must learn nothing" -- did not
// survive the move and the gate's header says so; what did survive is every
// number in `docs/wallet-crypto.md`, because the pattern length was chosen to
// keep them (`WalletCrypto.h`, "Why 8 to 12 and not the gate's own 4 to 8").
class WalletUnlockActivity final : public Activity {
 public:
  // `openItem` / `openCode` are carried through the unlock so `CMD:GOTO_WALLET 0 3`
  // still lands where it asked to once the pattern is in.
  WalletUnlockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int openItem = -1, int openCode = -1);

  void onEnter() override;
  void loop() override;
  // Without this the manager's own repaint -- the one after the gate pops back
  // here -- leaves the gate's last frame on the panel while this screen is live.
  void render(RenderLock&& lock) override;

 private:
  void openGate();
  void onGateResult(bool unlocked);
  void draw(HalDisplay::RefreshMode mode);

  const int openItem_;
  const int openCode_;
  uint8_t failures_ = 0;
  bool lockedOut_ = false;
  bool noWrap_ = false;
  // A wrap exists but no start cell does: provisioned before the pattern gate.
  // Separate from noWrap_ because the two states deserve different sentences --
  // one device has no key and the other has a key nobody can reach.
  bool noPattern_ = false;
  // The gate has been queued or is on top of this screen. Guards the one-shot
  // push: onEnter queues it, and the manager runs the push a loop later, so
  // loop() would otherwise queue a second one in between.
  bool gateOpened_ = false;
};
