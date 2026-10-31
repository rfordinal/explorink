#include "GroupFiles.h"

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr const char* kLogTag = "GROUPFILE";

// Read chunk. Small on purpose: this runs on the activity task's stack next to a
// line buffer, and the Resource Protocol caps a frame's locals at 256 bytes
// (CLAUDE.md). Same number PinLog uses, for the same reason.
constexpr size_t kChunkBytes = 64;

// How far past the keep window one rotation pass looks for files to delete. A
// device that was without a clock for a month writes dated files again the
// moment the phone hands it a time, and the days it skipped would otherwise sit
// there forever. Bounded so a pass is 60 exists() calls and not a directory
// walk of a card full of tiles.
constexpr uint32_t kRotateLookBackDays = 60;

constexpr uint32_t kSecondsPerDay = 86400u;

bool pathForName(const char* name, char* out, size_t outBytes) {
  const int written = snprintf(out, outBytes, "%s/%s", GroupBlackBox::kDir, name);
  return written > 0 && static_cast<size_t>(written) < outBytes;
}

// The file a given day's rows live in. `utc` 0 is the no-clock file.
bool pathForUtc(uint32_t utc, char* out, size_t outBytes) {
  char name[GroupBlackBox::kNameBytes];
  if (!groupDayFileName(utc, name, sizeof(name))) return false;
  return pathForName(name, out, outBytes);
}

// One complete line and where it starts in the file. Same shape as PinLog's, and
// deliberately not shared with it: that one is bound to kPinLineMax and to the
// pins path, and a shared version would have to be told both.
using LineVisit = void (*)(void* ctx, std::string_view line, uint32_t byteOffset);

bool streamLines(const char* path, LineVisit fn, void* ctx) {
  HalFile file;
  if (!Storage.openFileForRead(kLogTag, path, file)) return false;

  char line[kGroupLineMax + 1];
  size_t len = 0;
  bool discarding = false;
  uint32_t filePos = 0;
  uint32_t lineStart = 0;

  char chunk[kChunkBytes];
  int read = 0;
  while ((read = file.read(chunk, sizeof(chunk))) > 0) {
    for (int i = 0; i < read; ++i) {
      const char c = chunk[i];
      ++filePos;
      if (c == '\n' || c == '\r') {
        if (!discarding && len > 0) {
          line[len] = '\0';
          fn(ctx, std::string_view(line, len), lineStart);
        }
        discarding = false;
        len = 0;
        lineStart = filePos;
        continue;
      }
      if (discarding) continue;
      if (len >= kGroupLineMax) {
        discarding = true;
        len = 0;
        continue;
      }
      line[len++] = c;
    }
  }
  // Whatever is left has no terminator: a torn write, discarded.
  return true;
}

// The day files on the card, newest first, with `bb-noclock.csv` last.
//
// **A dated row beats an undated one**, always. The no-clock file holds what was
// heard while the device did not know the time, which cannot be ordered against
// anything -- so it is the last resort, never the first answer.
//
// Walked by name rather than by the clock, because the clock is exactly what a
// device may not have at boot: an X4 has no RTC and learns the time from the
// phone, and the dated files a previous run wrote still have to be found.
bool isDayFileName(const String& name) {
  const size_t len = name.length();
  return len > 7 && strncmp(name.c_str(), "bb-", 3) == 0 && strcmp(name.c_str() + len - 4, ".csv") == 0 &&
         strcmp(name.c_str(), "bb-noclock.csv") != 0;
}

// The largest dated name strictly below `below`, or empty when there is none.
// An empty `below` means "no bound": the newest file.
//
// O(files^2) name compares over a directory that holds one file per riding day.
// It runs once at boot and holds one name at a time, which is the trade -- the
// alternative is an array of names on a stack frame the Resource Protocol caps
// at 256 bytes (CLAUDE.md).
String nextDayFileBelow(const std::vector<String>& names, const String& below) {
  String best;
  for (const String& name : names) {
    if (!isDayFileName(name)) continue;
    if (below.length() != 0 && strcmp(name.c_str(), below.c_str()) >= 0) continue;
    if (best.length() == 0 || strcmp(name.c_str(), best.c_str()) > 0) best = name;
  }
  return best;
}

struct CountCtx {
  uint32_t valid = 0;
};

void countLine(void* ctx, std::string_view line, uint32_t) {
  GroupRecord rec;
  if (!decodeGroupRecord(line, rec)) return;
  static_cast<CountCtx*>(ctx)->valid++;
}

struct WindowCtx {
  uint32_t index = 0;
  uint32_t first = 0;
  uint32_t count = 0;
  uint32_t found = 0;
  uint32_t offsets[GroupBlackBox::kMaxPageEntries] = {};
};

