#pragma once

#include <cstdint>

// The GNSS block in the map header: how many bars are filled, and how tall.
//
// Pure arithmetic, no Arduino, no driver -- same shape and same reason as
// MapGnssHeading::stepFor() and MapFixTrust::posTrustFor() next door: a
// decision worth testing on the host, and one a second client (iOS, the
// simulator, a replay tool) has to reproduce exactly. MapActivity owns the
// state and calls in.
//
// ## Two numbers, two questions
//
// **How many** bars are filled says how many satellites the antenna hears.
// **How tall** they all are says how strong the best of them is. A rider needs
// both: sixteen satellites all at 20 dB-Hz and two satellites at 45 dB-Hz are
// both "no fix", and they need opposite things done about them.
//
// ## Why the first calibration was useless, and it is worth remembering
//
// Until 2026-09-10 the count was `min(tracked, 4)` and the height stepped at
// bestSnr 24 and 31. Both stupidly low, so both saturated: four junk satellites
// under a ceiling filled the block, and ONE satellite at 31 dB-Hz made every
// bar full height. Reported by the maintainer with the whole block full and no
// position at all -- which was the block working exactly as written, and
// telling him nothing.
//
// The evidence it was calibrated against, all of it from this L76K:
//
// | situation | tracked | bestSnr | fix? | source |
// |---|---|---|---|---|
// | bench, indoors | 1 | 29 | no | docs/gnss.md, "reported as a GNSS regression" |
// | indoors, no sky | 4 | 19 | no | docs/gnss.md, the `GNSS_NOFIX` sample |
// | desk, through a ceiling | 9 | 32 | yes, q=1, 8 used, HDOP 2.0 | docs/gnss.md, "A working fix indoors" |
// | on a ride | 11 | 38 | yes | docs/gnss.md, the `GNSS_FIX` sample |
// | open sky, 2026-09-09 | 16 | -- | -- | maintainer, reported 2026-09-10 |
//
// Plus the shape of a real ride: 2,269 logged fixes on 2026-09-08 carried
// `sats_used` 3 to 10, mean 7.5, median 8 (`gnss.csv`, kept outside git --
// docs/device-log-forensics.md). `tracked` is always at least `sats_used`.
namespace MapGnssBars {

inline constexpr int kBarCount = 4;

// Satellites tracked -- reporting a non-zero C/N0, Gnss::satsWithSignal() --
// needed for each bar. Chosen by the maintainer on 2026-09-10, against the
// table above, and the semantics he asked for are worth writing down because
// they are NOT "one bar per satellite":
//
//   1 bar  -- 4 satellites, the bare minimum a solution needs. Position
//             possible, and coarse.
//   2 bars -- it should normally have a position by now. A real ride sits here
//             (tracked ~9 to 11 with a fix).
//   3 bars -- good.
//   4 bars -- very precise. Reached under open sky on 2026-09-09, so the top
//             rung is not decoration.
//
// `tracked` and not `satsUsed`: satsUsed comes off GGA and is 0 without a fix,
// so it cannot say anything in the one situation the block exists for -- the
// rider standing there with no position, wanting to know whether to wait or
// walk into the open. And not `satsInView` either: that is the almanac's
// opinion about what is above the horizon and it reads 19 from indoors.
inline constexpr uint8_t kTrackedForBar[kBarCount] = {4, 8, 12, 16};

// A lit bar stays lit while tracked is at most this far below the threshold
// that lit it, and goes out one satellite further down. A satellite drops in
// and out every few seconds, and every change to the block costs a windowed
// refresh -- ~1,081 ms on a T5 S3 Pro (docs/refresh-
// modes.md, measured 2026-09-05). Two satellites of slack against thresholds
// four apart, so the rungs cannot overlap.
inline constexpr uint8_t kTrackedHysteresis = 2;

// The height steps, in dB-Hz of the best satellite's C/N0 (carrier-to-noise
// density: the satellite's signal against the noise floor). Maintainer's
// numbers, 2026-09-10: **below 26 there is nothing worth drawing** and **40 or
// better is full height**.
//
// The two ends are what matter and the middle rungs are spread evenly between
// them. 31 landing on the half step is a coincidence worth keeping: it is the
// same threshold Gnss::injectAidIni() documents as the point where the receiver
// can read the ephemeris off the air by itself, so a half-height bar means
// "this can fix unaided" and a quarter-height one means "it needs help".
//
// Against the evidence table: the indoor bench (29) and the no-sky sample (19)
// draw a quarter and nothing; the indoor fix (32) draws a half; the ride (38)
// draws three quarters. Full height is open sky and only open sky.
inline constexpr int kHeightStepCount = 4;
inline constexpr uint8_t kBestSnrForHeightStep[kHeightStepCount] = {26, 31, 36, 40};

// Same reason as kTrackedHysteresis, in dB-Hz. C/N0 wanders a decibel or two on
// a parked device.
inline constexpr uint8_t kBestSnrHysteresis = 2;

// What the panel is showing. `bars` is 0..kBarCount, `heightStep` is
// 0..kHeightStepCount with 0 meaning "no bar tall enough to draw".
//
// **-1 is "nothing has been drawn yet"**, not zero: the caller compares a fresh
// resolve() against this to decide whether the header needs a repaint at all,
// and a never-drawn block must not compare equal to an empty one.
struct State {
  int bars = -1;
  int heightStep = -1;
};

struct Block {
  int bars = 0;
  int heightStep = 0;

  bool operator==(const Block& other) const { return bars == other.bars && heightStep == other.heightStep; }
  bool operator!=(const Block& other) const { return !(*this == other); }
};

// What the block should show for this reading. **Does not touch `state`** --
// the hysteresis is measured against what is on the panel, and only the draw
// path knows that it drew. Two call sites need this: the repaint decision asks
// whether the answer changed, the draw uses it and then commits it into
// `state`. A resolve() that mutated would apply the hysteresis twice per frame.
Block resolve(uint8_t tracked, uint8_t bestSnr, const State& state);

// Pixel height of a filled bar, or 0 when nothing is tall enough to draw.
// Rounds up, so the four steps against the 14 px header icon row come out 4, 7,
// 11 and 14 -- distinguishable at arm's length, which quarters of 14 rounded
// down (3, 7, 10, 14) are not at the bottom end.
int barHeightPx(int heightStep, int iconHeight);

}  // namespace MapGnssBars
