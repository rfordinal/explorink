#include "TeamRecord.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kSecondsPerDay = 86400u;

// int32 scaled by 1e7 back to plain decimal degrees. Integer only -- the device
// has no FPU, and a printf("%f") on a soft-float double is both slow and a chunk
// of flash nothing else on this path needs (same helper as the console's).
void formatE7(int32_t value, char* buf, size_t bufLen) {
  const bool negative = value < 0;
  const uint32_t magnitude =
      negative ? static_cast<uint32_t>(-static_cast<int64_t>(value)) : static_cast<uint32_t>(value);
  snprintf(buf, bufLen, "%s%lu.%07lu", negative ? "-" : "", static_cast<unsigned long>(magnitude / 10000000u),
           static_cast<unsigned long>(magnitude % 10000000u));
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool parseUint(std::string_view s, uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  uint64_t value = 0;
  for (const char c : s) {
    if (!isDigit(c)) return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  if (value > 0xFFFFFFFFull) return false;
  out = static_cast<uint32_t>(value);
  return true;
}

// Decimal degrees back to int32 scaled by 1e7. Digits past the 7th are ignored
// rather than rounded, exactly like the console's `pos` -- one rule for a
// coordinate on this device, wherever it is written.
bool parseE7(std::string_view s, int32_t& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool negative = false;
  if (s[0] == '+' || s[0] == '-') {
    negative = (s[0] == '-');
    i = 1;
  }
  if (i >= s.size()) return false;

  int64_t whole = 0;
  size_t wholeDigits = 0;
  for (; i < s.size() && s[i] != '.'; ++i) {
    if (!isDigit(s[i]) || wholeDigits >= 3) return false;
    whole = whole * 10 + (s[i] - '0');
    ++wholeDigits;
  }
  if (wholeDigits == 0) return false;

  int64_t frac = 0;
  int fracDigits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    for (; i < s.size(); ++i) {
      if (!isDigit(s[i])) return false;
      if (fracDigits >= 7) continue;
      frac = frac * 10 + (s[i] - '0');
      ++fracDigits;
    }
  }
  for (; fracDigits < 7; ++fracDigits) frac *= 10;

  const int64_t value = whole * 10000000ll + frac;
  if (value > 1800000000ll) return false;
  out = static_cast<int32_t>(negative ? -value : value);
  return true;
}

// Next comma-separated field. Returns false when the line has run out, which is
// how a row with too few fields is refused rather than read short.
bool nextField(std::string_view& rest, std::string_view& field, bool last) {
  const size_t comma = rest.find(',');
  if (last) {
    if (comma != std::string_view::npos) return false;  // too many fields
    field = rest;
    rest = {};
    return true;
  }
  if (comma == std::string_view::npos) return false;
  field = rest.substr(0, comma);
  rest = rest.substr(comma + 1);
  return true;
}

// Civil date from days since the epoch (Howard Hinnant's algorithm). Integer
// only and no <ctime>: the device's own clock helpers are not available to a
// pure translation unit, and gmtime_r on this toolchain pulls in locale code.
void civilFromDays(uint32_t days, uint32_t& year, uint32_t& month, uint32_t& day) {
  int64_t z = static_cast<int64_t>(days) + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;                                     // [0, 146096]
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
  const int64_t mp = (5 * doy + 2) / 153;                                    // [0, 11]
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;                               // [1, 12]
  year = static_cast<uint32_t>(y + (m <= 2 ? 1 : 0));
  month = static_cast<uint32_t>(m);
  day = static_cast<uint32_t>(d);
}

}  // namespace

const char* teamSourceText(TeamFixSource source) {
  switch (source) {
    case TeamFixSource::Cmd:
      return "cmd";
    case TeamFixSource::Ble:
      return "ble";
    case TeamFixSource::Lora:
      return "lora";
    case TeamFixSource::Gnss:
      return "gnss";
    case TeamFixSource::Unknown:
      break;
  }
  return "unknown";
}

bool teamSourceFromText(std::string_view text, TeamFixSource& out) {
  if (text == "cmd") out = TeamFixSource::Cmd;
  else if (text == "ble") out = TeamFixSource::Ble;
  else if (text == "lora") out = TeamFixSource::Lora;
  else if (text == "gnss") out = TeamFixSource::Gnss;
  else if (text == "unknown") out = TeamFixSource::Unknown;
  else return false;
  return true;
}