void windowLine(void* ctx, std::string_view line, uint32_t byteOffset) {
  auto* w = static_cast<WindowCtx*>(ctx);
  GroupRecord rec;
  if (!decodeGroupRecord(line, rec)) return;
  const uint32_t index = w->index++;
  if (index < w->first) return;
  if (index >= w->first + w->count) return;
  if (w->found < GroupBlackBox::kMaxPageEntries) w->offsets[w->found++] = byteOffset;
}

bool readRecordAt(const char* path, uint32_t byteOffset, GroupRecord& out) {
  HalFile file;
  if (!Storage.openFileForRead(kLogTag, path, file)) return false;
  if (!file.seek(byteOffset)) return false;

  char line[kGroupLineMax + 1];
  const int read = file.read(line, kGroupLineMax);
  if (read <= 0) return false;
  size_t len = 0;
  while (len < static_cast<size_t>(read) && line[len] != '\n' && line[len] != '\r') ++len;
  line[len] = '\0';
  return decodeGroupRecord(std::string_view(line, len), out);
}

// One file's worth of "the last row each member wrote". Filled in file order, so
// a later row simply overwrites an earlier one.
struct ReplayCtx {
  const GroupRoster* roster = nullptr;
  GroupStore* store = nullptr;
  uint32_t bootId = 0;
  bool filled[GroupRoster::kSlotCount] = {};  // filled by an already-read, newer file
  bool inThisFile[GroupRoster::kSlotCount] = {};
};

void replayLine(void* ctx, std::string_view line, uint32_t) {
  auto* r = static_cast<ReplayCtx*>(ctx);
  GroupRecord rec;
  if (!decodeGroupRecord(line, rec)) return;
  if (std::string_view(rec.who) == kGroupSelfWho) return;  // the rider's own trace, not a marker

  // By id first: a roster edit between rides can move an acronym to a different
  // person, and then the acronym in an old row points at somebody who was never
  // there. The acronym is the fallback, for rows written before the id column.
  size_t slot = rec.id[0] != '\0' ? r->roster->findId(rec.id) : GroupRoster::kSlotCount;
  if (slot >= GroupRoster::kSlotCount) slot = r->roster->findAcr(rec.who);
  // A row for somebody who is no longer in the roster stays in the file and is
  // simply not drawn: the black box is evidence and is never rewritten, but the
  // allowlist decides what reaches the panel.
  if (slot >= GroupRoster::kSlotCount) return;
  if (r->filled[slot]) return;  // a newer file already answered for this member

  GroupFix fix;
  fix.present = true;
  fix.latE7 = rec.latE7;
  fix.lonE7 = rec.lonE7;
  fix.utc = rec.utc;
  fix.recvUtc = rec.recvUtc;
  // A row from *this* run keeps its uptime, and with it its age: re-entering the
  // map must not turn a position heard a minute ago into an undateable one
  // (seen on an X4 Pro, 2026-09-16). A row from any other run carries an uptime
  // measured against a clock that no longer exists, so it stays undateable.
  const bool sameRun = rec.boot != 0 && rec.boot == r->bootId;
  fix.recvUptimeMs = sameRun ? rec.uptimeMs : 0;
  fix.fromLog = !sameRun;
  fix.heading = rec.heading;
  fix.hasHeading = rec.hasHeading;
  fix.speedKmh = rec.speedKmh;
  fix.hasSpeed = rec.hasSpeed;
  fix.source = rec.source;
  r->store->set(slot, fix);
  r->inThisFile[slot] = true;
}

}  // namespace

void groupMigrateLegacyDir() {
  // **Latched only once there is nothing left to do.** The first version set
  // this before attempting the rename, so a single failure stranded the card:
  // nothing retried, the black box then created an empty /explorink/group, and
  // from the next boot the both-exist branch refused forever while the
  // rider's hand-built roster sat unreadable under the old name.
  static bool done = false;
  if (done) return;
  if (!Storage.ready()) return;

  constexpr const char* kLegacyDir = "/explorink/team";
  constexpr const char* kLegacyRoster = "/explorink/team/members.json";

  const bool legacyExists = Storage.exists(kLegacyDir);
  const bool newExists = Storage.exists(GroupRosterFile::kDir);
  const GroupMigrateAction action = groupMigrateDecide(legacyExists, newExists, Storage.exists(kLegacyRoster),
                                                       Storage.exists(GroupRosterFile::kPath));

  switch (action) {
    case GroupMigrateAction::Nothing:
      done = true;
      return;

    case GroupMigrateAction::MoveDirectory:
      if (Storage.rename(kLegacyDir, GroupRosterFile::kDir)) {
        LOG_INF(kLogTag, "moved %s to %s", kLegacyDir, GroupRosterFile::kDir);
        done = true;
      } else {
        // Not latched: the next entry point tries again, and a half-finished
        // rename lands in RescueRoster above rather than in Refuse.
        LOG_ERR(kLogTag, "could not move %s to %s -- will retry; nothing was deleted", kLegacyDir,
                GroupRosterFile::kDir);
      }
      return;

    case GroupMigrateAction::RescueRoster:
      // The roster is the irreplaceable half -- built by hand, member by
      // member. The black box is history and stays where it is; it is named
      // bb-YYYYMMDD.csv and a person can move it with a card reader.
      if (Storage.rename(kLegacyRoster, GroupRosterFile::kPath)) {
        LOG_INF(kLogTag, "rescued the roster from %s; black-box files remain in %s", kLegacyRoster, kLegacyDir);
        done = true;
      } else {
        LOG_ERR(kLogTag, "could not rescue %s -- will retry; nothing was deleted", kLegacyRoster);
      }
      return;

    case GroupMigrateAction::Refuse:
      // Both hold a roster. Nothing here can tell which one the rider means,
      // so this picks neither and says so. The new path is what gets read.
      LOG_ERR(kLogTag, "%s and %s both hold a roster -- using %s, leaving both files alone", kLegacyDir,
              GroupRosterFile::kDir, GroupRosterFile::kPath);
      done = true;
      return;
  }
}

