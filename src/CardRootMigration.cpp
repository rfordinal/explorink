#include "CardRootMigration.h"

#include <HalStorage.h>
#include <Logging.h>

namespace {
constexpr const char* kLogTag = "SDMIGRATE";
constexpr const char* kLegacyRoot = "/trailink";
constexpr const char* kNewRoot = "/explorink";

// One rename attempt. On failure, checks whether the attempt got far enough
// to create an empty `/explorink` before failing -- SdFat's FatFile::rename()
// does this as two separate steps (create the destination entry, THEN
// repoint it at the source's data and remove the source), so an I/O error or
// a power loss between them leaves exactly this: an empty new directory next
// to a legacy one that still holds everything. `Storage.rmdir()` only
// succeeds on an empty directory, so clearing it can never destroy real
// data -- if it succeeds, that confirms this was the interrupted-rename
// shape, not a genuine conflict, and the move is retried once, immediately,
// before anything else in this boot gets a chance to write under
// `/explorink` (see migrateCardRootDir()'s own doc comment on why that
// matters).
bool tryMove() {
  if (Storage.rename(kLegacyRoot, kNewRoot)) {
    LOG_INF(kLogTag, "moved %s to %s", kLegacyRoot, kNewRoot);
    return true;
  }
  if (Storage.exists(kNewRoot) && Storage.rmdir(kNewRoot)) {
    LOG_INF(kLogTag, "%s was empty after a failed move -- an earlier rename must have been interrupted; retrying",
            kNewRoot);
    if (Storage.rename(kLegacyRoot, kNewRoot)) {
      LOG_INF(kLogTag, "moved %s to %s", kLegacyRoot, kNewRoot);
      return true;
    }
  }
  LOG_ERR(kLogTag, "could not move %s to %s -- nothing was deleted", kLegacyRoot, kNewRoot);
  return false;
}
}  // namespace

void migrateCardRootDir() {
  // Not latched before the rename: a `done` flag set here would strand a
  // card exactly the way the first version of GroupFiles.h's migration did
  // (GroupFiles.cpp, groupMigrateLegacyDir()) -- one failure, and every later
  // boot refuses to look again. The directory's own existence is the flag.
  if (!Storage.ready()) return;

  const bool legacyExists = Storage.exists(kLegacyRoot);
  const bool newExists = Storage.exists(kNewRoot);
  const CardRootMigrateAction action = cardRootMigrateDecide(legacyExists, newExists);

  // DBG, not silent: a hardware pass with no line here cannot tell "nothing
  // to migrate" from "never ran" (found testing this on an X3, 2026-10-31).
  LOG_DBG(kLogTag, "legacy=%s(%d) new=%s(%d)", kLegacyRoot, legacyExists, kNewRoot, newExists);

  switch (action) {
    case CardRootMigrateAction::Nothing:
      return;

    case CardRootMigrateAction::MoveDirectory:
      tryMove();
      return;

    case CardRootMigrateAction::Refuse:
      // Both existing on a FRESH boot (as opposed to tryMove()'s own
      // same-boot rescue above) is the shape left by a rename that was
      // interrupted on an EARLIER boot and never got the chance to retry --
      // e.g. this function never got to run again before the device was
      // power-cycled a second time. Still safe to attempt the same rescue:
      // rmdir only ever succeeds on an empty directory.
      //
      // **What this does not cover, confirmed by review 2026-10-31**: this
      // function runs once, early, before anything else touches the card --
      // but nothing stops a LATER boot's own subsystem writes (PowerLog,
      // GnssLog, PinLog all lazily create their own directory under the new
      // root on first write, unconditionally, with no knowledge of this
      // migration) from populating a non-empty `/explorink` before the device
      // is power-cycled again. Once that happens, `/explorink` is no longer
      // empty, rmdir refuses, and this becomes a permanent Refuse with the
      // rider's real history still sitting unreachable under `/trailink`.
      // Closing that gap needs every such subsystem to defer its own
      // directory creation until this migration has had a chance to run and
      // succeed, which is a wider change than this task's scope. T-2054 (the
      // SD-card hardware pass with real legacy data) is where this residual
      // window should get a real test, not just a read-through.
      if (Storage.rmdir(kNewRoot)) {
        LOG_INF(kLogTag, "%s was empty -- retrying the move", kNewRoot);
        tryMove();
        return;
      }
      LOG_ERR(kLogTag, "%s and %s both exist and %s is not empty -- leaving both alone, reading from %s", kLegacyRoot,
              kNewRoot, kNewRoot, kNewRoot);
      return;
  }
}
