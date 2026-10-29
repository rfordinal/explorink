#include "IdentityLoadRule.h"

#include <gtest/gtest.h>

// The branch table that decides whether a device keeps its identity.
//
// Three outcomes and no fourth, and the whole point of testing them here is
// that the wrong one is unrecoverable: a key exists in exactly one place, and
// a rule that creates a new one at the wrong moment hands the rider's group a
// stranger with no undo.
//
// The defect this guards against shipped once. The first version asked only
// whether the open had succeeded, which made a busy card indistinguishable
// from an empty one -- and the card is busiest on the map, which is the screen
// that triggers the load.

namespace {
constexpr size_t kExpected = 96;  // 64 bytes of private key, 32 of public
}

TEST(IdentityLoadRule, NoFileIsTheOnlyReasonToMakeAKey) {
  EXPECT_EQ(identityLoadAction(false, false, -1, kExpected), IdentityLoadAction::Create);
}

TEST(IdentityLoadRule, AWholeFileIsUsed) {
  EXPECT_EQ(identityLoadAction(true, true, 96, kExpected), IdentityLoadAction::UseExisting);
}

TEST(IdentityLoadRule, AFileThatWillNotOpenIsNeverReplaced) {
  // The dangerous case: the key is probably fine and the card is merely busy.
  // Creating here would overwrite something that cannot be recreated.
  EXPECT_EQ(identityLoadAction(true, false, -1, kExpected), IdentityLoadAction::Refuse);
}

TEST(IdentityLoadRule, AShortReadIsNeverReplaced) {
  // A truncated write and a short read off a busy bus look identical from
  // here, so neither one gets to destroy the file.
  EXPECT_EQ(identityLoadAction(true, true, 40, kExpected), IdentityLoadAction::Refuse);
  EXPECT_EQ(identityLoadAction(true, true, 0, kExpected), IdentityLoadAction::Refuse);
  EXPECT_EQ(identityLoadAction(true, true, 95, kExpected), IdentityLoadAction::Refuse);
}

TEST(IdentityLoadRule, ALongReadIsRefusedToo) {
  // A file that is bigger than the format is not this format. Reading the
  // first 96 bytes of it and calling that an identity is a guess.
  EXPECT_EQ(identityLoadAction(true, true, 97, kExpected), IdentityLoadAction::Refuse);
}

TEST(IdentityLoadRule, ExistenceAndOpeningAreSeparateQuestions) {
  // The regression test for the shipped defect, stated as the property rather
  // than as a case: with no file the answer is Create, and once a file exists
  // no failure mode may ever reach Create again.
  EXPECT_EQ(identityLoadAction(false, false, -1, kExpected), IdentityLoadAction::Create);
  for (int bytes : {-1, 0, 1, 40, 95, 96, 97, 1000}) {
    for (bool opened : {false, true}) {
      EXPECT_NE(identityLoadAction(true, opened, bytes, kExpected), IdentityLoadAction::Create)
          << "existing file reached Create with opened=" << opened << " bytes=" << bytes;
    }
  }
}
