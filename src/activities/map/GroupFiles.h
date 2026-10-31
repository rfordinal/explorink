#pragma once

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS

#include <cstdint>

#include "GroupRecord.h"
#include "GroupRoster.h"
#include "GroupSource.h"
#include "GroupStore.h"

// The two files the group-markers layer owns, and the only code here that
// touches storage. Everything they need parsed lives in GroupRoster and
// GroupRecord, so the format rules stay host-testable with no card attached
// (test/group/).
//
// All access goes through HalStorage -- SdFat is not thread-safe (CLAUDE.md).
//
// The paths keep `/trailink`: the on-card layout is infrastructure not yet
// renamed (parent CLAUDE.md, Naming).

// Moves `/trailink/team` to `/trailink/group` once, if the old directory is
// there and the new one is not.
//
// **The rename of 2026-10-30 reached the card, so the card needs a step.**
// Everything the group layer owns lived under `/trailink/team`: the roster a
// rider built by hand, member by member, and every black-box day file since
// the layer shipped. Shipping the new path without this would have read as the
// group having been wiped -- the roster empty, the history gone -- with both
// still sitting on the card under a name nothing opens any more.
//
// One directory rename, not a file-by-file copy: it needs no free space and
// it moves the history with the roster.
//
// **It is not atomic, and an earlier comment here claimed it was.** FAT has
// no directory rename primitive: FatFile::rename() creates the destination
// and removes the source afterwards, so a failure between the two leaves both
// directories. That is why the decision below is a function of what is on the
// card rather than a single try, and why a failure is never latched -- the
// next call picks the half-done state up and rescues the roster.
//
// Nothing is ever deleted. The worst outcome is both directories present and
// a line in the log saying which one is being read.
//
// Idempotent and cheap: after the first successful run `/trailink/team` does
// not exist, so every later call is one `exists()`. Delete this once no card
// in use predates 2026-10-30.
// What the card's state says to do about `/trailink/team`.
//
// Pulled out of groupMigrateLegacyDir() so it can be proved on the host:
// the function itself needs a card, and the case that matters most -- a
// rename that failed halfway -- had never run anywhere (test/group/).
enum class GroupMigrateAction : uint8_t {
  Nothing,        // no legacy directory; never look again
  MoveDirectory,  // legacy only; rename it whole
  RescueRoster,   // both exist and only the legacy one has a roster
  Refuse,         // both exist and the new one has a roster of its own
};

// `legacyExists` / `newExists`: the two directories.
// `legacyHasRoster` / `newHasRoster`: whether each holds members.json.
//
// Inline in the header on purpose: GroupFiles.cpp pulls in HalStorage, which
// does not exist on the host, and this is the half that had to become
// testable (test/group_migration/).
inline GroupMigrateAction groupMigrateDecide(const bool legacyExists, const bool newExists, const bool legacyHasRoster,
                                             const bool newHasRoster) {
  if (!legacyExists) return GroupMigrateAction::Nothing;
  if (!newExists) return GroupMigrateAction::MoveDirectory;
  // **Both directories is the normal shape of a half-done rename, not just of
  // two builds disagreeing.** On FAT a directory rename is not atomic:
  // FatFile::rename() creates the destination and only then removes the
  // source, so any failure in between leaves exactly this. The first version
  // of this code asserted the opposite in a comment and refused on sight,
  // which stranded the roster under the old name forever.
  if (newHasRoster) return GroupMigrateAction::Refuse;
  return legacyHasRoster ? GroupMigrateAction::RescueRoster : GroupMigrateAction::Refuse;
}

void groupMigrateLegacyDir();

// The allowlist. One document, rewritten whole on every edit (GroupRoster.h says
// why there is no history here).
class GroupRosterFile {
 public:
  static constexpr const char* kDir = "/trailink/group";
  static constexpr const char* kPath = "/trailink/group/members.json";

  // A file that does not exist yet is a successful empty load, exactly like the
  // pins log: a rider who has never added anybody is not an error. False means
  // the card could not be read, or the document is malformed -- and the roster
  // is then empty, which refuses every incoming position rather than admitting
  // everybody.
  static bool load(GroupRoster& roster, size_t& skipped);

  static bool save(const GroupRoster& roster);
};

// The black box: every accepted position, ours and theirs, append-only.
//
// **One file per day, named for the day** (`bb-20260916.csv`). Rotation deletes
// whole days rather than trimming lines, so a file a laptop is reading never
// changes under it, and the oldest evidence is the first to go.
//
// A device with no clock cannot name the day, so it writes `bb-noclock.csv` --
// which rotation never deletes, because a file it cannot date is a file it
// cannot judge. Every X4 is such a device until the phone hands it a time.
class GroupBlackBox {
 public:
  static constexpr const char* kDir = "/trailink/group";

  // Appends one row and flushes, creating the day file with its header if it is
  // not there yet. False on any card failure -- the caller must then **not**
  // move the marker, or the panel would show a position the history never heard
  // of (the same log-first rule MapPins lives under).
  static bool append(const GroupRecord& rec);

  // Rebuilds the last known position of every roster member. Newest evidence
  // wins: dated files newest first, `bb-noclock.csv` last, and inside one file
  // the last row for a member. Returns false only when the directory could not
  // be read.
  //
  // `bootId` is this run's identity (MapGroup::bootId()). A row carrying it was
  // written by *this* run, so its uptime is comparable with the current one and
  // the position stays dateable across a re-entry into the map; every other row
  // is honestly undateable and draws as stale with `?`.
  static bool replayLast(const GroupRoster& roster, GroupStore& store, uint32_t bootId);

  // Deletes dated files older than `keepDays` days before `nowUtc`. Does nothing
  // when the device has no clock (`nowUtc` 0): a day it cannot name is a day it
  // must not delete.
  static void rotate(uint32_t nowUtc, uint32_t keepDays);

  // Newest-first paging over the newest file, for `group log`. `maxCount` 0 asks
  // for the total only and visits nothing.
  //
  // **The newest file, not the whole history**: a page that walked every kept
  // day would hold the activity's loop for seconds on a card full of rides, and
  // the question `group log` answers ("where is everyone now") is answered by the
  // newest one. Older days are read off the card on a laptop.
  static uint32_t page(uint32_t offset, uint32_t maxCount, IGroupLogVisitor& visitor);

  // Cap on the offsets one page() call remembers, same reason as PinLog's.
  static constexpr uint32_t kMaxPageEntries = 10;

  // Longest file name this writes, terminator included: `bb-20260916.csv`.
  static constexpr size_t kNameBytes = 20;

  // How many names one directory listing asks for. A file per riding day, so
  // this is years of them -- and it is a bound rather than an expectation,
  // because the listing allocates.
  static constexpr int kMaxDayFiles = 64;
};

#endif  // ENABLE_GROUP_MARKERS
