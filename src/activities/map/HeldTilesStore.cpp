#include "HeldTilesStore.h"

size_t HeldTilesStore::find(uint8_t z, uint32_t col, uint32_t row) const {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].z == z && entries_[i].col == col && entries_[i].row == row) return i;
  }
  return kMaxEntries;
}

void HeldTilesStore::record(uint8_t z, uint32_t col, uint32_t row, uint32_t contentId) {
  // A tile that did not open has no content to vouch for, and recording it at
  // content 0 would have the phone report it stale forever. It is already on
  // the missing path, which is where it belongs (MapActivity::renderViewport).
  if (contentId == 0) return;

  valid_ = true;

  const size_t known = find(z, col, row);
  if (known != kMaxEntries) {
    HeldTileEntry& e = entries_[known];
    // Same bytes, so any answer about it still stands. Re-recording the same
    // tile is the common case -- every viewport reset walks tiles the last one
    // already had -- and it must not undo a check or the list would never
    // drain.
    if (e.contentId == contentId) return;
    e.contentId = contentId;
    e.flags = 0;
    return;
  }

  size_t slot = count_;
  if (count_ == kMaxEntries) {
    // Full. A settled entry is the cheapest thing to lose: its answer has
    // already been acted on, and the map re-records it the next time it draws
    // that ground.
    slot = kMaxEntries;
    for (size_t i = 0; i < kMaxEntries; ++i) {
      if (entries_[i].checked()) {
        slot = i;
        break;
      }
    }
    if (slot == kMaxEntries) {
      // Nothing settled -- a first check that has not been answered yet, or a
      // rider covering ground faster than the phone answers. Drop the oldest
      // and keep the newest, because the tiles worth checking are the ones near
      // where the rider now is. One 1 KB shift, only on overflow.
      for (size_t i = 1; i < kMaxEntries; ++i) entries_[i - 1] = entries_[i];
      slot = kMaxEntries - 1;
    }
  } else {
    ++count_;
  }

  HeldTileEntry& e = entries_[slot];
  e.z = z;
  e.col = col;
  e.row = row;
  e.contentId = contentId;
  e.flags = 0;
}

bool HeldTilesStore::forget(uint8_t z, uint32_t col, uint32_t row) {
  const size_t known = find(z, col, row);
  if (known == kMaxEntries) return false;
  for (size_t i = known + 1; i < count_; ++i) entries_[i - 1] = entries_[i];
  --count_;
  return true;
}

size_t HeldTilesStore::beginListing() {
  size_t asked = 0;
  for (size_t i = 0; i < count_ && asked < kMaxPerListing; ++i) {
    if (entries_[i].checked()) continue;
    entries_[i].flags |= HeldTileEntry::kAsked;
    ++asked;
  }
  return asked;
}

void HeldTilesStore::markAskedChecked() {
  for (size_t i = 0; i < count_; ++i) {
    if (!entries_[i].asked()) continue;
    entries_[i].flags &= static_cast<uint8_t>(~HeldTileEntry::kAsked);
    entries_[i].flags |= HeldTileEntry::kChecked;
  }
}

void HeldTilesStore::clearAsked() {
  for (size_t i = 0; i < count_; ++i) entries_[i].flags &= static_cast<uint8_t>(~HeldTileEntry::kAsked);
}

void HeldTilesStore::rearmAll() {
  for (size_t i = 0; i < count_; ++i) entries_[i].flags = 0;
}

void HeldTilesStore::clear() {
  count_ = 0;
  valid_ = false;
}

size_t HeldTilesStore::pendingCount() const {
  size_t pending = 0;
  for (size_t i = 0; i < count_; ++i) {
    if (!entries_[i].checked()) ++pending;
  }
  return pending;
}
