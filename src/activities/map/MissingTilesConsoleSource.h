#pragma once

#include "MapCommandConsole.h"
#include "MissingTilesStore.h"

// Lets the `missing` command read MissingTilesStore without MapConsoleState
// including it: that header pulls in ArduinoJson and the SD-backed
// PersistableStore, and the console half is deliberately free of both (its
// native tests link neither). This adapter is the only place the two meet.
//
// Stateless, so callers keep one file-scope instance rather than a member: no
// heap, and nothing to keep in sync with the store it forwards to. Two screens
// need it -- the map (which answers `missing` over serial or BLE while it is
// up) and the tile sync screen (which is the whole fetch) -- which is why it
// lives in a header instead of inside one .cpp.
class MissingTilesConsoleSource final : public IMissingTilesSource {
 public:
  void orderForFetch() override { MISSING_TILES.sortByFetchPriority(); }
  size_t missingTileCount() const override { return MISSING_TILES.hits().size(); }
  MapMissingTile missingTileAt(size_t index) const override {
    const MissingTileHit& hit = MISSING_TILES.hits()[index];
    return MapMissingTile{hit.z, hit.col, hit.row, hit.count};
  }
};
