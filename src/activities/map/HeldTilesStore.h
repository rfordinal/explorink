#pragma once

#include <cstddef>
#include <cstdint>

// One tile the device holds on its card, and the content_id it was last opened
// at. The pair is what a freshness check compares: the phone reads the same
// slot out of the CDN's index and answers whether they differ
// (../../../docs/tile-freshness.md).
//
// 16 bytes. `flags` lands in the padding the uint32_t alignment already forces,
// so the two bits below are free.
struct HeldTileEntry {
  static constexpr uint8_t kChecked = 1u << 0;  // the phone has answered about this content_id
  static constexpr uint8_t kAsked = 1u << 1;    // in the listing currently on the wire

  uint8_t z = 0;
  uint8_t flags = 0;
  uint32_t col = 0;
  uint32_t row = 0;
  uint32_t contentId = 0;

  bool checked() const { return (flags & kChecked) != 0; }
  bool asked() const { return (flags & kAsked) != 0; }
};

// Every tile the map has drawn since boot, with the content_id each was opened
// at, minus the ones a freshness check has already settled.
//
// **This replaces the single-viewport snapshot it grew out of.** That version
// held one render's worth of tiles -- 16 at the 4x4 worst case -- and was
// rebuilt from scratch on every viewport reset, so a rider who panned across a
// city could only ever have the last screenful checked. Riding is exactly how a
// device accumulates tiles worth checking, so the list has to accumulate the
// same way MissingTilesStore does for the tiles that were absent.
//
// ## It drains, and that is the point
//
// An entry is pending until the phone answers about it, then it stops being
// listed. So `Live` mode works the backlog down in the background on its
// ten-minute cooldown, and the sync screen's check is the same queue emptied on
// demand rather than a second, parallel mechanism.
//
// A tile whose content_id has *moved* since it was recorded -- which is what a
// replaced tile looks like from here -- re-arms itself on the next render. That
// closes the loop after a stale fetch: the new copy is checked once, confirmed
// current, and settles. It cannot spin, because the second answer agrees.
//
// ## Bounded, in memory, never persisted
//
// kMaxEntries fixed entries, no heap at all: the array is 1 KB of static DRAM
// and lives as long as the firmware does, so a std::vector would buy nothing
// and cost a fragmentation risk (CLAUDE.md, The Resource Protocol).
//
// Not persisted, unlike MissingTilesStore, and the difference is real rather
// than an omission. That store exists so somebody *else* can fill the gaps --
// a laptop tool reads the file off the card (../../../docs/missing-tiles.md).
// Nothing off-device reads this list, and a content_id is only trustworthy
// while it matches the bytes on the card, which a reboot cannot promise. A
// reboot simply costs the accumulation, and the map rebuilds it by drawing.
class HeldTilesStore {
 public:
  // The wire is the constraint, not the RAM. Every reply line is packed into
  // ATT payloads and each indication waits for the peer's confirm before the
  // next goes out (MapBleConsole, BlePositionServer::sendCommandBlock) --
  // measured at 688-1503 ms per confirm on the current Android build
  // (../../../docs/tile-freshness.md). A `have` line is ~32 bytes, so seven fit
  // in one 253-byte indication and 64 entries cost ten of them: ten confirms,
  // not sixty-four. Beyond that the check stops being something a rider waits
  // through, and the drain above means a bigger store would not finish sooner
  // anyway -- it would just take more rounds to empty.
  static constexpr size_t kMaxEntries = 64;

  // Adds this tile, or updates the one already recorded for these coordinates.
  //
  // A tile re-recorded at the **same** content_id changes nothing -- it is the
  // same bytes, and an answer about it still stands. At a **different** one the
  // entry re-arms: the card holds something the phone has not seen, whether a
  // stale fetch replaced it or a rebuild arrived some other way.
  //
  // Called from the render walk, where content_id is free: MapTileReader keeps
  // every layer's crc32 in RAM after open() and contentId() is arithmetic over
  // values already there (MapActivity::renderViewport).
  void record(uint8_t z, uint32_t col, uint32_t row, uint32_t contentId);

  // Drops this tile. Returns true if it was on the list.
  //
  // For a tile that left the card, not for one that changed -- record() handles
  // a change, and dropping instead would lose the entry until the map happened
  // to draw it again.
  bool forget(uint8_t z, uint32_t col, uint32_t row);

  // Marks every pending entry as being on the wire and reports how many. The
  // caller is starting a listing; markAskedChecked() or clearAsked() closes it.
  //
  // Stamping rather than counting is what keeps a render that lands mid-check
  // honest: a tile recorded after this call is pending but not asked, so the
  // answer that comes back cannot settle a tile the phone was never told about.
  size_t beginListing();

  // The phone answered `checked <n>`: everything it was asked about has been
  // compared against the index, whether or not it turned out stale. A stale
  // tile is settled too -- the phone is already pushing the replacement, and
  // the new copy re-arms this entry through record() when it is next drawn.
  void markAskedChecked();

  // The phone answered `checked unknown` -- it could not read the index and is
  // claiming nothing. **Not the same as everything being current**, so the
  // asked entries go back to pending rather than settling.
  void clearAsked();

  // Everything is worth checking again: the entries stay, their answers do not.
  void rearmAll();

  void clear();

  size_t size() const { return count_; }
  const HeldTileEntry& at(size_t index) const { return entries_[index]; }

  // Entries no answer covers yet -- what a check is for and what the sync
  // screen counts.
  size_t pendingCount() const;

  // True once the map has drawn at least one tile. An empty store before that
  // means "nothing to check", and after it means "everything is settled"; the
  // two read the same but are not the same, and `have` must not report the
  // first as a clean bill of health.
  bool valid() const { return valid_; }

 private:
  // Returns kMaxEntries when these coordinates are not on the list.
  size_t find(uint8_t z, uint32_t col, uint32_t row) const;

  HeldTileEntry entries_[kMaxEntries];
  size_t count_ = 0;
  bool valid_ = false;
};

// One store, shared by the map screen that fills it and the sync screen that
// spends data emptying it. A file-scope global rather than a getInstance()
// singleton, because that is the shape the snapshot this replaced already had
// and neither screen owns it.
inline HeldTilesStore g_heldTiles;
