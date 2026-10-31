#include "CardRootMigration.h"

#include <HalStorage.h>
#include <Logging.h>

namespace {
constexpr const char* kLogTag = "SDMIGRATE";
constexpr const char* kLegacyRoot = "/trailink";
constexpr const char* kNewRoot = "/explorink";
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

  switch (action) {
    case CardRootMigrateAction::Nothing:
      return;

    case CardRootMigrateAction::MoveDirectory:
      if (Storage.rename(kLegacyRoot, kNewRoot)) {
        LOG_INF(kLogTag, "moved %s to %s", kLegacyRoot, kNewRoot);
      } else {
        LOG_ERR(kLogTag, "could not move %s to %s -- will retry next boot; nothing was deleted", kLegacyRoot, kNewRoot);
      }
      return;

    case CardRootMigrateAction::Refuse:
      LOG_ERR(kLogTag, "%s and %s both exist -- leaving both alone, reading from %s", kLegacyRoot, kNewRoot, kNewRoot);
      return;
  }
}
