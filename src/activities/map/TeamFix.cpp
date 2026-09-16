#include "TeamFix.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

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

#endif  // ENABLE_TEAM_MARKERS
