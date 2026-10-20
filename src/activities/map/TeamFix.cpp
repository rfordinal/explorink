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
  // Our own clock at receipt. The sender may have had none, and this one is
  // still a real time -- it is what makes a replayed position dateable, and what
  // the black box is read by.
  if (fix.recvUtc != 0 && nowUtc != 0 && nowUtc >= fix.recvUtc) {
    age.known = true;
    age.seconds = nowUtc - fix.recvUtc;
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
  // Under a minute there is nothing to say: `0m` is a number that reads as
  // information and carries none. It only comes up with the stale threshold
  // turned down to zero, and then the hollow head is the whole message.
  if (age.known && age.seconds < 60) return 0;
  if (!age.known) {
    if (bufLen < 2) return 0;
    buf[0] = '?';
    buf[1] = '\0';
    return 1;
  }

  // **Rounded up, to five minutes.** Two reasons and they are the same reason.
  // A number that ticks on e-ink is a waveform pass per tick, and the panel only
  // revisits these every kTeamAgeRefreshMs anyway (MapActivity) -- so a
  // resolution finer than that period is a number that is wrong most of the
  // time. And rounding *up* is the only safe direction: "10 minutes" for a fix
  // that is nine and a half is a position claimed older than it is, which costs
  // nobody anything, while rounding down claims a stale position is fresher.
  const uint32_t minutes = (age.seconds + 59) / 60;
  const uint32_t rounded = ((minutes + kTeamAgeStepMinutes - 1) / kTeamAgeStepMinutes) * kTeamAgeStepMinutes;
  const int written = rounded < 60 ? snprintf(buf, bufLen, "%lum", static_cast<unsigned long>(rounded))
                                   : snprintf(buf, bufLen, "%luh", static_cast<unsigned long>((rounded + 59) / 60));
  return written > 0 && static_cast<size_t>(written) < bufLen ? static_cast<size_t>(written) : 0;
}

// Metres to a short human distance, in the steps the map's own destination
// readout uses (MapActivity::destHeaderText): **100 m under a kilometre**, one
// decimal under ten, whole kilometres above.
//
// Quantised on purpose, and harder than it looks necessary. The number behind it
// is a position that arrived minutes ago, from a fix good to tens of metres, and
// a metre of it is noise dressed as precision. It also costs: a digit that moves
// with every fix is a waveform pass per fix on a panel that would otherwise hold
// its frame (the same reasoning as the header's, ../../docs/nearby-menu.md).
size_t formatDistance(uint32_t metres, char* buf, size_t bufLen) {
  int written = 0;
  // Nearest hundred, with a floor of one: under 100 m the two riders are within
  // sight of each other and `0 m` would read as "on top of you", which no fix
  // this old can promise.
  const uint32_t hundreds = (metres + 50) / 100;
  if (hundreds < 10) {
    written = snprintf(buf, bufLen, "%lu00 m", static_cast<unsigned long>(hundreds < 1 ? 1 : hundreds));
  } else if (metres < 10000) {
    // Same hundred-metre grid, printed as tenths of a kilometre -- so 960 m is
    // `1.0 km` and not `0.9 km`, which the metre branch would have rounded the
    // other way one step earlier.
    written = snprintf(buf, bufLen, "%lu.%lu km", static_cast<unsigned long>(hundreds / 10),
                       static_cast<unsigned long>(hundreds % 10));
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
