#include "GroupStore.h"

#if defined(ENABLE_GROUP_MARKERS) && ENABLE_GROUP_MARKERS

void GroupStore::clear() {
  for (auto& fix : fixes_) fix = GroupFix{};
}

bool GroupStore::set(size_t slot, const GroupFix& fix) {
  if (slot >= kSlotCount) return false;
  fixes_[slot] = fix;
  fixes_[slot].present = true;
  return true;
}

void GroupStore::clearSlot(size_t slot) {
  if (slot >= kSlotCount) return;
  fixes_[slot] = GroupFix{};
}

size_t GroupStore::presentCount() const {
  size_t n = 0;
  for (const auto& fix : fixes_) {
    if (fix.present) ++n;
  }
  return n;
}

#endif  // ENABLE_GROUP_MARKERS
