// The /trailink -> /explorink card-root rename, proved without a card.
// Mirrors test/group_migration/ (the GroupFiles.h /trailink/team ->
// /trailink/group move), same reasoning: FAT has no atomic directory
// rename, so the decision is a pure function of what is on the card.

#include <gtest/gtest.h>

#include "CardRootMigration.h"

namespace {
CardRootMigrateAction decide(const bool legacy, const bool next) { return cardRootMigrateDecide(legacy, next); }
}  // namespace

TEST(CardRootMigration, ACardThatNeverSawTheOldNameIsLeftAlone) {
  EXPECT_EQ(decide(false, false), CardRootMigrateAction::Nothing);
  EXPECT_EQ(decide(false, true), CardRootMigrateAction::Nothing);
}

TEST(CardRootMigration, TheOrdinaryCaseMovesTheWholeDirectory) {
  EXPECT_EQ(decide(true, false), CardRootMigrateAction::MoveDirectory);
}

TEST(CardRootMigration, BothExistingRefusesRatherThanGuessing) {
  // The normal shape of a rename that failed partway through on FAT
  // (FatFile::rename() creates the destination before removing the source),
  // not just two boots disagreeing. Unlike GroupFiles.h's migration there is
  // no single file to rescue here -- this moves everything at once, before
  // any subsystem has written under the new root -- so the only safe move is
  // to leave both alone.
  EXPECT_EQ(decide(true, true), CardRootMigrateAction::Refuse);
}

TEST(CardRootMigration, EveryInputCombinationHasAnAnswer) {
  for (int i = 0; i < 4; ++i) {
    const CardRootMigrateAction a = decide(i & 1, i & 2);
    EXPECT_TRUE(a == CardRootMigrateAction::Nothing || a == CardRootMigrateAction::MoveDirectory ||
                a == CardRootMigrateAction::Refuse)
        << "state " << i;
  }
}
