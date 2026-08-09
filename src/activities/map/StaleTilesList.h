#pragma once

#include <cstddef>
#include <cstdint>

// Tiles the phone said the CDN has republished with different content.
//
// **Deliberately not MissingTilesStore.** Two properties of that store make it
// the wrong home for these, and both were found by review before either was
// built (docs/tile-freshness.md, "Two things the implementation must not get
// wrong"):
//
//  - **It persists.** Its records survive a reboot (src/MissingTilesStore.cpp).
//    A stale entry written there would outlive the fetch that fixed it, and the
//    device would come back up asking the phone for a tile it already holds at
//    the right content.
//  - **It evicts by hit count.** A fresh entry has the lowest count, so a burst
//    of stale tiles would either be dropped first or, in the live mode, push out
//    genuinely missing tiles -- the ones a rider is looking at a hole for.
//
// So: in memory, never written to the card, empty on every boot. Losing it is
// correct behaviour, not a limitation -- the check is cheap and repeats.
//
// Allocates nothing. Fixed arrays sized for a viewport (MapViewport::kMaxTiles
// is 9), with headroom for a ride crossing a tile boundary between checks.
class StaleTilesList {
 public:
  // A viewport's worth several times over. Full means the newest report is
  // dropped: what is on screen now was added first this round, so keeping the
  // oldest is the right way round.
  static constexpr size_t kMaxEntries = 24;

  // Tiles fetched *because* they were stale, and tiles given up on. Both bound
  // the ping-pong guard below.
  static constexpr size_t kMaxRemembered = 12;

  struct Entry {
    uint8_t z = 0;
    uint32_t col = 0;
    uint32_t row = 0;
  };

  // Records a tile as stale. Returns true if it was added.
  //
  // **This is the ping-pong guard.** A tile reported stale a second time after
  // it was already fetched for staleness is not stale again -- the fetch did
  // not fix it, because a cache served the old copy or the index is ahead of
  // the tiles. Asking once more would ask forever, so it is given up on
  // instead. Without this, the live mode is an endless BLE transfer with a
  // rider's battery behind it (docs/tile-freshness.md).
  bool add(uint8_t z, uint32_t col, uint32_t row) {
    if (hasGivenUp(z, col, row)) return false;
    if (has(fetched_, fetchedCount_, z, col, row)) {
      giveUp(z, col, row);
      return false;
    }
    if (contains(z, col, row)) return false;
    if (count_ >= kMaxEntries) return false;
    entries_[count_++] = Entry{z, col, row};
    return true;
  }

  // The tile arrived. It leaves the list and is remembered as having been
  // fetched for staleness, which is what arms the guard in add().
  void onArrived(uint8_t z, uint32_t col, uint32_t row) {
    if (!contains(z, col, row)) return;  // an ordinary missing-tile arrival
    remove(z, col, row);
    push(fetched_, fetchedCount_, z, col, row);
  }

  // Drops a tile without remembering it -- the phone refused it, the rider left.
  void remove(uint8_t z, uint32_t col, uint32_t row) {
    for (size_t i = 0; i < count_; ++i) {
      if (entries_[i].z != z || entries_[i].col != col || entries_[i].row != row) continue;
      entries_[i] = entries_[count_ - 1];
      --count_;
      return;
    }
  }

  bool contains(uint8_t z, uint32_t col, uint32_t row) const { return has(entries_, count_, z, col, row); }

  // "Stop asking about this one." Also removes it from the active list, so the
  // next drain does not pick it up again.
  void giveUp(uint8_t z, uint32_t col, uint32_t row) {
    remove(z, col, row);
    if (hasGivenUp(z, col, row)) return;
    push(givenUp_, givenUpCount_, z, col, row);
  }

  bool hasGivenUp(uint8_t z, uint32_t col, uint32_t row) const { return has(givenUp_, givenUpCount_, z, col, row); }

  size_t count() const { return count_; }
  bool empty() const { return count_ == 0; }
  const Entry& at(size_t index) const { return entries_[index]; }

  // Clears the stale list. Does **not** clear the fetched or given-up sets:
  // those exist to survive exactly this, so a tile the fetch could not fix is
  // not rediscovered by the next check and asked for again.
  void clear() { count_ = 0; }

 private:
  static bool has(const Entry* list, size_t n, uint8_t z, uint32_t col, uint32_t row) {
    for (size_t i = 0; i < n; ++i) {
      if (list[i].z == z && list[i].col == col && list[i].row == row) return true;
    }
    return false;
  }

  // Ring behaviour on overflow: the oldest is forgotten and that tile may be
  // tried once more. The safer overflow of the two -- a bounded amount of extra
  // traffic beats never being able to fix a tile again.
  static void push(Entry* list, size_t& n, uint8_t z, uint32_t col, uint32_t row) {
    if (has(list, n, z, col, row)) return;
    if (n < kMaxRemembered) {
      list[n++] = Entry{z, col, row};
      return;
    }
    for (size_t i = 1; i < kMaxRemembered; ++i) list[i - 1] = list[i];
    list[kMaxRemembered - 1] = Entry{z, col, row};
  }

  Entry entries_[kMaxEntries];
  Entry fetched_[kMaxRemembered];
  Entry givenUp_[kMaxRemembered];
  size_t count_ = 0;
  size_t fetchedCount_ = 0;
  size_t givenUpCount_ = 0;
};
