#pragma once

#include <cstdint>

// Which screen Home > Wallet opens. Lifted out of ActivityManager::goToWallet()
// so the host can reach it (test/wallet/WalletEntryTest.cpp): the screen-level
// branch is not host testable, and it was exactly that branch that hid T-2109.
//
// **The wrap in NVS is the fact that a key exists. The card only says whether
// documents do.** The old test sent a key-less board to the gate only when the
// card held an encrypted tree. A board provisioned and then rebooted before its
// first sync has a wrap and an empty card, so it got the browse list ("No wallet
// on the card") and no gate. A locked board refuses every wallet push
// (MapTransferDecide.h, decideWalletGates, "wallet locked"), so that rider could
// never receive a document -- only a dev build's CMD:WALLETUNLOCK got out.
// Measured on t5s3pro-b, 2026-11-28.
//
// Three states, kept apart on purpose (docs/wallet-test-scenarios.md, W1, W8, W9):
//   - a wrap and no key in the session: the gate, whatever the card holds. A
//     cleartext tree on such a board is gated too -- a locked board cannot take
//     a sync onto it either, and the same rule has to hold for both cards.
//     **Maintainer's call, 2026-11-28**, and it costs a rider with a cleartext
//     card an unlock that no cryptography needs. Its own hardware path (W9b)
//     is host-tested and has never run on a board.
//   - no wrap, nothing on the card: the setup offer.
//   - no wrap, a cleartext tree: browse with no key, because such a card exists
//     in the field and reading it needs none (docs/wallet-crypto.md).
// No wrap and an encrypted tree is the dead end; the unlock screen draws its own
// reason for it ("This card was set up somewhere else"), so it routes there too.
enum class WalletEntry : uint8_t {
  Gate,    // WalletUnlockActivity opening the pattern grid
  Setup,   // WalletUnlockActivity offering enrollment
  Refuse,  // WalletUnlockActivity drawing the no-key dead end
  Browse,  // WalletActivity, the list
};

inline WalletEntry walletEntryDecide(const bool provisioned, const bool sessionHasKey, const bool encryptedTree,
                                     const bool cleartextTree) {
  if (sessionHasKey) return WalletEntry::Browse;
  if (provisioned) return WalletEntry::Gate;
  if (encryptedTree) return WalletEntry::Refuse;
  if (!cleartextTree) return WalletEntry::Setup;
  return WalletEntry::Browse;
}
