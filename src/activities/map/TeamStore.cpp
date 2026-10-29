#include "TeamStore.h"

#if defined(ENABLE_TEAM_MARKERS) && ENABLE_TEAM_MARKERS

void TeamStore::clear() {
  for (auto& fix : fixes_) fix = TeamFix{};
}

bool TeamStore::set(size_t slot, const TeamFix& fix) {
  if (slot >= kSlotCount) return false;
  fixes_[slot] = fix;
  fixes_[slot].present = true;
  return true;
}

void TeamStore::clearSlot(size_t slot) {
  if (slot >= kSlotCount) return;
  fixes_[slot] = TeamFix{};
}

size_t TeamStore::presentCount() const {
  size_t n = 0;
  for (const auto& fix : fixes_) {
    if (fix.present) ++n;
  }
  return n;
}

#endif  // ENABLE_TEAM_MARKERS
