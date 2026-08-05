// Native preview for MapRenderer: streams real .tib tiles around a
// coordinate through the renderer and dumps a PPM image. No PlatformIO/ESP32
// toolchain involved -- see docs/prototype-plan.md, P2 and P2.5.
//
// Usage:
//   map_preview --tiles <dir> --lat <d> --lon <d> [--heading 0-15]
//               [--zoom 0-4] [--marker 0-4] [--mode ride|hike|cycle]
//               [--tile <col>/<row>] [--hatch] [--out <file>]
//   map_preview --zoom-ladder
//
// <dir> is a mapbuilder-produced SD root (mapbuilder/build_tiles.py), i.e.
// it contains base/<z>/<col>/<row>.tib. Loading/projection logic lives in
// MapPreviewPipeline, shared with the golden-file test in
// test/map_tile_reader/ so both render through the identical path.
//
// --tile pins the render to exactly one tile at the zoom step's LOD instead
// of the whole range the viewport touches. It exists to quote a per-tile RAM
// number against the pre-P2.5 pipeline's per-tile cost, not for real
// previews.
//
// --hatch draws the missing-tile hatch the device always draws (P4). It is
// opt-in here so the committed golden PPM stays byte-identical.
//
// --marker moves the marker down the marker-height ladder (P5); left off, the
// marker sits at the compiled style's device.marker_y_px. --mode applies the
// built-in class mask for that travel
// mode, so the same coordinate can be diffed between ride and hike on the
// laptop before anything is flashed.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MapModeMask.h"
#include "MapPreviewPipeline.h"
#include "MapViewport.h"
#include "PpmCanvas.h"

namespace {

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 800;

bool parseArgs(int argc, char** argv, MapPreviewRequest& request, std::string& outPath) {
  bool haveLat = false, haveLon = false, haveTiles = false;
  // -1 means "not given", which keeps the compiled style's marker_y_px rather
  // than snapping to the nearest ladder rung.
  int markerStep = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (arg == "--tiles") {
      const char* v = next();
      if (!v) return false;
      request.tilesDir = v;
      haveTiles = true;
    } else if (arg == "--lat") {
      const char* v = next();
      if (!v) return false;
      request.lat = std::atof(v);
      haveLat = true;
    } else if (arg == "--lon") {
      const char* v = next();
      if (!v) return false;
      request.lon = std::atof(v);
      haveLon = true;
    } else if (arg == "--heading") {
      const char* v = next();
      if (!v) return false;
      request.heading = static_cast<uint8_t>(std::atoi(v) % 16);
    } else if (arg == "--zoom") {
      const char* v = next();
      if (!v) return false;
      request.zoom = std::atoi(v);
    } else if (arg == "--tile") {
      const char* v = next();
      if (!v) return false;
      unsigned col = 0, row = 0;
      if (std::sscanf(v, "%u/%u", &col, &row) != 2) {
        std::fprintf(stderr, "--tile wants <col>/<row>\n");
        return false;
      }
      request.singleTile = true;
      request.tileCol = col;
      request.tileRow = row;
    } else if (arg == "--marker") {
      const char* v = next();
      if (!v) return false;
      markerStep = std::atoi(v);
    } else if (arg == "--mode") {
      const char* v = next();
      if (!v) return false;
      MapRideMode mode;
      if (!mapRideModeFromName(v, mode)) {
        std::fprintf(stderr, "--mode wants ride, hike or cycle\n");
        return false;
      }
      request.classMask = MapModeMasks{}.forMode(mode);
    } else if (arg == "--hatch") {
      request.drawHatch = true;
    } else if (arg == "--zoom-ladder") {
      // Handled in main() before anything else; accepted here so it does not
      // trip the unknown-argument branch.
      continue;
    } else if (arg == "--out") {
      const char* v = next();
      if (!v) return false;
      outPath = v;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return false;
    }
  }
  if (markerStep >= 0) {
    if (markerStep >= MapViewport::kMarkerStepCount) {
      std::fprintf(stderr, "--marker must be 0-%d\n", MapViewport::kMarkerStepCount - 1);
      return false;
    }
    request.markerY = MapViewport::markerYForStep(markerStep);
  }
  return haveTiles && haveLat && haveLon;
}

}  // namespace

int main(int argc, char** argv) {
  // --zoom-ladder: print the compiled zoom ladder and exit. A caller that has
  // an m/px figure (mapbuilder's webapp) has to pick a rung, and the ladder
  // lives in MapViewport.h. Printing it from the binary that compiled it beats
  // a second copy of the numbers on the laptop side, which would drift.
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--zoom-ladder") != 0) continue;
    for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
      std::printf("%d %.1f %u\n", step, MapViewport::kZoomLadder[step].mpp, MapViewport::kZoomLadder[step].z);
    }
    return 0;
  }

  MapPreviewRequest request;
  std::string outPath = "map_preview.ppm";
  if (!parseArgs(argc, argv, request, outPath)) {
    std::fprintf(stderr,
                 "usage: map_preview --tiles <dir> --lat <d> --lon <d> "
                 "[--heading 0-15] [--zoom 0-4] [--marker 0-4] [--mode ride|hike|cycle] "
                 "[--tile <col>/<row>] [--hatch] [--out <file>]\n"
                 "       map_preview --zoom-ladder\n");
    return 1;
  }
  if (request.zoom < 0 || request.zoom > 4) {
    std::fprintf(stderr, "zoom must be 0-4\n");
    return 1;
  }

  PpmCanvas canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
  const MapPreviewResult preview = renderMapPreview(request, canvas);

  std::printf("z%u col %u..%u row %u..%u: loaded %d tiles (%d missing, mask 0x%x), %u ways, %u places\n",
              preview.lodZoom, preview.col0, preview.col1, preview.row0, preview.row1, preview.tilesLoaded,
              preview.tilesMissing, preview.missingMask, preview.waysDrawn, preview.placesDrawn);
  std::printf("marker y=%d, class mask 0x%08x, %u ways dropped by it\n", preview.markerY, request.classMask,
              preview.waysFiltered);
  std::printf("tile size on disk: %ld..%ld bytes\n", preview.smallestTileBytes, preview.largestTileBytes);
  std::printf("peak RAM: %zu B resident source + %zu B heap during render (%zu allocations) = %zu B total\n",
              preview.sourceBytes, preview.peakHeapDuringRender, preview.allocsDuringRender,
              preview.sourceBytes + preview.peakHeapDuringRender);

  if (!canvas.writePpm(outPath)) {
    std::fprintf(stderr, "Failed to write %s\n", outPath.c_str());
    return 1;
  }
  std::printf("Wrote %s (%dx%d)\n", outPath.c_str(), SCREEN_WIDTH, SCREEN_HEIGHT);
  return 0;
}
