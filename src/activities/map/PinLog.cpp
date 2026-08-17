#include "PinLog.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {

constexpr const char* kLogTag = "PINLOG";

// Read chunk. Small on purpose: this runs on the activity task's stack next to a
// line buffer, and the Resource Protocol caps a frame's locals at 256 bytes
// (CLAUDE.md). The log is a few hundred lines at most, so the extra read calls
// cost far less than the stack would.
constexpr size_t kChunkBytes = 64;

// One complete line and where it starts in the file.
using LineVisit = void (*)(void* ctx, std::string_view line, uint32_t byteOffset);

// Streams kPath line by line. Over-long lines and an unterminated tail are
// dropped, for the reasons in PinLogScanner -- this is the same policy against a
// real file. False means the file could not be opened.
bool streamLines(LineVisit fn, void* ctx) {
  HalFile file;
  if (!Storage.openFileForRead(kLogTag, PinLog::kPath, file)) return false;

  char line[kPinLineMax + 1];
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
      if (len >= kPinLineMax) {
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

struct CountCtx {
  uint32_t valid = 0;
};

void countLine(void* ctx, std::string_view line, uint32_t) {
  PinRecord rec;
  if (!decodePinRecord(line, rec)) return;
  static_cast<CountCtx*>(ctx)->valid++;
}

struct WindowCtx {
  uint32_t index = 0;      // valid-record index, in file order
  uint32_t first = 0;      // first index wanted
  uint32_t count = 0;      // how many wanted
  uint32_t found = 0;      // offsets stored
  uint32_t offsets[PinLog::kMaxPageEntries] = {};
};

void windowLine(void* ctx, std::string_view line, uint32_t byteOffset) {
  auto* w = static_cast<WindowCtx*>(ctx);
  PinRecord rec;
  if (!decodePinRecord(line, rec)) return;
  const uint32_t index = w->index++;
  if (index < w->first) return;
  if (index >= w->first + w->count) return;
  if (w->found < PinLog::kMaxPageEntries) w->offsets[w->found++] = byteOffset;
}

// Reads back one line at a known byte offset. Used only for the handful of lines
// a page prints, so it reopens the file per line rather than holding a handle
// across a visitor callback that may itself write to the console.
bool readRecordAt(uint32_t byteOffset, PinRecord& out) {
  HalFile file;
  if (!Storage.openFileForRead(kLogTag, PinLog::kPath, file)) return false;
  if (!file.seek(byteOffset)) return false;

  char line[kPinLineMax + 1];
  const int read = file.read(line, kPinLineMax);
  if (read <= 0) return false;
  size_t len = 0;
  while (len < static_cast<size_t>(read) && line[len] != '\n' && line[len] != '\r') ++len;
  line[len] = '\0';
  return decodePinRecord(std::string_view(line, len), out);
}

}  // namespace

bool PinLog::append(const PinRecord& rec) {
  char line[kPinLineMax + 2];
  const size_t len = encodePinRecord(rec, line, kPinLineMax + 1);
  if (len == 0) {
    LOG_ERR(kLogTag, "refusing to append an unencodable record (key '%s')", rec.key);
    return false;
  }
  line[len] = '\n';
  line[len + 1] = '\0';

  if (!Storage.ready()) {
    LOG_ERR(kLogTag, "no card -- pin not recorded");
    return false;
  }
  // O_CREAT does not create the parent, and a card that has never had tiles
  // pushed to it has no /trailink at all (same trap as PowerLog).
  if (!Storage.exists(kPath)) Storage.ensureDirectoryExists(kDir);

  HalFile file = Storage.open(kPath, O_WRITE | O_CREAT | O_APPEND);
  if (!file.isOpen()) {
    LOG_ERR(kLogTag, "cannot open %s", kPath);
    return false;
  }
  const size_t written = file.write(line, len + 1);
  file.flush();
  if (written != len + 1) {
    LOG_ERR(kLogTag, "short write: %u of %u bytes", static_cast<unsigned>(written), static_cast<unsigned>(len + 1));
    return false;
  }
  return true;
}

bool PinLog::replay(PinStore& store, PinReplayStats& stats) {
  store.clear();
  stats = PinReplayStats{};

  if (!Storage.ready()) {
    LOG_ERR(kLogTag, "no card -- no pins restored");
    return false;
  }
  if (!Storage.exists(kPath)) {
    LOG_INF(kLogTag, "no %s yet -- starting with no pins", kPath);
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead(kLogTag, kPath, file)) return false;

  PinLogReplayer replayer(store);
  char chunk[kChunkBytes];
  int read = 0;
  while ((read = file.read(chunk, sizeof(chunk))) > 0) {
    replayer.feed(chunk, static_cast<size_t>(read));
  }
  replayer.finish();
  stats = replayer.stats();

  LOG_INF(kLogTag, "replayed %s: %lu applied, %lu skipped, %u active", kPath,
          static_cast<unsigned long>(stats.applied), static_cast<unsigned long>(stats.skipped),
          static_cast<unsigned>(store.presentCount()));
  return true;
}

uint32_t PinLog::page(uint32_t offset, uint32_t maxCount, IPinLogVisitor& visitor) {
  if (!Storage.ready() || !Storage.exists(kPath)) return 0;

  CountCtx counted;
  if (!streamLines(&countLine, &counted)) return 0;
  const uint32_t total = counted.valid;
  if (offset >= total) return total;

  uint32_t count = maxCount < kMaxPageEntries ? maxCount : kMaxPageEntries;
  const uint32_t remaining = total - offset;
  if (count > remaining) count = remaining;
  if (count == 0) return total;

  // Newest first: offset 0 is the last valid record in the file, so the window in
  // file order starts `offset + count` back from the end.
  WindowCtx window;
  window.first = total - offset - count;
  window.count = count;
  if (!streamLines(&windowLine, &window)) return total;

  // Reversed here rather than in the scan: the file only reads forwards, and the
  // offsets are 4 bytes each against a record's ~44.
  for (uint32_t i = window.found; i > 0; --i) {
    PinRecord rec;
    if (readRecordAt(window.offsets[i - 1], rec)) visitor.onPinLogRecord(rec);
  }
  return total;
}
