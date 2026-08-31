#include <Gnss.h>

#include <cstdlib>
#include <cstring>

namespace {

// Copy field `index` of a comma-separated NMEA payload into `out`. Index 0 is
// the sentence type ("GNGGA"), 1 is the first data field. An absent or empty
// field yields an empty string, which every caller below treats as "unknown" --
// a receiver with no fix sends GGA with most fields empty rather than zeroed.
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
  return static_cast<uint32_t>(days * 86400L + hour * 3600L + minute * 60L + second);
}

// Two digits at a known offset, with no validation beyond "they are digits".
bool twoDigits(const char* text, size_t offset, uint8_t* out) {
  const char high = text[offset];
  const char low = text[offset + 1];
  if (high < '0' || high > '9' || low < '0' || low > '9') return false;
  *out = static_cast<uint8_t>((high - '0') * 10 + (low - '0'));
  return true;
}

}  // namespace

bool Gnss::begin(const GnssConfig& config) {
  if (running_) return true;
  if (config.serial == nullptr) return false;

  config_ = config;

  if (config_.powerEnable != nullptr && !config_.powerEnable(true)) {
    return false;
  }
  if (config_.powerSettleMs > 0) {
    delay(config_.powerSettleMs);
  }

  config_.serial->begin(config_.baud, SERIAL_8N1, config_.rxPin, config_.txPin);

  line_[0] = '\0';
  lineLength_ = 0;
  lineOverflowed_ = false;
  fix_ = GnssFix();
  for (uint8_t i = 0; i < kMaxTalkers; ++i) {
    talkers_[i] = TalkerState();
  }
  haveTime_ = false;
  haveDate_ = false;
  sentences_ = 0;
  checksumErrors_ = 0;
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

  const uint32_t fixMarkBefore = lastFixMs_;

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
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (lineLength_ > 0 && !lineOverflowed_) {
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

  return lastFixMs_ != fixMarkBefore;
}

void Gnss::handleSentence(char* line, size_t length) {
  // The line arrives without its leading '$'. Everything up to '*' is the
  // checksummed payload; the two hex digits after it are the checksum.
  const char* star = strchr(line, '*');
  if (star == nullptr || static_cast<size_t>(star - line) + 3 > length) {
    ++checksumErrors_;
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
    parseGsv(line, line);
  }
}

void Gnss::parseGga(const char* body) {
  char field[16];

  // Field 6 is the fix quality; 0 means the rest of the sentence carries no
  // position, so nothing is committed from it.
  if (!nmeaField(body, 6, field, sizeof(field))) return;
  const uint8_t quality = static_cast<uint8_t>(atoi(field));

  // Time first: it is valid long before a position is, and the caller wants to
  // see the clock advancing as evidence the receiver is alive.
  if (nmeaField(body, 1, field, sizeof(field)) && strlen(field) >= 6) {
    uint8_t hour = 0, minute = 0, second = 0;
    if (twoDigits(field, 0, &hour) && twoDigits(field, 2, &minute) && twoDigits(field, 4, &second) &&
        hour < 24 && minute < 60 && second < 61) {
      hour_ = hour;
      minute_ = minute;
      second_ = second;
      haveTime_ = true;
      recomputeUtc();
    }
  }

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
  lastFixMs_ = millis();
  if (lastFixMs_ == 0) lastFixMs_ = 1;  // 0 is the "never" sentinel
  if (firstFix) {
    ttffMs_ = lastFixMs_ - beginMs_;
    if (ttffMs_ == 0) ttffMs_ = 1;
  }
}

void Gnss::parseRmc(const char* body) {
  char field[16];

  // Field 2 is A (valid) or V (warning). Speed and course from a V sentence
  // are not trustworthy, but the date is: the receiver decodes it from the
  // almanac before it has a position.
  if (nmeaField(body, 9, field, sizeof(field)) && strlen(field) >= 6) {
    uint8_t day = 0, month = 0, shortYear = 0;
    if (twoDigits(field, 0, &day) && twoDigits(field, 2, &month) && twoDigits(field, 4, &shortYear) &&
        day >= 1 && day <= 31 && month >= 1 && month <= 12) {
      day_ = day;
      month_ = month;
      // NMEA's two-digit year. The receiver has no century, so 00-79 is read
      // as 2000-2079 and 80-99 as 1980-1999 -- the GPS epoch starts in 1980,
      // so nothing earlier can legitimately appear here.
      year_ = static_cast<uint16_t>(shortYear >= 80 ? 1900 + shortYear : 2000 + shortYear);
      haveDate_ = true;
      recomputeUtc();
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
    state->snrCount = state->pendingCount;
    state->snrBest = state->pendingBest;
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
  // More constellations than the table holds. Dropping the extra is better
  // than evicting a talker whose counts are already committed.
  return nullptr;
}

void Gnss::recomputeUtc() {
  if (!haveTime_ || !haveDate_) return;
  fix_.utc = toUnixSeconds(year_, month_, day_, hour_, minute_, second_);
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
