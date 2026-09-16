#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

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

#endif  // ENABLE_TEAM_MARKERS
