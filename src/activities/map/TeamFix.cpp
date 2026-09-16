#include "TeamFix.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstdio>

TeamFixAge teamFixAge(const TeamFix& fix, uint32_t nowUtc, uint32_t nowUptimeMs) {
  TeamFixAge age;
  if (!fix.present) return age;

  if (fix.utc != 0 && nowUtc != 0 && nowUtc >= fix.utc) {
    age.known = true;
    age.seconds = nowUtc - fix.utc;
    return age;
  }
  // A fix replayed from the black box carries a previous run's uptime, so the
  // subtraction below would date it by an unrelated clock. Left unknown instead.
  if (!fix.fromLog && nowUptimeMs >= fix.recvUptimeMs) {
    age.known = true;
    age.seconds = (nowUptimeMs - fix.recvUptimeMs) / 1000u;
  }
  return age;
}

TeamVisibility teamFixVisibility(const TeamFixAge& age, uint32_t staleAfterS, uint32_t hideAfterS) {
  if (!age.known) return TeamVisibility::Stale;
  if (hideAfterS != 0 && age.seconds >= hideAfterS) return TeamVisibility::Hidden;
  if (age.seconds >= staleAfterS) return TeamVisibility::Stale;
  return TeamVisibility::Fresh;
}

namespace {

// Minutes under an hour, whole hours above it, and `?` when the age cannot be
// known. Never seconds: a number that ticks on e-ink is a waveform pass per
// tick, and "four minutes ago" is the resolution the question actually has.
size_t formatAge(const TeamFixAge& age, char* buf, size_t bufLen) {
  if (!age.known) {
    if (bufLen < 2) return 0;
    buf[0] = '?';
    buf[1] = '\0';
    return 1;
  }
  const int written = age.seconds < 3600
                          ? snprintf(buf, bufLen, "%lum", static_cast<unsigned long>(age.seconds / 60))
                          : snprintf(buf, bufLen, "%luh", static_cast<unsigned long>(age.seconds / 3600));
  return written > 0 && static_cast<size_t>(written) < bufLen ? static_cast<size_t>(written) : 0;
}

// Metres to a short human distance, same steps as PinGeo::formatDistance --
// which is device-side and would drag its own header in here. 100 m under a
// kilometre, one decimal under ten, whole kilometres above.
size_t formatDistance(uint32_t metres, char* buf, size_t bufLen) {
  int written = 0;
  if (metres < 1000) {
    written = snprintf(buf, bufLen, "%lu m", static_cast<unsigned long>(metres));
  } else if (metres < 10000) {
    written = snprintf(buf, bufLen, "%lu.%lu km", static_cast<unsigned long>(metres / 1000),
                       static_cast<unsigned long>((metres % 1000) / 100));
  } else {
    written = snprintf(buf, bufLen, "%lu km", static_cast<unsigned long>((metres + 500) / 1000));
  }
  return written > 0 && static_cast<size_t>(written) < bufLen ? static_cast<size_t>(written) : 0;
}

}  // namespace

size_t teamMarkerLabel(bool wantDistance, uint32_t metres, bool wantAge, const TeamFixAge& age, char* buf,
                       size_t bufLen) {
  if (buf == nullptr || bufLen == 0) return 0;
  buf[0] = '\0';
  if (!wantDistance && !wantAge) return 0;

  char distance[16] = {};
  char ageText[8] = {};
  const size_t distanceLen = wantDistance ? formatDistance(metres, distance, sizeof(distance)) : 0;
  const size_t ageLen = wantAge ? formatAge(age, ageText, sizeof(ageText)) : 0;
  if (distanceLen == 0 && ageLen == 0) return 0;

  int written = 0;
  if (distanceLen != 0 && ageLen != 0) {
    written = snprintf(buf, bufLen, "%s/%s", distance, ageText);
  } else {
    written = snprintf(buf, bufLen, "%s", distanceLen != 0 ? distance : ageText);
  }
  if (written <= 0 || static_cast<size_t>(written) >= bufLen) {
    buf[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

#endif  // ENABLE_TEAM_MARKERS
