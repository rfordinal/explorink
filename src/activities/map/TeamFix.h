#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstddef>
#include <cstdint>

// One member's last known position, and the age rules the panel draws by.
//
// Pure: no Arduino, no HAL, no SD (test/team/).
//
// **Transport-independent, deliberately.** Nothing here knows whether the fix
// arrived over LoRa, over BLE from the phone, or from a console command -- the
// source is one field for the log and nothing branches on it
// (../../../docs/team-markers.md, and parent docs/lora.md: "the group-markers layer
// below stays the same, it does not know which protocol fed it").

enum class TeamFixSource : uint8_t {
  Unknown,
  Cmd,   // the console -- devel builds only
  Ble,   // the phone relayed it
  Lora,  // the device's own radio
  Gnss,  // our own receiver; only ever used for the `ME` rows
};

struct TeamFix {
  bool present = false;
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  // The sender's clock at the moment of the fix. 0 means they had none, and it
  // is recorded as such rather than filled in with ours -- a fabricated time in
  // a search is worse than an admitted gap (same rule as PinRecord's utc).
  uint32_t utc = 0;
  // Our uptime when the fix landed. This, not utc, is what dates a fix on a
  // device with no clock, which every X4 is.
  uint32_t recvUptimeMs = 0;
  // Replayed from the black box at boot rather than heard this run. Its uptime
  // belongs to a previous run, so it can only be dated by utc -- and on a device
  // with no clock it cannot be dated at all.
  bool fromLog = false;
  uint8_t heading = 0;  // 0-15, same sixteenth-of-a-turn code the BLE packet uses
  bool hasHeading = false;
  uint16_t speedKmh = 0;
  bool hasSpeed = false;
  TeamFixSource source = TeamFixSource::Unknown;
};

// How old the fix is, and whether that is knowable at all.
struct TeamFixAge {
  bool known = false;
  uint32_t seconds = 0;
};

// `nowUtc` 0 means the device has no clock. Prefers the sender's own timestamp
// when both clocks exist, because that is when the rider was actually there;
// falls back to our receipt uptime for a fix heard this run.
TeamFixAge teamFixAge(const TeamFix& fix, uint32_t nowUtc, uint32_t nowUptimeMs);

// What the map does with it.
enum class TeamVisibility : uint8_t {
  Fresh,   // solid marker
  Stale,   // dithered marker: the position is old and may have moved
  Hidden,  // too old to draw at all
};

// **An unknown age is never hidden.** A last known position with no date is
// still the best lead there is, and a device with no clock would otherwise drop
// every replayed member at boot -- which is exactly when a rider wants to see
// where everyone was. It draws as Stale, so the panel never claims it is
// current. `hideAfterS` 0 turns hiding off entirely.
TeamVisibility teamFixVisibility(const TeamFixAge& age, uint32_t staleAfterS, uint32_t hideAfterS);

// The label under a member's marker. Four shapes, and three of them say
// something the balloon alone cannot: how far and how old.
//
//   ""             everything current, and the rider can see the distance
//   "1.2 km"       far enough that the gap matters
//   "1.2 km/12m"   far and old
//   "12m"          old, but close enough to judge by eye
//
// **Age only when the position has gone stale**, so a group riding together
// draws no text at all; **distance only when the panel cannot show it** -- the
// marker is off the frame, or the rung is zoomed out far enough that "somewhere
// over there" needs a number (MapActivity, kTeamDistanceFromRung).
//
// An age that cannot be known prints `?` rather than a number nobody can stand
// behind: that is every replayed fix on a device with no clock (teamFixAge).
//
// An age under a minute prints nothing at all -- `0m` reads as information and
// carries none.
//
// Every other age is **rounded up to kTeamAgeStepMinutes**, which is also how
// often the panel revisits it: a finer number would be wrong between refreshes,
// and rounding up is the safe direction (a position is never claimed fresher
// than it is).
//
// Writes into `buf` and returns its length; 0 means there is nothing to draw.
size_t teamMarkerLabel(bool wantDistance, uint32_t metres, bool wantAge, const TeamFixAge& age, char* buf,
                       size_t bufLen);

// The step an age is reported in, and the period the map re-reads it on
// (MapActivity::kTeamAgeRefreshMs). One number, because a display step finer
// than the refresh is a number that is wrong most of the time.
inline constexpr uint32_t kTeamAgeStepMinutes = 5;

// Longest label this writes: "1234.5 km/999h" and the terminator, rounded up.
inline constexpr size_t kTeamLabelBytes = 24;

#endif  // ENABLE_TEAM_MARKERS
