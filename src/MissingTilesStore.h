#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <vector>

// One tile the device tried to open and could not -- absent, truncated or
// crc32-mismatched, same "unavailable" definition MapTileSource uses.
struct MissingTileHit {
  uint8_t z = 0;
  uint32_t col = 0;
  uint32_t row = 0;
  uint32_t count = 0;
};

// Persists which tiles MapActivity has hatched, across restarts, with a
// count of how many separate viewport resets asked for each one. Nothing on
// the device fetches a tile over the network -- the point is purely to leave
// a record on the SD card so a laptop tool (over WebDAV/WiFi file manager,
// same channel that already reaches /.crosspoint/, or a card pull) can read
// which tiles a real ride actually needed and prioritise building them.
//
// MapTileSource::unavailableMask() already knows this for one render, but it
// is cleared by the next begin() call (MapTileSource.h) -- nothing accumulates
// it across resets or across a power cycle. This store is that accumulation.
class MissingTilesStore : public PersistableStore<MissingTilesStore> {
 private:
  std::vector<MissingTileHit> hits_;
  // Set only when a tile is added or evicted -- a membership change. Bumping
  // an already-known tile's count does NOT set this: the count is still
  // written whenever some other change triggers a flush, but a hatch of a
  // tile already on the list is not itself worth an SD write on its own
  // account (see record()).
  bool dirty_ = false;

  // Bounds the JSON file and the RAM behind it. A ride that hits this many
  // distinct missing tiles has bigger problems than this list, so eviction
  // below is about staying bounded, not about losing anything that matters.
  static constexpr size_t kMaxEntries = 200;

  MissingTilesStore() = default;
  ~MissingTilesStore() = default;

  friend class PersistableStore<MissingTilesStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/missing_tiles.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Bumps the hit count for one missing tile (adds it at count 1 if new).
  // In-memory only, CLAUDE.md rule 8 -- caller schedules the actual SD write
  // via isDirty()/flushIfDirty(), same idea as MapActivity's ladder-settings
  // debounce but on its own, much longer schedule (renderViewport()).
  void record(uint8_t z, uint32_t col, uint32_t row);

  // Drops one tile from the list. Returns true if it was on it.
  //
  // This is what a tile arriving over BLE does: the gap is filled, so the
  // record of it is stale and must not be handed out to the next fetch.
  // A membership change, so it marks the store dirty -- unlike a count bump,
  // this one is worth an SD write on its own account, because a list that
  // still asks for tiles the device already has would have the phone send
  // them again after a restart.
  bool forget(uint8_t z, uint32_t col, uint32_t row);

  // True once a tile has been added or evicted since the last flush. A tile
  // already on the list simply getting hit again does not set this -- see
  // the dirty_ comment above.
  bool isDirty() const { return dirty_; }

  // Persists the list if the membership changed since the last flush. No-op,
  // and no SD write, when nothing changed.
  bool flushIfDirty();

  const std::vector<MissingTileHit>& hits() const { return hits_; }

  // Reorders the list into fetch priority (MissingTilePriority.h): the tiles
  // worth transferring first come first, so a fetch cut short has already
  // delivered the useful part. Called when a listing starts, not on every
  // record() -- the order matters only to a reader.
  //
  // Does not mark the store dirty. Entry order in the JSON carries no
  // meaning (fromJson() reads it back positionally into the same list), so a
  // reorder is not new information and does not earn an SD write.
  void sortByFetchPriority();
};

#define MISSING_TILES MissingTilesStore::getInstance()
