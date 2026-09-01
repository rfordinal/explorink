#include "GnssLog.h"

#ifdef ENABLE_GNSS_CMD

#include <Arduino.h>
#include <Gnss.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"

namespace {

constexpr const char* kLogTag = "GNSSLOG";

// The column order is the file format. Appending a column is fine; reordering
// one silently breaks every earlier row a script reads alongside it.
//
// No fix age column: the difference between two rows' uptime_ms says the same
// thing and cannot disagree with itself. `moving` and `heading` are the
// device's own decisions, next to the `speed` and `course` they were taken
// from. That pairing is the whole point: the
// question is not what the receiver said, it is whether MapGnssHeading was
// right to believe it.
constexpr const char* kHeader =
    "uptime_ms,utc,lat,lon,quality,sats_used,hdop,speed_kmh,course_deg,moving,heading,build\n";

// Rows wait here rather than going to the card one at a time. Sized so ten
// seconds of 1 Hz fixes fit with room to spare; a row is about 90 bytes.
constexpr size_t kBufferBytes = 1536;
char buffer[kBufferBytes];
size_t used = 0;
uint32_t nextFlushMs = 0;
bool disabled = false;
bool headerWrittenThisBoot = false;

// Writes the buffer out and empties it. Safe to call with nothing buffered.
void writeOut() {
  if (used == 0) return;
  if (disabled) return;
  if (!Storage.ready()) return;  // no card yet; keep buffering, try again later

  const bool fresh = !Storage.exists(GnssLog::kPath);
  // A card that has never had tiles pushed to it has no /trailink yet, and
  // O_CREAT does not create the parent.
  if (fresh) Storage.ensureDirectoryExists(GnssLog::kDir);
  HalFile file = Storage.open(GnssLog::kPath, O_WRITE | O_CREAT | O_APPEND);
  if (!file.isOpen()) {
    // One line, then stop trying, exactly as PowerLog does: a card that refuses
    // this file will refuse it every ten seconds for the rest of the ride, and
    // the log spam would be worse than the missing measurement.
    LOG_ERR(kLogTag, "cannot open %s -- gnss logging off for this boot", GnssLog::kPath);
    disabled = true;
    used = 0;
    return;
  }
  // Once per boot, not only on a fresh file: it makes each boot's rows
  // self-describing across a column change, and it is the run marker the
  // analysis wants anyway. A reader skips any line starting with "uptime_ms".
  if (fresh || !headerWrittenThisBoot) {
    file.print(kHeader);
    headerWrittenThisBoot = true;
  }
  file.write(reinterpret_cast<const uint8_t*>(buffer), used);
  // Explicit: a ride can end with a flat battery rather than a clean shutdown,
  // and an unflushed row is a row that was never measured.
  file.flush();
  used = 0;
}

}  // namespace

void GnssLog::record(const GnssFix& fix, bool moving, uint8_t headingStep) {
  if (SETTINGS.mapGnssLog == 0) return;
  if (disabled) return;

  char row[160];
  // %.6f for the position: about 0.11 m at the equator, well past what this
  // receiver resolves, and it keeps a row inside its budget. TRAILINK_VERSION
  // goes through %s and is never concatenated into the format string -- it
  // carries a branch name, and a '%' in one would make printf read an argument
  // that was never passed.
  const int n = snprintf(row, sizeof(row), "%lu,%lu,%.6f,%.6f,%u,%u,%.1f,%.1f,%.1f,%u,%u,%s\n",
                         static_cast<unsigned long>(millis()), static_cast<unsigned long>(fix.utc), fix.latitude,
                         fix.longitude, static_cast<unsigned>(fix.quality), static_cast<unsigned>(fix.satsUsed),
                         static_cast<double>(fix.hdop), static_cast<double>(fix.speedKmh),
                         static_cast<double>(fix.courseDegrees), moving ? 1u : 0u,
                         static_cast<unsigned>(headingStep), TRAILINK_VERSION);
  if (n <= 0) return;
  const size_t len = static_cast<size_t>(n) < sizeof(row) ? static_cast<size_t>(n) : sizeof(row) - 1;

  // A row that does not fit forces the flush early rather than being dropped.
  // Dropping is the one outcome that makes the file lie about the ride.
  if (used + len > kBufferBytes) writeOut();
  if (used + len > kBufferBytes) return;  // still no room: the card is not ready, and the buffer is full

  memcpy(buffer + used, row, len);
  used += len;

  const uint32_t now = millis();
  if (nextFlushMs == 0) nextFlushMs = now + kFlushIntervalMs;
  if (static_cast<int32_t>(now - nextFlushMs) >= 0) {
    writeOut();
    nextFlushMs = now + kFlushIntervalMs;
  }
}

void GnssLog::flush() {
  if (SETTINGS.mapGnssLog == 0) return;
  writeOut();
}

#else

// No receiver in this build, so no rows and no file. Defined rather than
// omitted so the call sites need no #ifdef of their own.
void GnssLog::record(const GnssFix&, bool, uint8_t) {}
void GnssLog::flush() {}

#endif  // ENABLE_GNSS_CMD
