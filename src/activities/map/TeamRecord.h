#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "TeamFix.h"
#include "TeamMembers.h"

// One line of the black box, and the day file it belongs in.
//
// Pure: no Arduino, no HAL, no SD. TeamBlackBox does the card, this does the
// bytes, so every format rule is host-testable (test/team/).
//
// CSV, one row per accepted position, header written when a file is created:
//
//   utc,uptime_ms,who,lat,lon,heading,speed_kmh,src
//   1789430400,81234,RF,48.1486000,17.1077000,4,62,lora
//
// CSV rather than the pins log's `v1|...|crc32` framing, deliberately. This file
// is read by a person -- on a laptop, possibly by somebody looking for a rider
// -- and /trailink/power.csv and /trailink/gnss.csv already set that precedent.
// There is no CRC for the same reason: a torn last line is visibly torn, and a
// checksum that makes a rescuer's spreadsheet refuse a row helps nobody.
//
// | field     | notes                                                        |
// |-----------|--------------------------------------------------------------|
// | utc       | unix seconds, 0 = the sender had no clock                    |
// | uptime_ms | our uptime at receipt; orders rows inside a run with no clock |
// | who       | member acronym, or `ME` for the rider's own position          |
// | lat, lon  | decimal degrees, 7 places, integer-formatted (no FPU)         |
// | heading   | 0-15 sixteenths of a turn, empty when not sent                |
// | speed_kmh | empty when not sent                                           |
// | src       | cmd / ble / lora / gnss / unknown -- how it reached us        |

// `ME` is the rider's own row, so it can never be a member's acronym -- two
// people under one name in a search log is the failure that reserves it.
inline constexpr const char* kTeamSelfWho = "ME";

struct TeamRecord {
  uint32_t utc = 0;
  uint32_t uptimeMs = 0;
  char who[kTeamAcrBytes] = {};
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  uint8_t heading = 0;
  bool hasHeading = false;
  uint16_t speedKmh = 0;
  bool hasSpeed = false;
  TeamFixSource source = TeamFixSource::Unknown;
};

// Every field at its type's widest plus the separators, rounded up. Sized for
// the type, not for today's data (same rule as kPinLineMax).
inline constexpr size_t kTeamLineMax = 96;

inline constexpr const char* kTeamCsvHeader = "utc,uptime_ms,who,lat,lon,heading,speed_kmh,src";

const char* teamSourceText(TeamFixSource source);
bool teamSourceFromText(std::string_view text, TeamFixSource& out);

// Writes the row into `buf`, no trailing newline (TeamBlackBox adds it). Returns
// the length written, or 0 when the buffer is too small or `who` is unstorable.
size_t encodeTeamRecord(const TeamRecord& rec, char* buf, size_t bufLen);

// Parses one row, terminator already stripped, header line included (it is
// refused like any other malformed row). False means "skip this line and keep
// reading" -- never "the file is broken".
bool decodeTeamRecord(std::string_view line, TeamRecord& out);

// The file a fix belongs in. Rotation is by day, so the name carries the date:
// `bb-20260916.csv`. A device with no clock writes `bb-noclock.csv`, because a
// day it cannot name is still a day whose rows must not be lost.
bool teamDayFileName(uint32_t utc, char* out, size_t outBytes);

// Days since 1970-01-01 UTC, which is what rotation compares. 0 for utc 0.
uint32_t teamDayNumber(uint32_t utc);

#endif  // ENABLE_TEAM_MARKERS
