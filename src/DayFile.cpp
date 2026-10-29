#include "DayFile.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t kSecondsPerDay = 86400u;
}  // namespace

uint32_t dayNumberFromUtc(uint32_t utc) { return utc / kSecondsPerDay; }

void civilFromDays(uint32_t days, uint32_t& year, uint32_t& month, uint32_t& day) {
  int64_t z = static_cast<int64_t>(days) + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;                                       // [0, 146096]
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);  // [0, 365]
  const int64_t mp = (5 * doy + 2) / 153;                       // [0, 11]
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;               // [1, 31]
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;                  // [1, 12]
  year = static_cast<uint32_t>(y + (m <= 2 ? 1 : 0));
  month = static_cast<uint32_t>(m);
  day = static_cast<uint32_t>(d);
}

bool dayFileName(const char* prefix, uint32_t utc, char* out, size_t outBytes) {
  if (prefix == nullptr || out == nullptr) return false;
  // "<prefix>-" + 8 digits + ".csv" + NUL, or "<prefix>-noclock.csv" + NUL,
  // whichever is longer. Checked rather than truncated: a truncated name is a
  // second file sharing a prefix with the first, which rotation would then
  // treat as a day it cannot read.
  const size_t prefixLen = strlen(prefix);
  const size_t needed = prefixLen + sizeof("-noclock.csv");
  if (outBytes < needed) return false;

  if (utc == 0) {
    snprintf(out, outBytes, "%s-noclock.csv", prefix);
    return true;
  }
  uint32_t year = 0;
  uint32_t month = 0;
  uint32_t day = 0;
  civilFromDays(dayNumberFromUtc(utc), year, month, day);
  snprintf(out, outBytes, "%s-%04lu%02lu%02lu.csv", prefix, static_cast<unsigned long>(year),
           static_cast<unsigned long>(month), static_cast<unsigned long>(day));
  return true;
}