size_t encodeTeamRecord(const TeamRecord& rec, char* buf, size_t bufLen) {
  if (buf == nullptr || bufLen == 0) return 0;
  const std::string_view who(rec.who);
  if (who != kTeamSelfWho && !isValidTeamAcr(who)) return 0;

  char lat[16];
  char lon[16];
  formatE7(rec.latE7, lat, sizeof(lat));
  formatE7(rec.lonE7, lon, sizeof(lon));

  char heading[4] = {};
  if (rec.hasHeading) snprintf(heading, sizeof(heading), "%u", static_cast<unsigned>(rec.heading));
  char speed[8] = {};
  if (rec.hasSpeed) snprintf(speed, sizeof(speed), "%u", static_cast<unsigned>(rec.speedKmh));

  const int written =
      snprintf(buf, bufLen, "%lu,%lu,%lu,%lu,%s,%s,%s,%s,%s,%s,%s", static_cast<unsigned long>(rec.utc),
               static_cast<unsigned long>(rec.recvUtc), static_cast<unsigned long>(rec.uptimeMs),
               static_cast<unsigned long>(rec.boot), rec.who, rec.id, lat, lon, heading, speed,
               teamSourceText(rec.source));
  if (written < 0 || static_cast<size_t>(written) >= bufLen) return 0;
  return static_cast<size_t>(written);
}

bool decodeTeamRecord(std::string_view line, TeamRecord& out) {
  if (line.empty() || line.size() > kTeamLineMax) return false;

  // Ten fields today, nine before `recv_utc`, eight before `boot` -- counted up
  // front rather than guessed at, because the field after the numbers is an
  // acronym and "is this a number" is not a format decision.
  size_t fields = 1;
  for (const char c : line) {
    if (c == ',') ++fields;
  }
  if (fields < 8 || fields > 11) return false;
  const bool hasBoot = fields >= 9;
  const bool hasRecvUtc = fields >= 10;
  const bool hasId = fields == 11;

  std::string_view rest = line;
  std::string_view utc, recvUtc, uptime, boot, who, id, lat, lon, heading, speed, src;
  if (!nextField(rest, utc, false)) return false;
  if (hasRecvUtc && !nextField(rest, recvUtc, false)) return false;
  if (!nextField(rest, uptime, false)) return false;
  if (hasBoot && !nextField(rest, boot, false)) return false;
  if (!nextField(rest, who, false)) return false;
  if (hasId && !nextField(rest, id, false)) return false;
  if (!nextField(rest, lat, false) || !nextField(rest, lon, false) ||
      !nextField(rest, heading, false) || !nextField(rest, speed, false) || !nextField(rest, src, true)) {
    return false;
  }

  TeamRecord rec;
  if (!parseUint(utc, rec.utc) || !parseUint(uptime, rec.uptimeMs)) return false;
  if (hasRecvUtc && !parseUint(recvUtc, rec.recvUtc)) return false;
  if (hasBoot && !parseUint(boot, rec.boot)) return false;
  if (who != kTeamSelfWho && !isValidTeamAcr(who)) return false;
  memcpy(rec.who, who.data(), who.size());
  rec.who[who.size()] = '\0';
  // Empty is legal: `ME` rows have no transport identity, and neither does a row
  // written before the column.
  if (!id.empty()) {
    if (!isValidTeamId(id)) return false;
    memcpy(rec.id, id.data(), id.size());
    rec.id[id.size()] = '\0';
  }
  if (!parseE7(lat, rec.latE7) || !parseE7(lon, rec.lonE7)) return false;
  if (!heading.empty()) {
    uint32_t value = 0;
    if (!parseUint(heading, value) || value > 15) return false;
    rec.heading = static_cast<uint8_t>(value);
    rec.hasHeading = true;
  }
  if (!speed.empty()) {
    uint32_t value = 0;
    if (!parseUint(speed, value) || value > 0xFFFFu) return false;
    rec.speedKmh = static_cast<uint16_t>(value);
    rec.hasSpeed = true;
  }
  if (!teamSourceFromText(src, rec.source)) return false;

  out = rec;
  return true;
}

uint32_t teamDayNumber(uint32_t utc) { return utc / kSecondsPerDay; }

bool teamDayFileName(uint32_t utc, char* out, size_t outBytes) {
  if (out == nullptr || outBytes < 20) return false;
  if (utc == 0) {
    snprintf(out, outBytes, "bb-noclock.csv");
    return true;
  }
  uint32_t year = 0;
  uint32_t month = 0;
  uint32_t day = 0;
  civilFromDays(teamDayNumber(utc), year, month, day);
  snprintf(out, outBytes, "bb-%04lu%02lu%02lu.csv", static_cast<unsigned long>(year), static_cast<unsigned long>(month),
           static_cast<unsigned long>(day));
  return true;
}

#endif  // ENABLE_TEAM_MARKERS
