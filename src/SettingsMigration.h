#pragma once

#include <cstdint>

// The rules a renamed settings key is adopted under.
//
// Pulled out of CrossPointSettings::fromJson() so they can be proved without
// ArduinoJson, a card or a device. The team -> group rename of 2026-10-30
// moved five keys, and the first version of that block skipped the clamp the
// generic loader applies to every other toggle -- a defect a review caught and
// no test could have, because there was no test
// (test/group_migration/, docs/settings-menu.md).

// What a toggle should become when the document may carry it under either
// name.
//
// `newPresent` / `oldPresent`: whether each key is in the document.
// `oldValue`: what the old key says, meaningful only when `oldPresent`.
// `current`: what the generic loader already left in the field -- the new
//            key's value when it was present, the struct default otherwise.
//
// Three rules, in order:
//   1. The new key wins whenever it is there, so a device that has saved once
//      since the rename never reads the stale half again.
//   2. An old value is clamped to 0/1. These are all toggles and the generic
//      loader clamps them; a hand-edited card carrying 2 must not produce the
//      one toggle on the device that is neither.
//   3. Anything else leaves the field alone.
struct SettingsMigratedToggle {
  uint8_t value;
  bool adopted;  // true when the old key was used, which asks for a resave
};

inline SettingsMigratedToggle settingsMigrateToggle(const bool newPresent, const bool oldPresent,
                                                    const uint8_t oldValue, const uint8_t current) {
  if (newPresent || !oldPresent) return {current, false};
  if (oldValue > 1) return {current, true};
  return {oldValue, true};
}
