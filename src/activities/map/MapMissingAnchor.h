#pragma once

// The rider's last known position, turned into the tile coordinates the missing
// list sorts by.
//
// Its own header because MissingTilePriority.h must stay free of the map layer:
// that file is pure policy and its native test links neither the projection nor
// the settings store. This is the one place the policy meets the geometry.

#include <CrossPointSettings.h>

#include "MapProjection.h"
#include "MapTileGrid.h"
#include "MissingTilePriority.h"

// Reads the fix persisted by the map screen (`SETTINGS.mapHasLastFix`, written
// by MapActivity when a position arrives) and projects it into each LOD tier's
// tile grid.
//
// Persisted, not live, on purpose: this is wanted on the tile sync screen, which
// has no viewport and may never have seen a fix in this session -- the rider
// walks in the door and syncs before a ride, with the GPS off. The last fix is
// the best statement of "where the rider is" available at that moment, and it
// survives a power cycle.
//
// An invalid anchor (no fix ever) is not a failure: the comparator falls back to
// the order it used before, so nothing is worse than it was.
inline MissingTileAnchor missingTileAnchorFromLastFix() {
  MissingTileAnchor anchor;
  if (!SETTINGS.mapHasLastFix) return anchor;

  double mercX = 0.0;
  double mercY = 0.0;
  MapProjection::lonLatToMerc(SETTINGS.mapLastLatE7 / 1e7, SETTINGS.mapLastLonE7 / 1e7, mercX, mercY);

  // One conversion per LOD tier, indexed the way the comparator indexes:
  // missingTileTierRank(z). Three trig-free calls into the same grid maths the
  // viewport uses (MapTileGrid), run once per listing, not per comparison.
  const uint8_t tierZ[] = {12, 11, 13};
  for (uint8_t z : tierZ) {
    uint32_t col = 0;
    uint32_t row = 0;
    MapTileGrid::mercToTileColRow(mercX, mercY, z, col, row);
    const uint8_t rank = missingTileTierRank(z);
    anchor.col[rank] = col;
    anchor.row[rank] = row;
  }
  // Rank 3 is "z outside the three LODs" -- a stale file or a bug. It keeps the
  // zeroes, which puts it at a nonsense distance; harmless, because tier rank
  // already sorts it last regardless.
  anchor.valid = true;
  return anchor;
}
