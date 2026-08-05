#include <gtest/gtest.h>

#include "MapTilePath.h"

namespace {

MapTileCoord parsed(const char* path) {
  MapTileCoord out;
  EXPECT_TRUE(parseMapTilePath(path, out)) << path;
  return out;
}

bool rejects(const char* path) {
  MapTileCoord out;
  return !parseMapTilePath(path, out);
}

}  // namespace

TEST(MapTilePath, AbsoluteCardPath) {
  // What MapTileSource::buildPath() writes, and what MapTransferReceiver holds
  // in finalPath_ when a file lands.
  const MapTileCoord tile = parsed("/trailink/base/12/2199/1416.tib");
  EXPECT_EQ(tile.z, 12);
  EXPECT_EQ(tile.col, 2199u);
  EXPECT_EQ(tile.row, 1416u);
}

TEST(MapTilePath, SenderRelativePath) {
  // What a begin frame carries -- relative to /trailink, no leading slash.
  const MapTileCoord tile = parsed("base/13/4482/2789.tib");
  EXPECT_EQ(tile.z, 13);
  EXPECT_EQ(tile.col, 4482u);
  EXPECT_EQ(tile.row, 2789u);
}

TEST(MapTilePath, ZeroCoordinatesAreLegal) {
  const MapTileCoord tile = parsed("base/11/0/0.tib");
  EXPECT_EQ(tile.z, 11);
  EXPECT_EQ(tile.col, 0u);
  EXPECT_EQ(tile.row, 0u);
}

TEST(MapTilePath, NonTilePushesAreRejectedNotGuessedAt) {
  // Every one of these is a legitimate transfer that simply clears no
  // MissingTilesStore entry.
  EXPECT_TRUE(rejects("mapstyle.json"));
  EXPECT_TRUE(rejects("routes/sunday.gpx"));
  EXPECT_TRUE(rejects("/trailink/base/12/2199/1416.part"));   // still in flight
  EXPECT_TRUE(rejects("/trailink/base/12/2199.tib"));         // a component short
  EXPECT_TRUE(rejects("/trailink/base/12/2199/1416/9.tib"));  // one too many
}

TEST(MapTilePath, TheSegmentMustBeOwnSegment) {
  // "mybase/" is not the base layer, and matching it would parse a path that
  // belongs to something else.
  EXPECT_TRUE(rejects("/trailink/mybase/12/2199/1416.tib"));
  EXPECT_TRUE(rejects("/trailink/12/2199/1416.tib"));
}

TEST(MapTilePath, JunkComponentsAreRejected) {
  EXPECT_TRUE(rejects("base/12/2199/.tib"));        // empty row
  EXPECT_TRUE(rejects("base//2199/1416.tib"));      // empty z
  EXPECT_TRUE(rejects("base/12/x/1416.tib"));       // not a number
  EXPECT_TRUE(rejects("base/12/-1/1416.tib"));      // no sign
  EXPECT_TRUE(rejects("base/12/2199/1416.tib.x"));  // suffix must end the path
  EXPECT_TRUE(rejects(nullptr));
}

TEST(MapTilePath, ZAboveAUint8IsRejected) {
  // MissingTileHit::z and MapTileCoord::z are uint8_t; a wrapped z would clear
  // the wrong entry.
  EXPECT_TRUE(rejects("base/256/1/1.tib"));
  // And a component long enough to wrap the accumulator does not wrap.
  EXPECT_TRUE(rejects("base/12/99999999999/1.tib"));
}
