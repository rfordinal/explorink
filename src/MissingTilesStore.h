#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

// The fetch-order policy, including MissingTileAnchor: this header declares the
// anchored sort, so the type has to come with it.
#include <cstdint>
#include <vector>

#include "MissingTilePriority.h"

// One tile the device tried to open and could not -- absent, truncated or
// crc32-mismatched, same "unavailable" definition MapTileSource uses.
struct MissingTileHit {
  uint8_t z = 0;
  // The supplier answered `skip` for this tile: it does not have it. Set by
  // markRefused(), read by autosync so it stops asking for a tile nobody can
  // send (MapActivity::maybeAutoSyncTiles()).
  //
  // Declared here, immediately after `z`, so it lands in the padding the
  // uint32_t alignment already forces -- sizeof(MissingTileHit) stays 16
  // bytes, and the 200-entry cap below stays the same 3.2 KB it was.
  //
  // **Not persisted**, deliberately: toJson()/fromJson() do not carry it. The
  // tile does not exist *today*; the CDN may hold it next week, and a reboot
  // is the natural moment to try again.
  bool refused = false;
  uint32_t col = 0;
  uint32_t row = 0;
  uint32_t count = 0;
};

// Persists which tiles MapActivity has hatched, across restarts, with a
// count of how many separate viewport resets asked for each one. The device
// has no network of its own: the record exists so somebody else can fill the
// gaps -- a laptop tool reading the file off the card, TileSyncActivity
// asking a phone for the whole list, or the map screen's autosync asking for
// what is hatched on screen right now (docs/missing-tiles.md).
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

  // Marks a tile as one the supplier said it does not have (`skip` on the
  // command channel). Returns true if the tile was on the list.
  //
  // The entry stays: the tile is still missing and the map still hatches it.
  // What changes is that autosync stops asking for it -- without this a rider
  // parked at the edge of coverage re-hatches the same tile on every viewport
  // reset, and every one of those would be a fresh ask for a tile the phone
  // has already refused, burning mobile data and BLE airtime on the same
  // answer forever.
  //
  // Does not mark the store dirty. The flag is in-memory only (see the
  // MissingTileHit comment), so there is nothing new to write.
  bool markRefused(uint8_t z, uint32_t col, uint32_t row);

  // True when this tile is on the list and has been refused. Unknown tiles
  // answer false -- never asked for, so never refused.
  bool isRefused(uint8_t z, uint32_t col, uint32_t row) const;

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

  // Same order, but with "near the rider" ahead of "hatched often" inside each
  // LOD tier. The anchor is the last known fix in tile coordinates; see
  // MissingTilePriority.h for why distance outranks the hit count.
  void sortByFetchPriority(const MissingTileAnchor& anchor);
};

#define MISSING_TILES MissingTilesStore::getInstance()
