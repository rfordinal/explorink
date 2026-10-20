#pragma once

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

#include <cstddef>

#include "TeamFix.h"
#include "TeamMembers.h"

// The live positions, one slot per roster slot. Pure, fixed array, no heap
// (test/team/).
//
// **Derived, never the truth.** The card's black box is the history; this is
// what the last line of it, per member, currently says. A reboot rebuilds it
// from the log (TeamBlackBox::replayLast), so RAM and the card cannot disagree
// -- the same rule PinStore lives under, for the same reason.
class TeamStore {
 public:
  static constexpr size_t kSlotCount = kTeamMaxMembers;

  void clear();

  // Stores a fix against a roster slot. False for an out-of-range slot only; the
  // caller has already decided the sender is a member, because that decision
  // needs the roster and this object deliberately does not hold one.
  bool set(size_t slot, const TeamFix& fix);

  // Drops one member's position without touching the roster -- what `team del`
  // does before the roster row goes, so a marker cannot outlive its member.
  void clearSlot(size_t slot);

  const TeamFix& at(size_t slot) const { return fixes_[slot < kSlotCount ? slot : 0]; }

  size_t presentCount() const;

 private:
  TeamFix fixes_[kSlotCount];
};

#endif  // ENABLE_TEAM_MARKERS
