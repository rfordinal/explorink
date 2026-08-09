#include "MapCommandParser.h"

namespace {

// Longest legal line is `pos <lat> <lon> heading <n> speed <n> alt <n>` -- 9
// tokens. One spare so a 10th token is seen and rejected rather than
// truncated away.
constexpr size_t kMaxTokens = 10;

constexpr int32_t kLatMaxE7 = 900000000;
constexpr int32_t kLonMaxE7 = 1800000000;
constexpr uint32_t kMaxHeading = 15;
constexpr uint32_t kMaxZoom = 4;
constexpr uint32_t kMaxMarker = 4;
constexpr uint32_t kMaxSpeedKmh = 65535;
constexpr uint32_t kMaxMissingOffset = 65535;
// Dead Sea shore is -430m, Everest is 8849m -- generous margin either side
// for GPS altitude noise without letting garbage input through.
constexpr int32_t kMinAltitudeM = -1000;
constexpr int32_t kMaxAltitudeM = 9000;

struct Tokens {
  std::string_view t[kMaxTokens];
  size_t n = 0;
  bool overflow = false;
};

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

Tokens tokenize(std::string_view line) {
  Tokens out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && isSpace(line[i])) ++i;
    if (i >= line.size()) break;
    const size_t start = i;
    while (i < line.size() && !isSpace(line[i])) ++i;
    if (out.n >= kMaxTokens) {
      out.overflow = true;
      return out;
    }
    out.t[out.n++] = line.substr(start, i - start);
  }
  return out;
}

// Decimal degrees to int32 scaled by 1e7. No exponent notation, no
// leading/trailing junk, no locale. Digits past the 7th decimal place are
// ignored rather than rejected -- 1e-7 degrees is ~1 cm, so anything finer
// is noise from whatever produced the number.
bool parseDegrees(std::string_view s, int64_t& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool negative = false;
  if (s[0] == '+' || s[0] == '-') {
    negative = (s[0] == '-');
    i = 1;
  }

  int64_t whole = 0;
  size_t wholeDigits = 0;
  while (i < s.size() && isDigit(s[i])) {
    if (wholeDigits >= 10) return false;  // far past any coordinate; int64 stays safe
    whole = whole * 10 + (s[i] - '0');
    ++wholeDigits;
    ++i;
  }

  int64_t frac = 0;
  size_t fracDigits = 0;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && isDigit(s[i])) {
      if (fracDigits < 7) {
        frac = frac * 10 + (s[i] - '0');
        ++fracDigits;
      }
      ++i;
    }
  }

  if (i != s.size()) return false;                        // trailing garbage: "1.2.3", "4e5", "12x"
  if (wholeDigits == 0 && fracDigits == 0) return false;  // "", "-", "."

  for (size_t k = fracDigits; k < 7; ++k) frac *= 10;
  const int64_t value = whole * 10000000 + frac;
  out = negative ? -value : value;
  return true;
}

bool parseUint(std::string_view s, uint32_t& out) {
  if (s.empty()) return false;
  uint64_t value = 0;
  size_t digits = 0;
  for (const char c : s) {
    if (!isDigit(c)) return false;
    if (digits >= 10) return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
    ++digits;
  }
  if (value > 0xFFFFFFFFull) return false;
  out = static_cast<uint32_t>(value);
  return true;
}

// Signed whole metres -- altitude, unlike heading/speed/zoom, can be below
// zero (there is real terrain below sea level).
bool parseInt(std::string_view s, int32_t& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool negative = false;
  if (s[0] == '+' || s[0] == '-') {
    negative = (s[0] == '-');
    i = 1;
  }
  if (i >= s.size()) return false;  // "+", "-"

  int64_t value = 0;
  size_t digits = 0;
  for (; i < s.size(); ++i) {
    if (!isDigit(s[i])) return false;
    if (digits >= 9) return false;  // well past Everest either sign; keeps int64 safe
    value = value * 10 + (s[i] - '0');
    ++digits;
  }
  out = static_cast<int32_t>(negative ? -value : value);
  return true;
}

MapCommand fail(MapCommandError error) {
  MapCommand cmd;
  cmd.type = MapCommandType::Error;
  cmd.error = error;
  return cmd;
}

// `<name> <0..max>` -- the shape shared by heading, zoom and marker.
MapCommand parseSingleUint(const Tokens& tokens, MapCommandType type, uint32_t max, uint8_t MapCommand::* field) {
  if (tokens.n != 2) return fail(MapCommandError::BadArity);
  uint32_t value = 0;
  if (!parseUint(tokens.t[1], value)) return fail(MapCommandError::BadNumber);
  if (value > max) return fail(MapCommandError::OutOfRange);
  MapCommand cmd;
  cmd.type = type;
  cmd.*field = static_cast<uint8_t>(value);
  return cmd;
}

