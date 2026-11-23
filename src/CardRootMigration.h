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
// both directories -- empty on the new side, since nothing has repointed it
// yet. CardRootMigration.cpp's tryMove() rescues exactly that shape (an empty
// `/explorink` can be rmdir'd with no data at risk, then the move retried) on
// the same boot immediately and, failing that, on the next boot's Refuse
// path too. The decision below is therefore a function of what is on the
// card, never a single try, and a failure is never latched.
//
// **What the rescue does not cover** (found by review 2026-10-31, after an
// earlier version of this file claimed the opposite): once a subsystem has
// written ANYTHING under `/explorink` on its own -- PowerLog, GnssLog and
// PinLog all lazily create their own directory on first write with no
// knowledge of this migration, and PowerLog's first write can happen as
// early as this boot's first loop() iteration -- the new root is no longer
// empty, rmdir refuses, and a genuinely interrupted rename becomes a
// permanent Refuse with real history stuck under `/trailink`. Narrowing that
// window further needs every such subsystem to defer its own directory
// creation until this migration has run, which is out of this file's scope.
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
  // GroupFiles.h's groupMigrateDecide(). An EARLIER version of this comment
  // claimed there was "no single irreplaceable file to rescue here... so the
  // only safe move is to leave both alone" -- that was wrong, found by
  // review 2026-10-31: the interrupted-rename shape leaves `/explorink`
  // EMPTY (SdFat creates the destination before repointing it to the
  // source's data, CardRootMigration.cpp's tryMove() has the exact mechanics), and an
  // empty directory is rescuable without guessing -- `Storage.rmdir()` only
  // ever succeeds on one. migrateCardRootDir() does that rescue before
  // falling back to this Refuse action; this decide function only picks the
  // action, so by the time a caller sees Refuse the cheap rescue has already
  // been tried and failed (genuinely non-empty, not just interrupted).
  return CardRootMigrateAction::Refuse;
}

// Call once, early at boot, right after Storage.begin() succeeds and before
// any subsystem's lazy init can create anything under the new root --
// otherwise a partial /explorink from normal use could look like a
// half-finished migration. Idempotent and cheap: after the first successful
// run, /trailink does not exist, so every later call is one exists().
void migrateCardRootDir();
