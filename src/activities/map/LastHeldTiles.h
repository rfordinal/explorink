#pragma once

#include "MapCommandConsole.h"

// The last viewport's tiles and the content_id each was opened at, kept alive
// between the map screen and the tile sync screen.
//
// The freshness check needs two things at once: a tile's content_id, and a
// screen the rider chose to spend data on. Only the map has the first -- the
// value is free there because the header parse already put every layer's crc32
// in RAM (docs/tile-freshness.md) -- and the sync screen is squarely the second.
// Neither has both, so the map leaves its answer here and the sync screen picks
// it up.
//
// **In memory, one snapshot, never persisted.** Not CrossPointState: that is
// written to the card, and this changes on every viewport reset. Writing it
// would be an SD write per frame for a value whose whole worth is that it is
// current.
//
// Stale after the rider has ridden on, and that is harmless: the check reports
// on tiles the device does hold, wherever they are, and a tile that has since
// scrolled off screen is still one worth replacing. Empty until the map has
// drawn once, which reads correctly -- there is nothing to check.
//
// A file-scope global in a header, the same shape as
// MissingTilesConsoleSource: two screens need it and neither owns it.
inline MapHeldTiles g_lastHeldTiles;
