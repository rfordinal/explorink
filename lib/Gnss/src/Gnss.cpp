#include <Gnss.h>

#include <cstdlib>
#include <cstring>

namespace {

// Copy field `index` of a comma-separated NMEA payload into `out`. Index 0 is
// the sentence type ("GNGGA"), 1 is the first data field.
//
// Contract: `out` is always NUL-terminated. An absent or empty field yields an
// EMPTY STRING, not a zero and not a failure -- so a caller must test
// `out[0] == '\0'` to mean "unknown", and must not read an empty field as 0.
// This matters because a receiver with no fix sends GGA with most fields empty
// rather than zeroed. The bool return distinguishes only "the payload had that
// many commas" from "it did not"; it says nothing about the field's content.
bool nmeaField(const char* payload, uint8_t index, char* out, size_t outSize) {
  out[0] = '\0';
  const char* cursor = payload;
  for (uint8_t field = 0; field < index; ++field) {
    cursor = strchr(cursor, ',');
    if (cursor == nullptr) return false;
    ++cursor;
  }
  const char* end = strchr(cursor, ',');
  size_t length = (end != nullptr) ? static_cast<size_t>(end - cursor) : strlen(cursor);
  if (length >= outSize) length = outSize - 1;
  memcpy(out, cursor, length);
  out[length] = '\0';
  return true;
}

// NMEA reports position as ddmm.mmmm / dddmm.mmmm: the last two digits before
// the decimal point are minutes and everything before them is degrees. The
// field is not fixed-width across receivers, so the split is found from the
// decimal point rather than assumed.
bool parseLatLon(const char* value, char hemisphere, double* out) {
  if (value[0] == '\0') return false;
  const char* dot = strchr(value, '.');
  const size_t integerLength = (dot != nullptr) ? static_cast<size_t>(dot - value) : strlen(value);
  if (integerLength < 3) return false;
  const size_t degreeLength = integerLength - 2;
  if (degreeLength > 3) return false;

  char degreeBuffer[4] = {0};
  memcpy(degreeBuffer, value, degreeLength);
  const double degrees = atof(degreeBuffer);
  const double minutes = atof(value + degreeLength);
  // A corrupt field can still carry a valid checksum (1 in 256), and without
  // this a value like "9999.9999" commits 100.7 degrees of latitude as a fix.
  if (minutes >= 60.0) return false;
  double result = degrees + minutes / 60.0;
  if (hemisphere == 'S' || hemisphere == 'W') result = -result;
  *out = result;
  return true;
}

// Howard Hinnant's days_from_civil, which is exact for every proleptic
// Gregorian date and needs no table. Cheaper and smaller than pulling in
// <time.h>'s mktime, which would also drag in timezone state this has no use
// for -- NMEA time is always UTC.
uint32_t toUnixSeconds(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                       uint8_t second) {
  int y = static_cast<int>(year);
  y -= month <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(y - era * 400);
  const unsigned dayOfYear = (153u * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const long days = static_cast<long>(era) * 146097 + static_cast<long>(dayOfEra) - 719468;
  // Cast to uint32_t BEFORE multiplying, which makes this correct on any target
  // rather than correct on the one that was checked. The previous version wrote
  // `days * 86400L`: `long` is 32 bits here (confirmed by compiling a
  // _Static_assert with the pinned xtensa-esp32s3-elf toolchain), so that was
  // signed 32-bit arithmetic overflowing on 2038-01-19 -- silently, inside a
  // function whose return type promises 2106. Unsigned 32-bit arithmetic carries
  // it to 2106 with no assumption about `long` at all. `days` is non-negative
  // for any year >= 1970, which is what makes the cast safe.
  return static_cast<uint32_t>(days) * 86400u + hour * 3600u + minute * 60u + second;
}

// hhmmss[.sss] -> h/m/s. Rejects anything that is not six leading digits in
// range; the fractional part is ignored, this library has no use for it.
bool parseNmeaTime(const char* value, uint8_t* hour, uint8_t* minute, uint8_t* second);

// Two digits at a known offset, with no validation beyond "they are digits".
bool twoDigits(const char* text, size_t offset, uint8_t* out) {
  const char high = text[offset];
  const char low = text[offset + 1];
  if (high < '0' || high > '9' || low < '0' || low > '9') return false;
  *out = static_cast<uint8_t>((high - '0') * 10 + (low - '0'));
  return true;
}

bool parseNmeaTime(const char* value, uint8_t* hour, uint8_t* minute, uint8_t* second) {
  if (strlen(value) < 6) return false;
  uint8_t h = 0, m = 0, sec = 0;
  if (!twoDigits(value, 0, &h) || !twoDigits(value, 2, &m) || !twoDigits(value, 4, &sec)) return false;
  // second == 60 is a leap second. Accepted rather than rejected: it is a real
  // value, and toUnixSeconds folds it into the next minute's :00, which is one
  // second wrong for one second a few years apart.
  if (h > 23 || m > 59 || sec > 60) return false;
  *hour = h;
  *minute = m;
  *second = sec;
  return true;
}

}  // namespace

bool Gnss::begin(const GnssConfig& config) {
  if (running_) return true;
  if (config.serial == nullptr) return false;

  config_ = config;

  if (config_.powerEnable != nullptr && !config_.powerEnable(true)) {
    // The hook may have failed halfway -- on the LilyGo board it is two I2C
    // writes and the first one is the one that powers the rail. Ask for off
    // before giving up, so a failed begin() cannot leave a radio powered.
    config_.powerEnable(false);
    return false;
  }
  if (config_.powerSettleMs > 0) {
    delay(config_.powerSettleMs);
  }

  // Before begin(), which is the only time it takes effect. The return value is
  // the size actually granted, so poll()'s near-full test measures against what
  // the driver really allocated rather than what was asked for.
  if (config_.rxBufferBytes > 0) {
    rxBufferBytes_ = config_.serial->setRxBufferSize(config_.rxBufferBytes);
  }
  if (rxBufferBytes_ == 0) rxBufferBytes_ = 256;  // the Arduino default

  config_.serial->begin(config_.baud, SERIAL_8N1, config_.rxPin, config_.txPin);

  line_[0] = '\0';
  lineLength_ = 0;
  lineOverflowed_ = false;
  seenStart_ = false;
  fix_ = GnssFix();
  for (uint8_t i = 0; i < kMaxTalkers; ++i) {
    talkers_[i] = TalkerState();
  }
  sentences_ = 0;
  checksumErrors_ = 0;
  framingErrors_ = 0;
  rxNearlyFull_ = 0;
  fixDirty_ = false;
  bytesRead_ = 0;
  ttffMs_ = 0;
  lastFixMs_ = 0;
  beginMs_ = millis();
  running_ = true;
  return true;
}

void Gnss::end() {
  if (!running_) return;
  config_.serial->end();
  if (config_.powerEnable != nullptr) {
    config_.powerEnable(false);
  }
  running_ = false;
}

uint32_t Gnss::fixAgeMs() const {
  if (lastFixMs_ == 0) return 0;
  return millis() - lastFixMs_;
}

uint32_t Gnss::uptimeMs() const {
  if (!running_) return 0;
  return millis() - beginMs_;
}

bool Gnss::poll() {
  if (!running_) return false;

  fixDirty_ = false;

  // Sampled before draining. A buffer this close to full on entry means the
  // caller was away long enough to risk losing bytes -- and a real overflow
  // discards whole sentences inside the driver, where no counter in this class
  // can see them. This is the only warning a caller gets.
  {
    const int pending = config_.serial->available();
    if (pending > 0 && static_cast<size_t>(pending) + 32 >= rxBufferBytes_) {
      ++rxNearlyFull_;
    }
  }

  while (config_.serial->available() > 0) {
    const int byteRead = config_.serial->read();
    if (byteRead < 0) break;
    ++bytesRead_;
    const char c = static_cast<char>(byteRead);

    if (c == '$') {
      // A '$' mid-sentence means the previous one was cut short. Restarting is
      // the only sane recovery: NMEA has no length field to resynchronise on.
      lineLength_ = 0;
      lineOverflowed_ = false;
      seenStart_ = true;
      continue;
    }
    if (c == '\r' || c == '\n') {
      // Nothing is parsed until the first '$' of the session has been seen. The
      // very first "line" after begin() is almost always the tail of a sentence
      // already in flight, and feeding it to the parser was measured on hardware
      // to land in EITHER error counter depending on where the tear fell: before
      // the '*' the tail still carries a valid "*hh" and fails the checksum, so
      // it lands in checksumErrors_ and pollutes the one counter a bring-up uses
      // to judge the baud rate. Two mid-stream opens on 2026-08-31 landed one in
      // each counter. Discarding up to the first '$' is what actually fixes
      // that; splitting the counters alone did not.
      if (lineLength_ > 0 && !lineOverflowed_ && seenStart_) {
        line_[lineLength_] = '\0';
        handleSentence(line_, lineLength_);
      }
      lineLength_ = 0;
      lineOverflowed_ = false;
      continue;
    }
    if (lineLength_ + 1 >= kLineMax) {
      lineOverflowed_ = true;
      continue;
    }
    line_[lineLength_++] = c;
  }

  return fixDirty_;
}

void Gnss::handleSentence(char* line, size_t length) {
  // The line arrives without its leading '$'. Everything up to '*' is the
  // checksummed payload; the two hex digits after it are the checksum.
  // No '*', or no room for two hex digits after it. That is a framing problem,
  // not a checksum problem -- a garbage burst on a cold UART lands here, and
  // counting it as a checksum error inflates the one number a bring-up uses to
  // judge the baud rate. Separate counters, separate diagnoses.
  const char* star = strchr(line, '*');
  if (star == nullptr || static_cast<size_t>(star - line) + 3 > length) {
    ++framingErrors_;
    return;
  }

  uint8_t computed = 0;
  for (const char* p = line; p < star; ++p) {
    computed ^= static_cast<uint8_t>(*p);
  }
  const uint8_t declared = static_cast<uint8_t>(strtoul(star + 1, nullptr, 16));
  if (computed != declared) {
    ++checksumErrors_;
    return;
  }

  ++sentences_;

  // The raw sink gets the sentence with its checksum still on, before the
  // terminator below removes it. Verified on hardware 2026-08-31: stripping it
  // first produced log lines that looked like NMEA and were not, because a
  // sentence without its "*hh" is not something any NMEA tool will accept.
  if (rawSink_ != nullptr) {
    rawSink_(line, length);
  }

  // Terminate at the '*' so every field parser below sees the payload only.
  // The buffer is this object's own line_, which poll() rebuilds per sentence.
  const size_t payloadLength = static_cast<size_t>(star - line);
  line[payloadLength] = '\0';

  // Talker id is the first two characters, sentence type the next three:
  // GPGGA, GNGGA, GLGSV and so on. A short payload is not ours to interpret.
  if (payloadLength < 5) return;
  const char* type = line + 2;
  if (strncmp(type, "GGA", 3) == 0) {
    parseGga(line);
  } else if (strncmp(type, "RMC", 3) == 0) {
    parseRmc(line);
  } else if (strncmp(type, "GSV", 3) == 0) {
    // Same buffer twice on purpose: GSV needs the talker id, which is the first
    // two characters of the payload, as well as the fields after it. The two
    // parameters are one string read two ways, not two strings.
    parseGsv(line, line);
  } else if (strncmp(type, "ZDA", 3) == 0) {
    parseZda(line);
  }
}

// GGA supplies fix quality, satellites used, position, altitude and HDOP.
//
// It also carries a time field, and this function deliberately does NOT use it
// for fix_.utc. Do not "fix" that by adding it back: GGA has no date, so its
// time can only be paired with a date from some other sentence, and that pairing
// is what regressed the clock by 24 hours at every UTC midnight. The reasoning
// is in full below, at the point where the time field is skipped.
void Gnss::parseGga(const char* body) {
  char field[16];

  // Field 6 is the fix quality; 0 means the rest of the sentence carries no
  // position, so nothing is committed from it.
  if (!nmeaField(body, 6, field, sizeof(field))) return;
  const uint8_t quality = static_cast<uint8_t>(atoi(field));

  // GGA's time field is deliberately NOT used for fix_.utc. It used to be,
  // stitched against the date from the last RMC, and that regresses the clock by
  // 24 hours at every UTC midnight: a 00:00:0x GGA recomputes against
  // yesterday's date until the next RMC arrives. Found by review, 2026-08-31.
  //
  // On the L76K observed here, GGA leads RMC by nine sentences, so the window is
  // about 0.4 s at 9600 baud -- and longer whenever the caller blocks. That
  // ordering is an observation about one receiver, NOT an NMEA rule, and nothing
  // here depends on it: the cure is that date and time are now only ever taken
  // from a single sentence that carries both (RMC, ZDA), so the pair cannot be
  // mismatched whatever order a receiver emits things in. A receiver that sends
  // RMC first would merely have had a shorter bad window, not a correct clock.

  if (nmeaField(body, 7, field, sizeof(field))) {
    fix_.satsUsed = static_cast<uint8_t>(atoi(field));
  }
  if (nmeaField(body, 8, field, sizeof(field)) && field[0] != '\0') {
    fix_.hdop = static_cast<float>(atof(field));
  }

  fix_.quality = quality;
  if (quality == 0) return;

  char latitude[16];
  char latitudeHemisphere[4];
  char longitude[16];
  char longitudeHemisphere[4];
  if (!nmeaField(body, 2, latitude, sizeof(latitude))) return;
  if (!nmeaField(body, 3, latitudeHemisphere, sizeof(latitudeHemisphere))) return;
  if (!nmeaField(body, 4, longitude, sizeof(longitude))) return;
  if (!nmeaField(body, 5, longitudeHemisphere, sizeof(longitudeHemisphere))) return;

  double parsedLatitude = 0.0;
  double parsedLongitude = 0.0;
  if (!parseLatLon(latitude, latitudeHemisphere[0], &parsedLatitude)) return;
  if (!parseLatLon(longitude, longitudeHemisphere[0], &parsedLongitude)) return;

  fix_.latitude = parsedLatitude;
  fix_.longitude = parsedLongitude;
  if (nmeaField(body, 9, field, sizeof(field)) && field[0] != '\0') {
    fix_.altitudeMeters = static_cast<float>(atof(field));
  }

  const bool firstFix = !fix_.valid;
  fix_.valid = true;
  fixDirty_ = true;
  lastFixMs_ = millis();
  if (lastFixMs_ == 0) lastFixMs_ = 1;  // 0 is the "never" sentinel
  if (firstFix) {
    ttffMs_ = lastFixMs_ - beginMs_;
    if (ttffMs_ == 0) ttffMs_ = 1;
  }
}

void Gnss::parseRmc(const char* body) {
  char field[16];

  // Field 2 is A (valid) or V (warning). Speed and course from a V sentence are
  // not trustworthy, but the clock is: the receiver decodes date and time from
  // the signal before it has a position. Both are taken from THIS sentence, so
  // they always describe the same instant.
  char timeField[16];
  if (nmeaField(body, 1, timeField, sizeof(timeField)) && nmeaField(body, 9, field, sizeof(field)) &&
      strlen(field) >= 6) {
    uint8_t hour = 0, minute = 0, second = 0;
    uint8_t day = 0, month = 0, shortYear = 0;
    if (parseNmeaTime(timeField, &hour, &minute, &second) && twoDigits(field, 0, &day) &&
        twoDigits(field, 2, &month) && twoDigits(field, 4, &shortYear) && day >= 1 && day <= 31 &&
        month >= 1 && month <= 12) {
      // NMEA's two-digit year. The receiver sends no century, so 00-79 reads as
      // 2000-2079 and 80-99 as 1980-1999 -- the GPS epoch starts in 1980, so
      // nothing earlier can legitimately appear. ZDA below avoids the guess
      // entirely and is preferred when the receiver sends it.
      const uint16_t year = static_cast<uint16_t>(shortYear >= 80 ? 1900 + shortYear : 2000 + shortYear);
      fix_.utc = toUnixSeconds(year, month, day, hour, minute, second);
      fixDirty_ = true;
    }
  }

  if (!nmeaField(body, 2, field, sizeof(field)) || field[0] != 'A') return;

  if (nmeaField(body, 7, field, sizeof(field)) && field[0] != '\0') {
    fix_.speedKmh = static_cast<float>(atof(field)) * 1.852f;  // knots to km/h
  }
  if (nmeaField(body, 8, field, sizeof(field)) && field[0] != '\0') {
    fix_.courseDegrees = static_cast<float>(atof(field));
  }
}

// ZDA carries time, day, month and a FOUR-digit year in one sentence, which
// makes it strictly better than RMC for the clock: same atomicity, no century
// guess. Not every receiver sends it; the L76K on the LilyGo T5 S3 Pro does.
void Gnss::parseZda(const char* body) {
  char field[16];
  uint8_t hour = 0, minute = 0, second = 0;
  if (!nmeaField(body, 1, field, sizeof(field))) return;
  if (!parseNmeaTime(field, &hour, &minute, &second)) return;

  if (!nmeaField(body, 2, field, sizeof(field))) return;
  const int day = atoi(field);
  if (!nmeaField(body, 3, field, sizeof(field))) return;
  const int month = atoi(field);
  if (!nmeaField(body, 4, field, sizeof(field))) return;
  const int year = atoi(field);
  if (day < 1 || day > 31 || month < 1 || month > 12 || year < 1980 || year > 2105) return;

  fix_.utc = toUnixSeconds(static_cast<uint16_t>(year), static_cast<uint8_t>(month),
                           static_cast<uint8_t>(day), hour, minute, second);
  fixDirty_ = true;
}

void Gnss::parseGsv(const char* talker, const char* body) {
  char field[16];

  TalkerState* state = talkerFor(talker);
  if (state == nullptr) return;

  if (!nmeaField(body, 1, field, sizeof(field))) return;
  const uint8_t messageCount = static_cast<uint8_t>(atoi(field));
  if (!nmeaField(body, 2, field, sizeof(field))) return;
  const uint8_t messageNumber = static_cast<uint8_t>(atoi(field));
  if (!nmeaField(body, 3, field, sizeof(field))) return;
  const uint8_t inView = static_cast<uint8_t>(atoi(field));

  // GSV arrives as a cycle of messageCount sentences per constellation. The
  // signal figures are accumulated across the cycle and committed at its end,
  // so a reader never sees a half-scanned sky.
  if (messageNumber <= 1) {
    state->pendingCount = 0;
    state->pendingBest = 0;
    state->expectedNext = 2;
    state->cycleIntact = true;
  } else if (messageNumber != state->expectedNext) {
    // A message of this cycle was lost -- a checksum error on a 9600 baud line
    // is exactly what checksumErrors_ counts. Without this the survivors
    // accumulate on top of the previous cycle's residue and satsWithSignal()
    // reports up to double. Found by review, 2026-08-31.
    state->cycleIntact = false;
    state->expectedNext = static_cast<uint8_t>(messageNumber + 1);
  } else {
    state->expectedNext = static_cast<uint8_t>(messageNumber + 1);
  }

  // Four fields per satellite from field 4: PRN, elevation, azimuth, C/N0.
  // An empty C/N0 means the satellite is in view but not tracked.
  for (uint8_t slot = 0; slot < 4; ++slot) {
    const uint8_t snrIndex = static_cast<uint8_t>(4 + slot * 4 + 3);
    if (!nmeaField(body, snrIndex, field, sizeof(field))) break;
    if (field[0] == '\0') continue;
    const uint8_t snr = static_cast<uint8_t>(atoi(field));
    if (snr == 0) continue;
    ++state->pendingCount;
    if (snr > state->pendingBest) state->pendingBest = snr;
  }

  state->inView = inView;
  if (messageCount > 0 && messageNumber >= messageCount) {
    if (state->cycleIntact) {
      state->snrCount = state->pendingCount;
      state->snrBest = state->pendingBest;
    }
    // Cleared whether or not the cycle was intact, so a dropped message can
    // never leak into the next sweep.
    state->pendingCount = 0;
    state->pendingBest = 0;
    state->cycleIntact = true;
    state->expectedNext = 1;
  }
}

Gnss::TalkerState* Gnss::talkerFor(const char* id) {
  for (uint8_t i = 0; i < kMaxTalkers; ++i) {
    if (talkers_[i].id[0] == id[0] && talkers_[i].id[1] == id[1]) {
      return &talkers_[i];
    }
  }
  for (uint8_t i = 0; i < kMaxTalkers; ++i) {
    if (talkers_[i].id[0] == 0) {
      talkers_[i].id[0] = id[0];
      talkers_[i].id[1] = id[1];
      return &talkers_[i];
    }
  }
  // More constellations than kMaxTalkers holds. The new one is dropped rather
  // than given a slot by eviction, because every existing slot holds counts that
  // are already committed and being reported: evicting one would silently
  // subtract a whole constellation from satsInView() mid-ride, which reads as
  // the sky emptying. Undercounting one late constellation is the smaller lie.
  return nullptr;
}

uint8_t Gnss::satsInView() const {
  uint16_t total = 0;
  for (uint8_t i = 0; i < kMaxTalkers; ++i) {
    total += talkers_[i].inView;
  }
  return total > 255 ? 255 : static_cast<uint8_t>(total);
}

uint8_t Gnss::satsWithSignal() const {
  uint16_t total = 0;
  for (uint8_t i = 0; i < kMaxTalkers; ++i) {
    total += talkers_[i].snrCount;
  }
  return total > 255 ? 255 : static_cast<uint8_t>(total);
}

uint8_t Gnss::bestSnr() const {
  uint8_t best = 0;
  for (uint8_t i = 0; i < kMaxTalkers; ++i) {
    if (talkers_[i].snrBest > best) best = talkers_[i].snrBest;
  }
  return best;
}
