#pragma once

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS

#include <cstddef>

#include "GroupFix.h"
#include "GroupMembers.h"

// The live positions, one slot per roster slot. Pure, fixed array, no heap
// (test/group/).
//
// **Derived, never the truth.** The card's black box is the history; this is
// what the last line of it, per member, currently says. A reboot rebuilds it
// from the log (GroupBlackBox::replayLast), so RAM and the card cannot disagree
// -- the same rule PinStore lives under, for the same reason.
class GroupStore {
 public:
  static constexpr size_t kSlotCount = kGroupMaxMembers;

  void clear();

  // Stores a fix against a roster slot. False for an out-of-range slot only; the
  // caller has already decided the sender is a member, because that decision
  // needs the roster and this object deliberately does not hold one.
  bool set(size_t slot, const GroupFix& fix);

  // Drops one member's position without touching the roster -- what `group del`
  // does before the roster row goes, so a marker cannot outlive its member.
  void clearSlot(size_t slot);

  const GroupFix& at(size_t slot) const { return fixes_[slot < kSlotCount ? slot : 0]; }

  size_t presentCount() const;

 private:
  GroupFix fixes_[kSlotCount];
};

#endif  // ENABLE_GROUP_MARKERS