bool GroupRosterFile::load(GroupRoster& roster, size_t& skipped) {
  roster.clear();
  skipped = 0;

  if (!Storage.ready()) {
    LOG_ERR(kLogTag, "no card -- no roster, so every incoming position is refused");
    return false;
  }
  // Before the first look at the new path, never after: a load that ran first
  // would report an empty roster and the rider would see the group gone.
  groupMigrateLegacyDir();
  if (!Storage.exists(kPath)) {
    LOG_INF(kLogTag, "no %s yet -- nobody is approved", kPath);
    return true;
  }

  char buf[GroupRoster::kJsonBytes];
  const size_t read = Storage.readFileToBuffer(kPath, buf, sizeof(buf));
  if (read == 0) {
    LOG_ERR(kLogTag, "cannot read %s", kPath);
    return false;
  }
  if (read >= sizeof(buf) - 1) {
    // Truncated at the buffer is a half-read allowlist, which would silently
    // drop the members past the cut.
    LOG_ERR(kLogTag, "%s is larger than %u bytes -- refusing a partial roster", kPath,
            static_cast<unsigned>(sizeof(buf)));
    return false;
  }
  if (!roster.parseJson(std::string_view(buf, read), skipped)) {
    LOG_ERR(kLogTag, "%s is malformed or a version this build does not know", kPath);
    return false;
  }
  LOG_INF(kLogTag, "roster: %u member(s), %u row(s) skipped", static_cast<unsigned>(roster.count()),
          static_cast<unsigned>(skipped));
  return true;
}

bool GroupRosterFile::save(const GroupRoster& roster) {
  if (!Storage.ready()) {
    LOG_ERR(kLogTag, "no card -- roster not saved");
    return false;
  }
  char buf[GroupRoster::kJsonBytes];
  const size_t len = roster.writeJson(buf, sizeof(buf));
  if (len == 0) {
    LOG_ERR(kLogTag, "roster does not fit %u bytes", static_cast<unsigned>(sizeof(buf)));
    return false;
  }
  Storage.ensureDirectoryExists(kDir);
  return Storage.writeFile(kPath, String(buf));
}

bool GroupBlackBox::append(const GroupRecord& rec) {
  // The other entry point. Today every caller has already loaded the roster,
  // so this cannot be the first to run -- it is here because append() is the
  // one function that *creates* the directory (ensureDirectoryExists below),
  // and creating it behind a pending migration is what turns a retryable
  // half-move into two directories that both look real.
  groupMigrateLegacyDir();
  char line[kGroupLineMax + 2];
  const size_t len = encodeGroupRecord(rec, line, kGroupLineMax + 1);
  if (len == 0) {
    LOG_ERR(kLogTag, "refusing to append an unencodable row (who '%s')", rec.who);
    return false;
  }
  line[len] = '\n';
  line[len + 1] = '\0';

  if (!Storage.ready()) {
    LOG_ERR(kLogTag, "no card -- position not recorded");
    return false;
  }

  char path[64];
  if (!pathForUtc(rec.utc, path, sizeof(path))) return false;

  // O_CREAT does not create the parent, and a card that has never had tiles
  // pushed to it has no /explorink at all (same trap as PowerLog and PinLog).
  const bool fresh = !Storage.exists(path);
  if (fresh) Storage.ensureDirectoryExists(kDir);

  HalFile file = Storage.open(path, O_WRITE | O_CREAT | O_APPEND);
  if (!file.isOpen()) {
    LOG_ERR(kLogTag, "cannot open %s", path);
    return false;
  }
  // The header goes in with the first row, not at boot: a day the device wrote
  // nothing on has no file at all, which is what makes the file list a list of
  // days that actually happened.
  if (fresh) {
    char header[96];
    const int headerLen = snprintf(header, sizeof(header), "%s\n", kGroupCsvHeader);
    if (headerLen > 0) file.write(header, static_cast<size_t>(headerLen));
  }
  const size_t written = file.write(line, len + 1);
  file.flush();
  if (written != len + 1) {
    LOG_ERR(kLogTag, "short write: %u of %u bytes", static_cast<unsigned>(written), static_cast<unsigned>(len + 1));
    return false;
  }
  return true;
}

