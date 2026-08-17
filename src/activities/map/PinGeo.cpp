#include "PinGeo.h"

#include <cstdio>

namespace {

// Metres per 1e-7 degree of latitude, times 1e6: 111320 m per degree.
constexpr int64_t kMetresPerE7Micro = 11132;
constexpr int64_t kMicro = 1000000;

// cos(latitude) scaled by 1024, one entry per whole degree 0..90. Interpolated
// linearly between entries, which keeps the worst-case error well under the 10 m
// the result is rounded to. A table rather than libm's cosf: nothing else in the
// map path links libm, and a 91-entry uint16 table is 182 bytes of flash.
constexpr uint16_t kCos1024[91] = {
    1024, 1024, 1023, 1023, 1022, 1020, 1018, 1016, 1014, 1011, 1008, 1005, 1002, 998, 994, 989, 984, 979, 974,
    968,  962,  956,  949,  943,  935,  928,  920,  912,  904,  896,  887,  878,  868, 859, 849, 839, 828, 818,
    807,  796,  784,  773,  761,  749,  737,  724,  711,  698,  685,  672,  658,  644, 630, 616, 602, 587, 573,
    558,  543,  527,  512,  496,  481,  465,  449,  433,  416,  400,  384,  367,  350, 333, 316, 299, 282, 265,
    248,  230,  213,  195,  178,  160,  143,  125,  107,  89,   71,   54,   36,   18,  0,
};

// cos of |latitude| in 1e7 degrees, scaled by 1024.
int64_t cos1024(int32_t latE7) {
  int64_t deg10 = latE7;
  if (deg10 < 0) deg10 = -deg10;
  if (deg10 > 900000000) deg10 = 900000000;
  const int64_t whole = deg10 / 10000000;
  const int64_t frac = deg10 % 10000000;  // 0 .. 9999999
  const int64_t a = kCos1024[whole];
  const int64_t b = whole < 90 ? kCos1024[whole + 1] : 0;
  return a + (b - a) * frac / 10000000;
}

// Integer square root, Newton-free: the classic bit-by-bit method. No float, and
// no dependency on libm's sqrt for a value that is about to be rounded to 10 m.
uint32_t isqrt64(uint64_t value) {
  uint64_t remainder = value;
  uint64_t result = 0;
  uint64_t bit = 1ull << 62;
  while (bit > remainder) bit >>= 2;
  while (bit != 0) {
    if (remainder >= result + bit) {
      remainder -= result + bit;
      result = (result >> 1) + bit;
    } else {
      result >>= 1;
    }
    bit >>= 2;
  }
  return static_cast<uint32_t>(result);
}

}  // namespace

namespace PinGeo {

uint32_t distanceM(int32_t lat1E7, int32_t lon1E7, int32_t lat2E7, int32_t lon2E7) {
  const int64_t dLat = static_cast<int64_t>(lat2E7) - static_cast<int64_t>(lat1E7);
  int64_t dLon = static_cast<int64_t>(lon2E7) - static_cast<int64_t>(lon1E7);
  // The short way round: two points either side of the antimeridian are close,
  // and a naive difference would make them 40,000 km apart.
  if (dLon > 1800000000) dLon -= 3600000000ll;
  if (dLon < -1800000000) dLon += 3600000000ll;

  // The scale is taken at the midpoint latitude, not at either end: at the far
  // end of a long north-south separation the two cosines differ, and the
  // midpoint is the cheap symmetric answer (either end would make the distance
  // depend on the argument order).
  const int32_t midLatE7 = static_cast<int32_t>((static_cast<int64_t>(lat1E7) + lat2E7) / 2);

  const int64_t dyM = dLat * kMetresPerE7Micro / kMicro;
  const int64_t dxM = dLon * kMetresPerE7Micro * cos1024(midLatE7) / (kMicro * 1024);

  const uint64_t sum = static_cast<uint64_t>(dyM * dyM) + static_cast<uint64_t>(dxM * dxM);
  return isqrt64(sum);
}

void formatDistance(uint32_t metres, char* buf, size_t bufLen) {
  if (buf == nullptr || bufLen == 0) return;
  buf[0] = '\0';
  if (bufLen < 12) return;

  if (metres < 1000) {
    const uint32_t rounded = ((metres + 5) / 10) * 10;
    // 999 m rounds to 1000, and "1000 m" is not what the next bracket would say
    // about the same distance -- fall through rather than print two spellings of
    // one kilometre.
    if (rounded < 1000) {
      snprintf(buf, bufLen, "%lu m", static_cast<unsigned long>(rounded));
      return;
    }
  }
  if (metres < 10000) {
    const uint32_t tenths = (metres + 50) / 100;
    snprintf(buf, bufLen, "%lu.%lu km", static_cast<unsigned long>(tenths / 10),
             static_cast<unsigned long>(tenths % 10));
    return;
  }
  snprintf(buf, bufLen, "%lu km", static_cast<unsigned long>((metres + 500) / 1000));
}

}  // namespace PinGeo