MapCommand parsePos(const Tokens& tokens) {
  if (tokens.n < 3) return fail(MapCommandError::BadArity);

  int64_t lat = 0;
  int64_t lon = 0;
  if (!parseDegrees(tokens.t[1], lat) || !parseDegrees(tokens.t[2], lon)) {
    return fail(MapCommandError::BadNumber);
  }
  if (lat < -kLatMaxE7 || lat > kLatMaxE7) return fail(MapCommandError::OutOfRange);
  if (lon < -kLonMaxE7 || lon > kLonMaxE7) return fail(MapCommandError::OutOfRange);

  MapCommand cmd;
  cmd.type = MapCommandType::Pos;
  cmd.latE7 = static_cast<int32_t>(lat);
  cmd.lonE7 = static_cast<int32_t>(lon);

  // Optional tail. Heading and speed are accepted bare or behind their own
  // keyword, and bare values fill heading first, then speed -- altitude can
  // be negative, so it never takes a bare slot (a bare "-5" would be
  // ambiguous with a mistyped heading/speed) and is only ever `alt <n>`.
  size_t i = 3;
  while (i < tokens.n) {
    bool wantHeading = false;
    bool wantSpeed = false;
    bool wantAltitude = false;
    if (tokens.t[i] == "heading") {
      wantHeading = true;
      ++i;
    } else if (tokens.t[i] == "speed") {
      wantSpeed = true;
      ++i;
    } else if (tokens.t[i] == "alt") {
      wantAltitude = true;
      ++i;
    } else if (!cmd.hasHeading) {
      wantHeading = true;
    } else if (!cmd.hasSpeed) {
      wantSpeed = true;
    } else {
      return fail(MapCommandError::BadArity);
    }

    if (i >= tokens.n) return fail(MapCommandError::BadArity);                  // keyword with no value
    if (wantHeading && cmd.hasHeading) return fail(MapCommandError::BadArity);  // given twice
    if (wantSpeed && cmd.hasSpeed) return fail(MapCommandError::BadArity);
    if (wantAltitude && cmd.hasAltitude) return fail(MapCommandError::BadArity);

    if (wantAltitude) {
      int32_t value = 0;
      if (!parseInt(tokens.t[i], value)) return fail(MapCommandError::BadNumber);
      if (value < kMinAltitudeM || value > kMaxAltitudeM) return fail(MapCommandError::OutOfRange);
      cmd.altitudeM = static_cast<int16_t>(value);
      cmd.hasAltitude = true;
    } else {
      uint32_t value = 0;
      if (!parseUint(tokens.t[i], value)) return fail(MapCommandError::BadNumber);
      if (wantHeading) {
        if (value > kMaxHeading) return fail(MapCommandError::OutOfRange);
        cmd.heading = static_cast<uint8_t>(value);
        cmd.hasHeading = true;
      } else {
        if (value > kMaxSpeedKmh) return fail(MapCommandError::OutOfRange);
        cmd.speedKmh = static_cast<uint16_t>(value);
        cmd.hasSpeed = true;
      }
    }
    ++i;
  }

  return cmd;
}

// `missing` or `missing <offset>`. The bare form is page 0, so a human can
// type it and a paging loop can keep the same command name.
MapCommand parseMissing(const Tokens& tokens) {
  if (tokens.n > 2) return fail(MapCommandError::BadArity);
  MapCommand cmd;
  cmd.type = MapCommandType::Missing;
  if (tokens.n == 2) {
    uint32_t value = 0;
    if (!parseUint(tokens.t[1], value)) return fail(MapCommandError::BadNumber);
    if (value > kMaxMissingOffset) return fail(MapCommandError::OutOfRange);
    cmd.missingOffset = static_cast<uint16_t>(value);
  }
  return cmd;
}

// `skip <z> <col> <row> [<reason>]`. The phone saying it cannot supply a tile.
MapCommand parseSkip(const Tokens& tokens) {
  if (tokens.n < 4 || tokens.n > 5) return fail(MapCommandError::BadArity);
  MapCommand cmd;
  cmd.type = MapCommandType::Skip;

  uint32_t z = 0;
  uint32_t col = 0;
  uint32_t row = 0;
  if (!parseUint(tokens.t[1], z) || !parseUint(tokens.t[2], col) || !parseUint(tokens.t[3], row)) {
    return fail(MapCommandError::BadNumber);
  }
  // z is a uint8_t everywhere else it lives (MissingTileHit, MapTileCoord).
  if (z > 255) return fail(MapCommandError::OutOfRange);
  cmd.skipZ = static_cast<uint8_t>(z);
  cmd.skipCol = col;
  cmd.skipRow = row;

  if (tokens.n == 5) {
    const std::string_view reason = tokens.t[4];
    const size_t copy =
        reason.size() < MapCommand::kSkipReasonBytes - 1 ? reason.size() : MapCommand::kSkipReasonBytes - 1;
    for (size_t i = 0; i < copy; ++i) cmd.skipReason[i] = reason[i];
    cmd.skipReason[copy] = '\0';
  }
  return cmd;
}

