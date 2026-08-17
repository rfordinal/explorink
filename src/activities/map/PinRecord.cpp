#include "PinRecord.h"

#include <cstdio>
#include <cstring>

#include "MapCrc32.h"

namespace {

constexpr size_t kFieldCount = 11;
constexpr std::string_view kVersion = "v1";

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Splits on '|' into exactly kFieldCount views. False on any other count, so a
// line with a lost or an extra separator is skipped instead of half-read.
bool splitFields(std::string_view line, std::string_view* out) {
  size_t count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '|') {
      if (count >= kFieldCount) return false;
      out[count++] = line.substr(start, i - start);
      start = i + 1;
    }
  }
  return count == kFieldCount;
}

bool parseUint32(std::string_view s, uint32_t& out) {
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

bool parseInt32(std::string_view s, int32_t& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool negative = false;
  if (s[0] == '+' || s[0] == '-') {
    negative = (s[0] == '-');
    i = 1;
  }
  if (i >= s.size() || s.size() - i > 10) return false;
  int64_t value = 0;
  for (; i < s.size(); ++i) {
    if (!isDigit(s[i])) return false;
    value = value * 10 + (s[i] - '0');
    if (value > 2147483648ll) return false;
  }
  if (negative) {
    value = -value;
    if (value < -2147483648ll) return false;
  } else if (value > 2147483647ll) {
    return false;
  }
  out = static_cast<int32_t>(value);
  return true;
}

bool parseHex32(std::string_view s, uint32_t& out) {
  if (s.size() != 8) return false;
  uint32_t value = 0;
  for (const char c : s) {
    uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint32_t>(c - 'a') + 10;
    } else {
      return false;  // uppercase refused: this end writes lowercase, so a hex
                     // digit in the other case is a sign the line was rewritten
    }
    value = (value << 4) | digit;
  }
  out = value;
  return true;
}

bool parseOp(std::string_view s, PinOp& out) {
  if (s == "add") {
    out = PinOp::Add;
  } else if (s == "rep") {
    out = PinOp::Replace;
  } else if (s == "del") {
    out = PinOp::Delete;
  } else if (s == "res") {
    out = PinOp::Restore;
  } else {
    return false;
  }
  return true;
}

}  // namespace

bool setPinRecordKey(PinRecord& out, std::string_view key) {
  if (!isValidPinKey(key)) return false;
  memcpy(out.key, key.data(), key.size());
  out.key[key.size()] = '\0';
  return true;
}

const char* pinOpText(PinOp op) {
  switch (op) {
    case PinOp::Add:
      return "add";
    case PinOp::Replace:
      return "rep";
    case PinOp::Delete:
      return "del";
    case PinOp::Restore:
      return "res";
  }
  return "add";
}

size_t encodePinRecord(const PinRecord& rec, char* buf, size_t bufLen) {
  if (buf == nullptr || bufLen < kPinLineMax + 1) return 0;
  // The key is validated on the way in as well as on the way out: a record
  // assembled by hand with a '|' in its key would produce a line that decodes as
  // a different record, which is the one corruption a CRC cannot catch.
  if (!isValidPinKey(std::string_view(rec.key))) return 0;

  char lat[12] = {};
  char lon[12] = {};
  if (rec.hasPos) {
    snprintf(lat, sizeof(lat), "%ld", static_cast<long>(rec.latE7));
    snprintf(lon, sizeof(lon), "%ld", static_cast<long>(rec.lonE7));
  }

  // Everything but the checksum, which is computed over exactly these bytes.
  const int bodyLen =
      snprintf(buf, bufLen, "v1|%lu|%lu|%lu|%s|%s|%lu|%s|%s|", static_cast<unsigned long>(rec.seq),
               static_cast<unsigned long>(rec.utc), static_cast<unsigned long>(rec.uptimeMs), pinOpText(rec.op),
               rec.key, static_cast<unsigned long>(rec.id), lat, lon);
  if (bodyLen <= 0 || static_cast<size_t>(bodyLen) >= bufLen) return 0;

  const uint32_t crc = MapCrc32::of(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(bodyLen));
  const int tailLen = snprintf(buf + bodyLen, bufLen - static_cast<size_t>(bodyLen), "|%08lx",
                               static_cast<unsigned long>(crc));
  if (tailLen <= 0 || static_cast<size_t>(bodyLen + tailLen) >= bufLen) return 0;
  return static_cast<size_t>(bodyLen + tailLen);
}

bool decodePinRecord(std::string_view line, PinRecord& out) {
  if (line.size() > kPinLineMax) return false;

  std::string_view field[kFieldCount];
  if (!splitFields(line, field)) return false;
  if (field[0] != kVersion) return false;  // unknown version: skipped, keep reading

  // The CRC covers every byte up to, and not including, the '|' before it.
  const size_t crcLen = field[kFieldCount - 1].size();
  if (crcLen + 1 > line.size()) return false;
  const size_t bodyLen = line.size() - crcLen - 1;
  uint32_t stated = 0;
  if (!parseHex32(field[kFieldCount - 1], stated)) return false;
  if (MapCrc32::of(reinterpret_cast<const uint8_t*>(line.data()), bodyLen) != stated) return false;

  PinRecord rec;
  if (!parseUint32(field[1], rec.seq)) return false;
  if (!parseUint32(field[2], rec.utc)) return false;
  if (!parseUint32(field[3], rec.uptimeMs)) return false;
  if (!parseOp(field[4], rec.op)) return false;
  if (!setPinRecordKey(rec, field[5])) return false;
  if (!parseUint32(field[6], rec.id)) return false;

  const bool latEmpty = field[7].empty();
  const bool lonEmpty = field[8].empty();
  if (latEmpty != lonEmpty) return false;  // half a coordinate is not a coordinate
  if (!latEmpty) {
    if (!parseInt32(field[7], rec.latE7)) return false;
    if (!parseInt32(field[8], rec.lonE7)) return false;
    rec.hasPos = true;
  }
  // A record that places a pin must carry where. A del must not.
  if (rec.op != PinOp::Delete && !rec.hasPos) return false;

  // field[9] is `trip`, reserved. Anything in it is carried by the CRC and
  // ignored here -- a newer firmware's trip id must not make this build skip the
  // record.
  out = rec;
  return true;
}
