#include "LoraEventLog.h"

#if defined(ENABLE_LORA_MESH) && ENABLE_LORA_MESH

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "../DayFile.h"

namespace {

constexpr const char* kLogTag = "LORALOG";
constexpr uint32_t kSecondsPerDay = 86400u;
// Same look-back the black box uses: rotation walks a bounded number of past
// days rather than listing the directory, so a card that was away for a month
// still gets its old files removed.
constexpr uint32_t kRotateLookBackDays = 60;

bool pathForUtc(uint32_t utc, char* out, size_t outBytes) {
  char name[LoraEventLog::kNameBytes];
  if (!dayFileName("ev", utc, name, sizeof(name))) return false;
  const int written = snprintf(out, outBytes, "%s/%s", LoraEventLog::kDir, name);
  return written > 0 && static_cast<size_t>(written) < outBytes;
}

}  // namespace

bool LoraEventLog::append(const LoraLogEvent& ev, uint32_t utc, uint32_t bootId) {
  char line[kLoraLogLineMax + 2];
  const size_t len = encodeLoraLogEvent(ev, utc, bootId, line, kLoraLogLineMax + 1);
  if (len == 0) {
    LOG_ERR(kLogTag, "refusing to append an unencodable row (kind %u)", static_cast<unsigned>(ev.kind));
    return false;
  }
  line[len] = '\n';
  line[len + 1] = '\0';

  if (!Storage.ready()) return false;  // no card; the radio carries on regardless

  char path[64];
  if (!pathForUtc(utc, path, sizeof(path))) return false;

  // O_CREAT does not create the parent, and a card that has never had tiles
  // pushed to it has no `/trailink` at all. `PresenceMesh` already makes this
  // directory for the identity file, but only once a key has been written --
  // a card swapped in afterwards would otherwise swallow every row.
  const bool fresh = !Storage.exists(path);
  if (fresh) Storage.ensureDirectoryExists(kDir);

  HalFile file = Storage.open(path, O_WRITE | O_CREAT | O_APPEND);
  if (!file.isOpen()) {
    LOG_ERR(kLogTag, "cannot open %s", path);
    return false;
  }
  // Header with the first row, not at boot: a day the radio never ran has no
  // file, which is what makes the file list a list of days that happened.
  if (fresh) {
    char header[96];
    const int headerLen = snprintf(header, sizeof(header), "%s\n", kLoraLogCsvHeader);
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

void LoraEventLog::rotate(uint32_t nowUtc, uint32_t keepDays) {
  if (nowUtc == 0 || keepDays == 0) return;  // a day it cannot name is a day it must not delete
  if (!Storage.ready()) return;

  const uint32_t today = dayNumberFromUtc(nowUtc);
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

#endif  // ENABLE_LORA_MESH
