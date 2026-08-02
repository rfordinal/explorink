#include "MapPreviewPipeline.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "IFileSource.h"
#include "MapTileGrid.h"
#include "MapTileReader.h"
#include "StdioFileSource.h"

namespace {

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 800;

// style.device.marker_x_px / marker_y_px default -- docs/map-render-spec.md,
// "Marker freedom is implemented in mapbuilder". The viewport re-anchors on
// the marker at this screen position on every reset, so this harness treats
// the requested lat/lon as both the anchor and the marker's own fix.
constexpr int16_t kAnchorScreenX = 230;
constexpr int16_t kAnchorScreenY = 620;

// Label overhang margin for the geometry bbox -- docs/map-data-spec.md,
// "Which tiles to load".
constexpr double kMarginPx = 64.0;

struct LodStep {
  double mpp;
  uint8_t z;
};

// docs/map-data-spec.md, "Zoom is a hardware button, so zoom is a ladder".
constexpr LodStep kZoomLadder[5] = {
    {3.0, 13},   // step 0, detail
    {5.0, 13},   // step 1, detail
    {7.5, 12},   // step 2, regional
    {11.0, 11},  // step 3, overview
    {15.0, 11},  // step 4, overview
};

long fileSizeBytes(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return -1;
  return static_cast<long>(st.st_size);
}

std::string tilePath(const std::string& tilesDir, uint8_t z, uint32_t col, uint32_t row) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/base/%u/%u/%u.tib", z, col, row);
  return tilesDir + buf;
}

// docs/map-data-spec.md, "Which tiles to load": rotate the viewport rect by
// heading, take its axis-aligned Mercator bbox, inflate by the label
// margin, then map to the tile grid at this LOD's zoom.
void tileRangeForViewport(const MapProjection& proj, uint8_t z, uint32_t& col0, uint32_t& row0, uint32_t& col1,
                          uint32_t& row1) {
  double minX = 0, minY = 0, maxX = 0, maxY = 0;
  const int corners[4][2] = {{0, 0}, {SCREEN_WIDTH, 0}, {0, SCREEN_HEIGHT}, {SCREEN_WIDTH, SCREEN_HEIGHT}};
  for (int i = 0; i < 4; ++i) {
    double mx, my;
    proj.screenToMerc(static_cast<int16_t>(corners[i][0]), static_cast<int16_t>(corners[i][1]), mx, my);
    if (i == 0) {
      minX = maxX = mx;
      minY = maxY = my;
    } else {
      minX = std::min(minX, mx);
      maxX = std::max(maxX, mx);
      minY = std::min(minY, my);
      maxY = std::max(maxY, my);
    }
  }
  const double marginMerc = kMarginPx * proj.mppMerc();
  minX -= marginMerc;
  maxX += marginMerc;
  minY -= marginMerc;
  maxY += marginMerc;

  uint32_t cA, rA, cB, rB;
  MapTileGrid::mercToTileColRow(minX, maxY, z, cA, rA);  // NW corner
  MapTileGrid::mercToTileColRow(maxX, minY, z, cB, rB);  // SE corner
  col0 = std::min(cA, cB);
  col1 = std::max(cA, cB);
  row0 = std::min(rA, rB);
  row1 = std::max(rA, rB);
}

// Reads the roads and places layers of one tile into `state`, projecting
// every point through `proj`. Water, buildings and junctions are never
// opened -- the layer directory lets us seek past them for free
// (docs/prototype-plan.md, P2: "Read only what you draw").
bool loadTileRoadsAndPlaces(const std::string& path, const MapProjection& proj, MapViewState& state) {
  StdioFileSource file;
  MapTileReader reader;
  if (!reader.open(file, path.c_str())) {
    std::fprintf(stderr, "skip %s: failed to open or crc32 mismatch\n", path.c_str());
    return false;
  }

  if (reader.hasLayer(MapTileReader::Layer::Roads) && reader.beginLayer(MapTileReader::Layer::Roads)) {
    MapTileReader::WayHeader wh;
    int16_t xs[MapTileReader::kMaxWayPoints];
    int16_t ys[MapTileReader::kMaxWayPoints];
    while (reader.readWayHeader(wh)) {
      if (wh.pointCount > MapTileReader::kMaxWayPoints || !reader.readWayPoints(xs, ys, wh.pointCount)) {
        std::fprintf(stderr, "abort %s: malformed roads layer\n", path.c_str());
        break;
      }
      MapWay way;
      way.classId = wh.classId;
      way.roughness = wh.roughness;
      way.flags = wh.flags;
      way.points.reserve(wh.pointCount);
      for (uint16_t i = 0; i < wh.pointCount; ++i) {
        int16_t sx, sy;
        proj.projectTileLocal(reader.originX(), reader.originY(), xs[i], ys[i], sx, sy);
        way.points.emplace_back(sx, sy);
      }
      state.ways.push_back(std::move(way));
    }
  }

  if (reader.hasLayer(MapTileReader::Layer::Places) && reader.beginLayer(MapTileReader::Layer::Places)) {
    MapTileReader::PlaceHeader ph;
    char name[64];
    while (reader.readPlaceHeader(ph)) {
      if (!reader.readPlaceName(ph, name, sizeof(name))) {
        std::fprintf(stderr, "abort %s: malformed places layer\n", path.c_str());
        break;
      }
      int16_t sx, sy;
      proj.projectTileLocal(reader.originX(), reader.originY(), ph.x, ph.y, sx, sy);
      state.placeDots.emplace_back(sx, sy);
    }
  }

  reader.close();
  return true;
}

}  // namespace

MapPreviewResult buildMapPreview(const std::string& tilesDir, double lat, double lon, uint8_t heading, int zoom) {
  MapPreviewResult result;
  const LodStep& lod = kZoomLadder[zoom];
  result.lodZoom = lod.z;

  const double mppMerc = lod.mpp / std::cos(lat * 3.14159265358979323846 / 180.0);

  MapProjection proj;
  proj.reset(lat, lon, kAnchorScreenX, kAnchorScreenY, heading, mppMerc);

  uint32_t col0, row0, col1, row1;
  tileRangeForViewport(proj, lod.z, col0, row0, col1, row1);
  const uint32_t tileCount = (col1 - col0 + 1) * (row1 - row0 + 1);
  if (tileCount > 9) {
    std::fprintf(stderr, "warning: %u tiles needed (col %u..%u, row %u..%u) -- more than the 3x3 worst case\n",
                 tileCount, col0, col1, row0, row1);
  }

  result.state.markerX = kAnchorScreenX;
  result.state.markerY = kAnchorScreenY;
  result.state.heading = static_cast<MapHeading>(heading);

  for (uint32_t col = col0; col <= col1; ++col) {
    for (uint32_t row = row0; row <= row1; ++row) {
      const std::string path = tilePath(tilesDir, lod.z, col, row);
      const long sz = fileSizeBytes(path);
      if (sz < 0) {
        ++result.tilesMissing;
        continue;
      }
      if (loadTileRoadsAndPlaces(path, proj, result.state)) {
        ++result.tilesLoaded;
        if (result.smallestTileBytes < 0 || sz < result.smallestTileBytes) result.smallestTileBytes = sz;
        if (sz > result.largestTileBytes) result.largestTileBytes = sz;
      }
    }
  }

  return result;
}
