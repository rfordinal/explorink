#pragma once

#include <cstdint>

#include "activities/map/MapFixTrust.h"

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

// How far the rider moves before the position is worth a packet, on a fix
// whose accuracy is good enough to believe.
inline constexpr uint32_t kMoveMetres = 100;

// The same, on a fix the marker itself would stop trusting.
//
// Under tree cover a phone reports 25 to 50 m, and the distance here is
// measured against a **fixed anchor** -- the position last sent -- so the
// wander accumulates against it rather than cancelling out. An anchor that
// was itself an outlier puts every sample on the far side of the error circle
// past 100 m, and the packet that goes out says the rider moved when nothing
// did. Doubling the threshold costs one update of resolution on a fix that
// never had it.
inline constexpr uint32_t kLooseMoveMetres = 200;

// The line between parked and moving.
//
// **Not a new number.** `MapFixTrust::kLooseAtOrAboveM` is 25 because that is
// the error that still fits inside the drawn marker at the finest zoom rung,
// and `MapTeam::kSelfLogMinMetres` is 25 for the rider's own black box rows.
// One line, three uses.
inline constexpr uint32_t kSettleRadiusM = 25;

// How long inside kSettleRadiusM before stopping is worth saying out loud,
// counted in cadence floors rather than seconds so it scales with whatever
// the rider chose. Two, because one is a pause at a gate and the packet
// should not be spent on it.
inline constexpr uint32_t kSettleHoldFloors = 2;

// The cap on silence while the rider is still moving.
//
// Separate from kMaxSilenceSec because **slow is not parked**, and the hourly
// cap treats them the same: 60 m of scrambling in fifteen minutes trips no
// movement test, so the group holds an hour-old position of somebody who is
// walking. Ten minutes is OpenTrailPaper's number for the same case
// (`../../../docs/lora.md`, "And their cadence is the precedent our own guess
// was missing").
inline constexpr uint32_t kMovingCapSec = 600;

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
  // Moved a little and then stopped, and the group's marker is standing
  // somewhere the rider no longer is. **The most useful packet of a hike**:
  // everything else here is driven by movement, so without it a stop inside
  // the move threshold goes unannounced until the hourly cap. Sent once per
  // stop, then latched until the rider moves again.
  Settled,
  // Moving, but too slowly to trip the move threshold. Caps the silence at
  // kMovingCapSec instead of the hour a parked node gets.
  Moving,
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
  // The fix's own accuracy in metres, `MapFixTrust::kAccuracyUnstated` when
  // the source stated none -- which is every GNSS fix on this hardware.
  //
  // **The on-board receiver has no accuracy to give and HDOP cannot stand in
  // for one.** A 16 h ride of 12,026 fixes found 2.2 % of consecutive pairs
  // physically implausible with a median HDOP of 2.3, comfortably inside
  // Trusted, and the four biggest jumps sat just under the line that would
  // have broken the marker's ring. Geometry does not see multipath
  // (`../../docs/marker-fix-trust.md`, "A ride says HDOP misses what the
  // rider sees"). So this widens the threshold on the phone's number and on
  // nothing else.
  uint16_t accuracyM = MapFixTrust::kAccuracyUnstated;
  // How long the fix has stayed inside kSettleRadiusM of where it first
  // settled. Zero when there is no fix or the rider is still moving.
  uint32_t stillForMs = 0;
  // Whether this stop has already been announced. The latch that makes
  // Reason::Settled one packet rather than one per cadence.
  bool settledAnnounced = false;
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

// How far the rider has to move before it is worth a packet, given what the
// fix admits about itself. Exposed because it is the number most likely to be
// argued with.
uint32_t moveThresholdM(uint16_t accuracyM);

// Pure. Same input, same answer, every time.
Decision decide(const Input& in);

}  // namespace BeaconPolicy
