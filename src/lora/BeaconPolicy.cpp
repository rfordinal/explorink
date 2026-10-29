#include "BeaconPolicy.h"

namespace BeaconPolicy {
namespace {

// One row per cadence, and the stamp behind it is **not** the last-sent stamp.
// That one anchors the silence cap, so moving it forward on a decline would
// postpone the parked heartbeat for as long as the rider stood still -- which
// is exactly the case the heartbeat exists for. One number per question.
bool declineIsWorthARow(const Input& in) {
  if (!in.declineLoggedEver) return true;
  return in.sinceLastDeclineLogMs >= in.floorMs;
}

}  // namespace

Decision decide(const Input& in) {
  Decision out;

  // The floor, first and cheapest. Before the first advert it does not apply:
  // there is no "last send" to measure from, and a node that just came up
  // should say so rather than wait out a cadence.
  if (in.hasSentEver && in.sinceLastSentMs < in.floorMs) {
    out.action = Action::Nothing;
    return out;
  }

  // **Clock before anything else, and throttled.** Without one nothing below
  // matters -- the send would be refused anyway -- and with `hasSentEver`
  // still false the floor above passes on every tick, so an unthrottled
  // refusal would write a row per tick.
  if (!in.hasWallClock) {
    out.action = Action::Decline;
    out.reason = Reason::NoClock;
    out.logDecline = declineIsWorthARow(in);
    return out;
  }

  if (!in.hasSentEver) {
    out.action = Action::Send;
    out.reason = Reason::First;
    return out;
  }

  // Has anything happened that a packet should carry?
  if (in.hasFix != in.lastSentHadFix) {
    out.action = Action::Send;
    out.reason = Reason::FixChanged;
    return out;
  }
  // Only comparable when both ends of the comparison are real positions.
  if (in.hasFix && in.lastSentHadFix && in.movedM >= kMoveMetres) {
    out.action = Action::Send;
    out.reason = Reason::Moved;
    return out;
  }
  if (in.sinceLastSentMs >= kMaxSilenceSec * 1000UL) {
    out.action = Action::Send;
    out.reason = Reason::Keepalive;
    return out;
  }

  out.action = Action::Decline;
  out.reason = Reason::NoNews;
  out.logDecline = declineIsWorthARow(in);
  return out;
}

}  // namespace BeaconPolicy
