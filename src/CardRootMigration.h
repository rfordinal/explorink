#pragma once

#include <cstdint>

// The on-card root directory was "/trailink" (the old brand, TrailInk) and is
// now "/explorink". Everything the firmware persists -- GNSS log, power log,
// pins, LoRa identity, the group roster and black box, routes, the tile
// cache -- lived under that one root, so renaming it is a single directory
// move rather than per-subsystem migrations (contrast GroupFiles.h's
// `/trailink/team` -> `/trailink/group` move, which happens one level
// *inside* this root and keeps working unchanged once both its legacy and
// new paths are updated to the `/explorink` prefix).
//
// Same non-atomicity as GroupFiles.h's migration, same reason: FAT has no
// directory-rename primitive, so FatFile::rename() creates the destination
// and removes the source afterwards, and a failure between the two leaves
// both directories. The decision is therefore a function of what is on the
// card, never a single try, and a failure is never latched.
enum class CardRootMigrateAction : uint8_t {
  Nothing,        // no legacy directory; never look again
  MoveDirectory,  // legacy only; rename it whole
  Refuse,         // both exist; do nothing rather than guess or overwrite
};

// Inline, same reason as GroupFiles.h's groupMigrateDecide(): the .cpp pulls
// HalStorage, which does not exist on the host, and this is the half worth
// testing without a card (test/card_root_migration/).
inline CardRootMigrateAction cardRootMigrateDecide(const bool legacyExists, const bool newExists) {
  if (!legacyExists) return CardRootMigrateAction::Nothing;
  if (!newExists) return CardRootMigrateAction::MoveDirectory;
  // Both existing is the normal shape of a rename that failed partway
  // through, not just two boots disagreeing -- same reasoning as
  // GroupFiles.h's groupMigrateDecide(). Unlike that migration there is no
  // single irreplaceable file to rescue here (this moves everything at
  // once, before any subsystem has written under the new root), so the only
  // safe move is to leave both alone and let whichever one the firmware
  // reads decide by its own per-subsystem exists() checks.
  return CardRootMigrateAction::Refuse;
}

// Call once, early at boot, right after Storage.begin() succeeds and before
// any subsystem's lazy init can create anything under the new root --
// otherwise a partial /explorink from normal use could look like a
// half-finished migration. Idempotent and cheap: after the first successful
// run, /trailink does not exist, so every later call is one exists().
void migrateCardRootDir();