// `stale <z> <col> <row>`. The phone saying the CDN has different content for a
// tile the device already holds.
MapCommand parseStale(const Tokens& tokens) {
  if (tokens.n != 4) return fail(MapCommandError::BadArity);
  MapCommand cmd;
  cmd.type = MapCommandType::Stale;

  uint32_t z = 0;
  uint32_t col = 0;
  uint32_t row = 0;
  if (!parseUint(tokens.t[1], z) || !parseUint(tokens.t[2], col) || !parseUint(tokens.t[3], row)) {
    return fail(MapCommandError::BadNumber);
  }
  if (z > 255) return fail(MapCommandError::OutOfRange);
  cmd.skipZ = static_cast<uint8_t>(z);
  cmd.skipCol = col;
  cmd.skipRow = row;
  return cmd;
}

// `checked <n>` or `checked unknown`. The verdict that closes a freshness check.
MapCommand parseChecked(const Tokens& tokens) {
  if (tokens.n != 2) return fail(MapCommandError::BadArity);
  MapCommand cmd;
  cmd.type = MapCommandType::Checked;
  if (tokens.t[1] == "unknown") return cmd;  // checkedKnown stays false

  uint32_t n = 0;
  if (!parseUint(tokens.t[1], n)) return fail(MapCommandError::BadNumber);
  if (n > 0xFFFFu) return fail(MapCommandError::OutOfRange);
  cmd.checkedCount = static_cast<uint16_t>(n);
  cmd.checkedKnown = true;
  return cmd;
}

MapCommand parseMode(const Tokens& tokens) {
  if (tokens.n != 2) return fail(MapCommandError::BadArity);
  MapCommand cmd;
  cmd.type = MapCommandType::Mode;
  if (tokens.t[1] == "ride") {
    cmd.mode = MapRideMode::Ride;
  } else if (tokens.t[1] == "hike") {
    cmd.mode = MapRideMode::Hike;
  } else if (tokens.t[1] == "cycle") {
    cmd.mode = MapRideMode::Cycle;
  } else {
    return fail(MapCommandError::BadMode);
  }
  return cmd;
}

MapCommand parseBare(const Tokens& tokens, MapCommandType type) {
  if (tokens.n != 1) return fail(MapCommandError::BadArity);
  MapCommand cmd;
  cmd.type = type;
  return cmd;
}

}  // namespace

MapCommand parseMapCommand(std::string_view line) {
  const Tokens tokens = tokenize(line);
  if (tokens.overflow) return fail(MapCommandError::BadArity);
  if (tokens.n == 0) return MapCommand{};  // Empty

  const std::string_view name = tokens.t[0];
  if (name == "pos") return parsePos(tokens);
  if (name == "heading") return parseSingleUint(tokens, MapCommandType::Heading, kMaxHeading, &MapCommand::heading);
  if (name == "zoom") return parseSingleUint(tokens, MapCommandType::Zoom, kMaxZoom, &MapCommand::zoom);
  if (name == "marker") return parseSingleUint(tokens, MapCommandType::Marker, kMaxMarker, &MapCommand::marker);
  if (name == "mode") return parseMode(tokens);
  if (name == "redraw") return parseBare(tokens, MapCommandType::Redraw);
  if (name == "tiles") return parseBare(tokens, MapCommandType::Tiles);
  if (name == "missing") return parseMissing(tokens);
  if (name == "skip") return parseSkip(tokens);
  if (name == "have") return parseBare(tokens, MapCommandType::Have);
  if (name == "stale") return parseStale(tokens);
  if (name == "checked") return parseChecked(tokens);
  if (name == "info") return parseBare(tokens, MapCommandType::Info);
  return fail(MapCommandError::UnknownCommand);
}

const char* mapCommandErrorText(MapCommandError error) {
  switch (error) {
    case MapCommandError::None:
      return "none";
    case MapCommandError::UnknownCommand:
      return "unknown_command";
    case MapCommandError::BadArity:
      return "bad_arity";
    case MapCommandError::BadNumber:
      return "bad_number";
    case MapCommandError::OutOfRange:
      return "out_of_range";
    case MapCommandError::BadMode:
      return "bad_mode";
    case MapCommandError::LineTooLong:
      return "line_too_long";
  }
  return "unknown";
}
