#pragma once

#include "MapCommandConsole.h"
#include "MissingTilesStore.h"

// Lets the `missing` command read MissingTilesStore without MapConsoleState
// including it: that header pulls in ArduinoJson and the SD-backed
// PersistableStore, and the console half is deliberately free of both (its
// native tests link neither). This adapter is the only place the two meet.
//
// Almost stateless: one anchor, so callers keep one file-scope instance rather
// than a member. No heap, and nothing to keep in sync with the store it
// forwards to. Two screens need it -- the map (which answers `missing` over
// serial or BLE while it is up) and the tile sync screen (which is the whole
// fetch) -- which is why it lives in a header instead of inside one .cpp.
class MissingTilesConsoleSource final : public IMissingTilesSource {
 public:
  // The rider's last known position, in tile coordinates. Load-bearing for
  // correctness, not just for order: the sync screen snapshots the list to draw
  // its rows, and `missing` re-sorts the store before paging it
  // (MapCommandConsole's listMissing, at offset 0). If the two sorts disagree,
  // the phone is told about tiles in one order and the panel labels its rows in
  // another, and every arrival ticks the wrong row.
  void setAnchor(const MissingTileAnchor& anchor) { anchor_ = anchor; }

  void orderForFetch() override { MISSING_TILES.sortByFetchPriority(anchor_); }
  size_t missingTileCount() const override { return MISSING_TILES.hits().size(); }
  MapMissingTile missingTileAt(size_t index) const override {
    const MissingTileHit& hit = MISSING_TILES.hits()[index];
    return MapMissingTile{hit.z, hit.col, hit.row, hit.count};
  }

 private:
  MissingTileAnchor anchor_;
};
