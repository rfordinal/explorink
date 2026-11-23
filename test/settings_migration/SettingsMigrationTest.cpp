// The rules a renamed settings key is adopted under.
//
// The team -> group rename moved five keys on every card in the field. The
// first version of the adoption block bypassed the clamp the generic loader
// applies to every other toggle; a review caught it and no test could have,
// because CrossPointSettings::fromJson() has never had one.

#include <gtest/gtest.h>

#include "SettingsMigration.h"

namespace {
constexpr uint8_t kDefault = 1;  // what the struct initialiser left behind
}

TEST(SettingsMigration, TheNewKeyWinsWheneverItIsThere) {
  // A device that has saved once since the rename must never read the stale
  // half again -- even while the old key is still in the file, which it is
  // until the resave lands.
  const auto m = settingsMigrateToggle(/*newPresent=*/true, /*oldPresent=*/true, /*oldValue=*/0, /*current=*/1);
  EXPECT_EQ(m.value, 1);
  EXPECT_FALSE(m.adopted);
}

TEST(SettingsMigration, AnOldKeyIsAdoptedWhenTheNewOneIsAbsent) {
  const auto m = settingsMigrateToggle(false, true, 0, kDefault);
  EXPECT_EQ(m.value, 0) << "a rider who switched this off must stay switched off";
  EXPECT_TRUE(m.adopted) << "adoption has to ask for a resave or it repeats every boot";
}

TEST(SettingsMigration, ADeliberateZeroSurvives) {
  // The bug this guards against is treating 0 as "absent". Two of the five
  // keys default to on, so losing a stored 0 turns a feature back on behind
  // the rider -- including one that writes their track to the card.
  const auto m = settingsMigrateToggle(false, true, 0, 1);
  EXPECT_EQ(m.value, 0);
  EXPECT_TRUE(m.adopted);
}

TEST(SettingsMigration, AnOutOfRangeOldValueIsRefusedNotStored) {
  // **This is what the review found.** The generic loader clamps every toggle
  // to 0/1; this path bypassed it, so a hand-edited card could leave exactly
  // one toggle on the device holding 2.
  const auto m = settingsMigrateToggle(false, true, 2, kDefault);
  EXPECT_EQ(m.value, kDefault) << "an impossible value must not reach the field";
  EXPECT_TRUE(m.adopted) << "still resave, so the bad key leaves the card";

  for (int bad = 2; bad < 256; ++bad) {
    EXPECT_EQ(settingsMigrateToggle(false, true, static_cast<uint8_t>(bad), kDefault).value, kDefault)
        << "oldValue " << bad;
  }
}

TEST(SettingsMigration, NeitherKeyLeavesTheFieldAlone) {
  const auto m = settingsMigrateToggle(false, false, 0, kDefault);
  EXPECT_EQ(m.value, kDefault);
  EXPECT_FALSE(m.adopted) << "nothing was migrated, so nothing should force a write";
}

TEST(SettingsMigration, TheResultIsAlwaysAValidToggle) {
  for (int np = 0; np < 2; ++np)
    for (int op = 0; op < 2; ++op)
      for (int ov = 0; ov < 256; ++ov)
        for (int cur = 0; cur < 2; ++cur) {
          const auto m = settingsMigrateToggle(np, op, static_cast<uint8_t>(ov), static_cast<uint8_t>(cur));
          EXPECT_LE(m.value, 1) << "np=" << np << " op=" << op << " ov=" << ov << " cur=" << cur;
        }
}