bool GroupBlackBox::replayLast(const GroupRoster& roster, GroupStore& store, uint32_t bootId) {
  store.clear();
  if (!Storage.ready()) {
    LOG_ERR(kLogTag, "no card -- nobody's last position restored");
    return false;
  }

  const std::vector<String> names = Storage.listFiles(kDir, kMaxDayFiles);
  ReplayCtx ctx;
  ctx.roster = &roster;
  ctx.store = &store;
  ctx.bootId = bootId;

  char path[64];
  String current;
  size_t filesRead = 0;
  while (true) {
    current = nextDayFileBelow(names, current);
    if (current.length() == 0) break;
    if (!pathForName(current.c_str(), path, sizeof(path))) break;
    for (bool& seen : ctx.inThisFile) seen = false;
    if (streamLines(path, &replayLine, &ctx)) ++filesRead;
    // A member the newest file answered for is settled: an older file's row for
    // them is an older position and must not overwrite it.
    for (size_t slot = 0; slot < GroupRoster::kSlotCount; ++slot) {
      if (ctx.inThisFile[slot]) ctx.filled[slot] = true;
    }
    // Every member answered for: the older files hold older positions and there
    // is nothing left for them to say. This is the difference between reading
    // one file and reading a fortnight of them on the way into the map screen.
    bool everyone = true;
    for (size_t slot = 0; slot < GroupRoster::kSlotCount; ++slot) {
      if (roster.at(slot).present && !ctx.filled[slot]) everyone = false;
    }
    if (everyone) break;
  }

  // Last, and only for the members no dated file mentioned.
  if (pathForName("bb-noclock.csv", path, sizeof(path)) && Storage.exists(path)) {
    for (bool& seen : ctx.inThisFile) seen = false;
    if (streamLines(path, &replayLine, &ctx)) ++filesRead;
  }

  LOG_INF(kLogTag, "black box: %u file(s) read, %u member position(s) restored", static_cast<unsigned>(filesRead),
          static_cast<unsigned>(store.presentCount()));
  return true;
}

void GroupBlackBox::rotate(uint32_t nowUtc, uint32_t keepDays) {
  if (nowUtc == 0 || keepDays == 0) return;  // a day it cannot name is a day it must not delete
  if (!Storage.ready()) return;

  const uint32_t today = groupDayNumber(nowUtc);
  char path[64];
  for (uint32_t back = keepDays; back <= keepDays + kRotateLookBackDays; ++back) {
    if (back > today) break;
    if (!pathForUtc((today - back) * kSecondsPerDay, path, sizeof(path))) continue;
    if (!Storage.exists(path)) continue;
    if (Storage.remove(path)) {
      LOG_INF(kLogTag, "rotated out %s (older than %u day(s))", path, static_cast<unsigned>(keepDays));
    } else {
      LOG_ERR(kLogTag, "cannot delete %s", path);
    }
  }
}

uint32_t GroupBlackBox::page(uint32_t offset, uint32_t maxCount, IGroupLogVisitor& visitor) {
  if (!Storage.ready()) return 0;

  const std::vector<String> names = Storage.listFiles(kDir, kMaxDayFiles);
  String newest = nextDayFileBelow(names, String());
  if (newest.length() == 0) newest = "bb-noclock.csv";

  char path[64];
  if (!pathForName(newest.c_str(), path, sizeof(path))) return 0;
  if (!Storage.exists(path)) return 0;

  CountCtx counted;
  if (!streamLines(path, &countLine, &counted)) return 0;
  const uint32_t total = counted.valid;
  if (offset >= total) return total;

  uint32_t count = maxCount < kMaxPageEntries ? maxCount : kMaxPageEntries;
  const uint32_t remaining = total - offset;
  if (count > remaining) count = remaining;
  if (count == 0) return total;

  WindowCtx window;
  window.first = total - offset - count;
  window.count = count;
  if (!streamLines(path, &windowLine, &window)) return total;

  // Reversed here rather than in the scan: the file only reads forwards, and the
  // offsets are 4 bytes each against a row's ~60.
  for (uint32_t i = window.found; i > 0; --i) {
    GroupRecord rec;
    if (readRecordAt(path, window.offsets[i - 1], rec)) visitor.onGroupLogRecord(rec);
  }
  return total;
}

#endif  // ENABLE_GROUP_MARKERS
