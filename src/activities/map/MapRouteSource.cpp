#include "MapRouteSource.h"

#include <cstring>

MapRouteSource::MapRouteSource(IFileSource& file, const MapProjection& proj) : file_(file), proj_(proj) {}

bool MapRouteSource::load(const char* path) {
  unload();
  if (path == nullptr) return false;

  const size_t len = std::strlen(path);
  if (len == 0 || len >= kMaxPathLen) return false;

  if (!reader_.open(file_, path)) return false;
  // The point array is checked here, once, rather than on every viewport reset:
  // the file does not change underneath a session, and re-reading 24 KB purely
  // to re-confirm a checksum would double what the route costs per reset.
  if (!reader_.verifyPoints()) {
    reader_.close();
    return false;
  }

  std::memcpy(path_, path, len + 1);
  loaded_ = true;
  pointsEmitted_ = 0;
  return true;
}

void MapRouteSource::unload() {
  reader_.close();
  loaded_ = false;
  pointsEmitted_ = 0;
  path_[0] = '\0';
}

bool MapRouteSource::computeFit(int screenWidth, int screenHeight, MapRouteFit::Result& out) {
  if (!loaded_) return false;
  if (!reader_.beginPoints()) return false;

  // The header's bbox centre, which only has to be near the route -- it keeps
  // the accumulators inside float's precision and is not the final anchor
  // (MapRouteFit.h).
  const int32_t centreX = static_cast<int32_t>((static_cast<int64_t>(reader_.bboxMinX()) + reader_.bboxMaxX()) / 2);
  const int32_t centreY = static_cast<int32_t>((static_cast<int64_t>(reader_.bboxMinY()) + reader_.bboxMaxY()) / 2);

  fit_.begin(centreX, centreY);
  int32_t x = 0;
  int32_t y = 0;
  while (reader_.nextPoint(x, y)) {
    fit_.addPoint(x, y);
  }
  // A short read means the pass ended early, and a fit over part of a route
  // frames the wrong thing. Refuse rather than centre on a fragment.
  if (fit_.pointsSeen() != reader_.pointCount()) return false;

  return fit_.finish(screenWidth, screenHeight, out);
}

bool MapRouteSource::beginRoute() {
  if (!loaded_) return false;
  pointsEmitted_ = 0;
  return reader_.beginPoints();
}

bool MapRouteSource::nextRoutePoint(int32_t& outX, int32_t& outY) {
  int32_t mercX = 0;
  int32_t mercY = 0;
  if (!reader_.nextPoint(mercX, mercY)) return false;
  proj_.projectMercWide(static_cast<double>(mercX), static_cast<double>(mercY), outX, outY);
  ++pointsEmitted_;
  return true;
}
