#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstdint>

#include "TeamRecord.h"
#include "TeamSource.h"
#include "TeamRoster.h"
#include "TeamStore.h"

// The two files the group-markers layer owns, and the only code here that
// touches storage. Everything they need parsed lives in TeamRoster and
// TeamRecord, so the format rules stay host-testable with no card attached
// (test/team/).
//
// All access goes through HalStorage -- SdFat is not thread-safe (CLAUDE.md).
//
// The paths keep `/trailink`: the on-card layout is infrastructure not yet
// renamed (parent CLAUDE.md, Naming).

// The allowlist. One document, rewritten whole on every edit (TeamRoster.h says
// why there is no history here).
class TeamRosterFile {
 public:
  static constexpr const char* kDir = "/trailink/team";
  static constexpr const char* kPath = "/trailink/team/members.json";

  // A file that does not exist yet is a successful empty load, exactly like the
  // pins log: a rider who has never added anybody is not an error. False means
  // the card could not be read, or the document is malformed -- and the roster
  // is then empty, which refuses every incoming position rather than admitting
  // everybody.
  static bool load(TeamRoster& roster, size_t& skipped);

  static bool save(const TeamRoster& roster);
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
class TeamBlackBox {
 public:
  static constexpr const char* kDir = "/trailink/team";

  // Appends one row and flushes, creating the day file with its header if it is
  // not there yet. False on any card failure -- the caller must then **not**
  // move the marker, or the panel would show a position the history never heard
  // of (the same log-first rule MapPins lives under).
  static bool append(const TeamRecord& rec);

  // Rebuilds the last known position of every roster member. Newest evidence
  // wins: dated files newest first, `bb-noclock.csv` last, and inside one file
  // the last row for a member. Returns false only when the directory could not
  // be read.
  static bool replayLast(const TeamRoster& roster, TeamStore& store);

  // Deletes dated files older than `keepDays` days before `nowUtc`. Does nothing
  // when the device has no clock (`nowUtc` 0): a day it cannot name is a day it
  // must not delete.
  static void rotate(uint32_t nowUtc, uint32_t keepDays);

  // Newest-first paging over the newest file, for `team log`. `maxCount` 0 asks
  // for the total only and visits nothing.
  //
  // **The newest file, not the whole history**: a page that walked every kept
  // day would hold the activity's loop for seconds on a card full of rides, and
  // the question `team log` answers ("where is everyone now") is answered by the
  // newest one. Older days are read off the card on a laptop.
  static uint32_t page(uint32_t offset, uint32_t maxCount, ITeamLogVisitor& visitor);

  // Cap on the offsets one page() call remembers, same reason as PinLog's.
  static constexpr uint32_t kMaxPageEntries = 10;

  // Longest file name this writes, terminator included: `bb-20260916.csv`.
  static constexpr size_t kNameBytes = 20;

  // How many names one directory listing asks for. A file per riding day, so
  // this is years of them -- and it is a bound rather than an expectation,
  // because the listing allocates.
  static constexpr int kMaxDayFiles = 64;
};

#endif  // ENABLE_TEAM_MARKERS
