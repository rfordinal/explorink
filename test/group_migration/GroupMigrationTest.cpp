// The two migrations the Group rename put on a rider's card, proved without a
// card.
//
// **Neither had a test and the one hardware pass only ever walked the happy
// path.** A code review found that a single failed directory rename stranded
// the roster permanently, which nothing here or on hardware had exercised.
// The decision is a pure function now so that case can be stated as an
// expectation rather than hoped for.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

// Only the decision, which the header keeps inline so no card or HAL is
// needed to prove it.
#include "GroupFiles.h"

namespace {

// Named for what the card looks like, because that is how the bug was
// described: "both directories exist and only one has a roster".
GroupMigrateAction decide(const bool legacyDir, const bool newDir, const bool legacyRoster, const bool newRoster) {
  return groupMigrateDecide(legacyDir, newDir, legacyRoster, newRoster);
}

}  // namespace

TEST(GroupMigration, ACardThatNeverSawTheOldNameIsLeftAlone) {
  EXPECT_EQ(decide(false, false, false, false), GroupMigrateAction::Nothing);
  EXPECT_EQ(decide(false, true, false, true), GroupMigrateAction::Nothing);
}

TEST(GroupMigration, TheOrdinaryCaseMovesTheWholeDirectory) {
  // The history moves with the roster: one rename, no free space needed.
  EXPECT_EQ(decide(true, false, true, false), GroupMigrateAction::MoveDirectory);
  // Black-box files but no roster yet -- still the whole directory.
  EXPECT_EQ(decide(true, false, false, false), GroupMigrateAction::MoveDirectory);
}

TEST(GroupMigration, AHalfFinishedRenameRescuesTheRoster) {
  // **This is the case that was broken.** FAT has no atomic directory rename:
  // FatFile::rename() creates the destination and removes the source
  // afterwards, so a failure in between leaves both. The old code read that
  // as two builds disagreeing and refused forever, stranding a roster the
  // rider had built by hand.
  EXPECT_EQ(decide(true, true, true, false), GroupMigrateAction::RescueRoster);
}

TEST(GroupMigration, TwoRealRostersAreNeverPickedBetween) {
  // Both directories hold a members.json: an old build ran after a new one and
  // each wrote its own. Nothing on the device can tell which the rider means.
  EXPECT_EQ(decide(true, true, true, true), GroupMigrateAction::Refuse);
}

TEST(GroupMigration, NothingToRescueIsNotAnInvitationToGuess) {
  // Both directories, neither holds a roster. There is no roster to save, so
  // moving day files around would only risk the history for nothing.
  EXPECT_EQ(decide(true, true, false, false), GroupMigrateAction::Refuse);
  // And a new roster with no old one is simply the new world.
  EXPECT_EQ(decide(true, true, false, true), GroupMigrateAction::Refuse);
}

TEST(GroupMigration, EveryInputCombinationHasAnAnswer) {
  // Sixteen states, no gaps: the switch over this enum drives a filesystem,
  // and a fallthrough there deletes or strands somebody's group.
  for (int i = 0; i < 16; ++i) {
    const GroupMigrateAction a = decide(i & 1, i & 2, i & 4, i & 8);
    EXPECT_TRUE(a == GroupMigrateAction::Nothing || a == GroupMigrateAction::MoveDirectory ||
                a == GroupMigrateAction::RescueRoster || a == GroupMigrateAction::Refuse)
        << "state " << i;
  }
}

TEST(GroupMigration, NoDecisionEverDeletesOrOverwritesARoster) {
  // The invariant the whole function exists for. RescueRoster is the only
  // action that touches members.json, and it is only ever chosen when the
  // destination has none.
  for (int i = 0; i < 16; ++i) {
    const bool newRoster = i & 8;
    if (decide(i & 1, i & 2, i & 4, newRoster) == GroupMigrateAction::RescueRoster) {
      EXPECT_FALSE(newRoster) << "state " << i << " would overwrite a roster";
    }
  }
}
