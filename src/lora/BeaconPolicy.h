#pragma once

#include <cstdint>

// When a position goes out, as arithmetic.
//
// **This file is the rules and nothing else.** It reads no clock, no settings,
// no radio and no GNSS receiver; it is handed numbers and answers what to do
// with them. The caller (`meshBeaconTick()` in main.cpp) keeps every side
// effect: taking the time, asking for the position, sending the advert and
// writing the log row.
//
// The split exists because the rules are the part worth arguing about and they
// were unreachable: they sat inline in an 8,000-line main.cpp, where no test
// can see them and a change can only be judged by flashing a board. The phone
// side learned this first -- `SendPolicy.kt` in the Android repo is the same
// shape for the same reason, and it is the half of that feature anybody can
// still reason about (parent `docs/android-app-repo.md`).
//
// **The cadence is a floor, not a period**: no faster than this, not exactly
// this often. Three states share one mechanism
// (`../../docs/lora-mesh.md`, "When a position goes out"):
//
//   moving with a fix   -> a position every kMoveMetres, never faster than the
//                          floor
//   parked with a fix   -> the same position again once the silence cap
//                          expires, because an hour in a pub still has to say
//                          where the pub is
//   no fix              -> a nameplate with no coordinate, same cap
//
// The numbers are OpenTrailPaper's, not ours: 100 m and an hourly heartbeat,
// from a project that has run groups on this band for years
// (`../../../docs/lora.md`, "And their cadence is the precedent our own guess
// was missing"). Ours start there and have to say what they buy before moving.
namespace BeaconPolicy {

// How far the rider moves before the position is worth a packet.
inline constexpr uint32_t kMoveMetres = 100;

// The longest this node stays silent, whatever the movement test says. The
// parked case is the one that is easy to get wrong: an hour in a pub on a hike
// still has to tell the group where the pub is, so silence is capped rather
// than left to the movement test.
inline constexpr uint32_t kMaxSilenceSec = 3600;

// What the caller should do with this tick.
enum class Action : uint8_t {
  // The floor has not elapsed. Not an event: this happens thousands of times
  // an hour and is the timer doing its job, so it is never logged. What makes
  // those silences readable is the cadence carried on the `up` row
  // (`LoraEventLine.h`, "What is an event and what is not").
  Nothing,
  Send,
  // The floor elapsed and the node chose not to speak, which is a decision
  // with a reason and therefore worth a row -- throttled, see `logDecline`.
  Decline,
};

enum class Reason : uint8_t {
  // Nothing has gone out yet on this radio.
  First,
  // A fix appeared or disappeared. "I can see the sky again" is news that
  // should not wait out the whole silence cap.
  FixChanged,
  // Moved at least kMoveMetres since the position last sent.
  Moved,
  // Nothing happened for kMaxSilenceSec; say where we are anyway.
  Keepalive,
  // **No wall clock, no transmit.** Maintainer's rule, 2026-10-23. MeshCore
  // stamps every advert with the RTC, an unseeded one counts from an arbitrary
  // epoch, and the receiver dates the marker by that stamp -- so an advert sent
  // without a clock arrives years old and is hidden on arrival. Refusing is the
  // only answer that misrepresents nothing (`../../docs/lora-mesh.md`, "No wall
  // clock, no transmit").
  NoClock,
  // The floor elapsed, there is a clock, and nothing has changed worth a packet.
  NoNews,
};

struct Input {
  // False before the first advert of this radio session.
  bool hasSentEver = false;
  // Since the last advert that actually went out. Meaningless when
  // `hasSentEver` is false.
  uint32_t sinceLastSentMs = 0;
  // The cadence floor, from `loraBeaconSec`. Never below kMeshBeaconMinSec --
  // that clamp belongs to the setting, not here.
  uint32_t floorMs = 0;
  // Whether a real time is available from anywhere. See Reason::NoClock.
  bool hasWallClock = false;
  // Whether a position is available at all -- from the phone over BLE or from
  // the on-board receiver, whichever the map itself is reading.
  bool hasFix = false;
  // What the last advert carried, so this one can tell whether it has news.
  bool lastSentHadFix = false;
  // Metres from the position last sent. **Only read when both this tick and
  // the last advert had a fix**, because a distance to a position that was
  // never sent is not a distance.
  uint32_t movedM = 0;
  // The decline-row throttle, carried by the caller. One row per cadence, so a
  // node that cannot speak does not write a row per loop tick.
  bool declineLoggedEver = false;
  uint32_t sinceLastDeclineLogMs = 0;
};

struct Decision {
  Action action = Action::Nothing;
  Reason reason = Reason::First;
  // Only meaningful with Action::Decline. False means the same decline was
  // already written inside this cadence.
  bool logDecline = false;
};

// Pure. Same input, same answer, every time.
Decision decide(const Input& in);

}  // namespace BeaconPolicy
