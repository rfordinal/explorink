// Native preview for MapRenderer: loads real .tib tiles around a coordinate,
// projects roads and places to screen space, and dumps a PPM image. No
// PlatformIO/ESP32 toolchain involved -- see docs/prototype-plan.md, P2.
//
// Usage:
//   map_preview --tiles <dir> --lat <d> --lon <d> [--heading 0-15]
//               [--zoom 0-4] [--out <file>]
//
// <dir> is a mapbuilder-produced SD root (mapbuilder/build_tiles.py), i.e.
// it contains base/<z>/<col>/<row>.tib. Loading/projection logic lives in
// MapPreviewPipeline, shared with the golden-file test in
// test/map_tile_reader/ so both render through the identical path.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "MapPreviewPipeline.h"
#include "MapTileReader.h"
#include "PpmCanvas.h"

namespace {

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 800;

struct Args {
  std::string tilesDir;
  double lat = 0.0;
  double lon = 0.0;
  uint8_t heading = 0;
  int zoom = 0;
  std::string out = "map_preview.ppm";
};

bool parseArgs(int argc, char** argv, Args& out) {
  bool haveLat = false, haveLon = false, haveTiles = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (arg == "--tiles") {
      const char* v = next();
      if (!v) return false;
      out.tilesDir = v;
      haveTiles = true;
    } else if (arg == "--lat") {
      const char* v = next();
      if (!v) return false;
      out.lat = std::atof(v);
      haveLat = true;
    } else if (arg == "--lon") {
      const char* v = next();
      if (!v) return false;
      out.lon = std::atof(v);
      haveLon = true;
    } else if (arg == "--heading") {
      const char* v = next();
      if (!v) return false;
      out.heading = static_cast<uint8_t>(std::atoi(v) % 16);
    } else if (arg == "--zoom") {
      const char* v = next();
      if (!v) return false;
      out.zoom = std::atoi(v);
    } else if (arg == "--out") {
      const char* v = next();
      if (!v) return false;
      out.out = v;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return false;
    }
  }
  return haveTiles && haveLat && haveLon;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parseArgs(argc, argv, args)) {
    std::fprintf(stderr,
                 "usage: map_preview --tiles <dir> --lat <d> --lon <d> "
                 "[--heading 0-15] [--zoom 0-4] [--out <file>]\n");
    return 1;
  }
  if (args.zoom < 0 || args.zoom > 4) {
    std::fprintf(stderr, "zoom must be 0-4\n");
    return 1;
  }

  const MapPreviewResult preview = buildMapPreview(args.tilesDir, args.lat, args.lon, args.heading, args.zoom);

  std::printf("loaded %d tiles (%d missing) at z%u, %zu ways, %zu places\n", preview.tilesLoaded,
              preview.tilesMissing, preview.lodZoom, preview.state.ways.size(), preview.state.placeDots.size());
  if (preview.tilesLoaded > 0) {
    // The O(1) claim: MapTileReader::peakBufferBytes() is a compile-time
    // constant, so it is identical here whether the smallest or the largest
    // tile in this run was read.
    std::printf(
        "tile size on disk: %ld..%ld bytes -- reader stream buffer stayed at %zu bytes regardless (O(1) in tile "
        "size)\n",
        preview.smallestTileBytes, preview.largestTileBytes, MapTileReader::peakBufferBytes());
  }

  PpmCanvas canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
  MapRenderer::render(canvas, preview.state);

  if (!canvas.writePpm(args.out)) {
    std::fprintf(stderr, "Failed to write %s\n", args.out.c_str());
    return 1;
  }
  std::printf("Wrote %s (%dx%d)\n", args.out.c_str(), SCREEN_WIDTH, SCREEN_HEIGHT);
  return 0;
}
