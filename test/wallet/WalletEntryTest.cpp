// Which screen Home > Wallet opens (WalletEntryDecide.h). T-2109: a provisioned
// board with an empty card used to get the browse list and no gate, so a rider
// who rebooted before the first sync could never unlock -- and a locked board
// refuses every wallet push.

#include <gtest/gtest.h>

#include "WalletEntryDecide.h"

namespace {
// Argument order spelled out once, so a row below reads as a card state.
WalletEntry decide(const bool provisioned, const bool key, const bool encrypted, const bool cleartext) {
  return walletEntryDecide(provisioned, key, encrypted, cleartext);
}
}  // namespace

TEST(WalletEntry, AProvisionedBoardWithAnEmptyCardShowsTheGate) {
  // The T-2109 case itself: wrap in NVS, no key, nothing on the card.
  EXPECT_EQ(decide(true, false, false, false), WalletEntry::Gate);
}

TEST(WalletEntry, AProvisionedBoardWithAnEncryptedTreeShowsTheGate) {
  EXPECT_EQ(decide(true, false, true, false), WalletEntry::Gate);
}

TEST(WalletEntry, AProvisionedBoardWithACleartextTreeIsGatedToo) {
  // A locked board refuses a sync onto a cleartext card as well, so the wrap,
  // not the card, has to decide here too.
  EXPECT_EQ(decide(true, false, false, true), WalletEntry::Gate);
}

TEST(WalletEntry, AKeyInTheSessionAlwaysBrowses) {
  // The second call of an unlock. An empty card then says "No wallet on the
  // card", which is the truth and not the bug.
  for (int card = 0; card < 4; ++card) {
    EXPECT_EQ(decide(true, true, card & 1, card & 2), WalletEntry::Browse) << "card " << card;
  }
}

TEST(WalletEntry, AColdBoardWithAnEmptyCardOffersSetup) {
  // W1.
  EXPECT_EQ(decide(false, false, false, false), WalletEntry::Setup);
}

TEST(WalletEntry, AColdBoardWithAnEncryptedCardIsTheDeadEnd) {
  // W9: never offer to mint a new key over documents this board cannot see.
  EXPECT_EQ(decide(false, false, true, false), WalletEntry::Refuse);
  EXPECT_EQ(decide(false, false, true, true), WalletEntry::Refuse);
}

TEST(WalletEntry, AColdBoardWithACleartextCardBrowsesWithNoKey) {
  // Such a card exists in the field and reading it needs no key.
  EXPECT_EQ(decide(false, false, false, true), WalletEntry::Browse);
}

TEST(WalletEntry, OnlyAKeyOrACleartextCardWithNoWrapReachesTheList) {
  // Every one of the 16 states, against the rule in one line: the list without
  // a key only when there is no wrap and the card is plain.
  for (int i = 0; i < 16; ++i) {
    const bool provisioned = i & 1, key = i & 2, encrypted = i & 4, cleartext = i & 8;
    const bool browse = key || (!provisioned && !encrypted && cleartext);
    EXPECT_EQ(decide(provisioned, key, encrypted, cleartext) == WalletEntry::Browse, browse) << "state " << i;
  }
}
