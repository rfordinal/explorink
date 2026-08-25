// Native preview for MapRenderer: streams real .tib tiles around a
// coordinate through the renderer and dumps a PPM image. No PlatformIO/ESP32
// toolchain involved -- see docs/prototype-plan.md, P2 and P2.5.
//
// Usage:
//   map_preview --tiles <dir> --lat <d> --lon <d> [--heading 0-15]
//               [--zoom 0-4] [--marker 0-4] [--mode ride|hike|cycle]
//               [--tile <col>/<row>] [--hatch] [--route <file.tir>]
//               [--no-points] [--no-labels] [--point-categories water,hut,...]
//               [--fit-route] [--no-marker] [--out <file>]
//   map_preview --tiles <dir> --route <file.tir> --fit-route
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
// --no-marker skips the puck this tool normally draws in place of a real
// mode marker (MapPreviewPipeline.h's MapPreviewRequest::drawMarker). For a
// consumer that draws its own marker afterward at a position/heading this
// call has no way to know -- see marker_stamp in this same directory.
//
// --route draws a .tir route file over the tiles
// (../../../docs/route-file-spec.md in the parent xteink repo). --fit-route then
// frames the whole route the way the device does when the rider picks one:
// MapRouteFit chooses the rung, the heading and the anchor, and --lat/--lon are
// not needed at all. That is what makes route styling a two-second laptop edit
// instead of a flash.
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
#include "MapPointTypes.h"
#include "MapPreviewPipeline.h"
#include "MapViewport.h"
#include "PpmCanvas.h"

namespace {

constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 800;

bool parseArgs(int argc, char** argv, MapPreviewRequest& request, std::string& outPath) {
  bool haveLat = false, haveLon = false, haveTiles = false;
  // --mode and --zoom together pick the class mask, and --mode may arrive
  // first, so the mask cannot be built while the arguments are still coming in.
  // Resolved once at the end, when both are known.
  bool modeGiven = false;
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
      // The mask is picked with the rung, so --mode must not be read before
      // --zoom. Store the mode and let the pipeline resolve both together.
      request.mode = mode;
      modeGiven = true;
    } else if (arg == "--no-points") {
      request.drawPoints = false;
    } else if (arg == "--no-labels") {
      request.drawLabels = false;
    } else if (arg == "--point-categories") {
      // A comma-separated list of safety category names or ids, which is what
      // `Nearby -> Show on map` does with one of them. Anything unknown is a
      // usage error rather than a silently empty layer.
      const std::string v = next();
      if (v.empty()) return false;
      uint16_t mask = 0;
      size_t start = 0;
      while (start <= v.size()) {
        const size_t comma = v.find(',', start);
        const std::string token = v.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!token.empty()) {
          int id = -1;
          for (uint8_t c = 0; c < kSafetyCategoryCount; ++c) {
            if (kSafetyCategoryNames[c] != nullptr && token == kSafetyCategoryNames[c]) id = c;
          }
          if (id < 0) {
            char* end = nullptr;
            const long parsed = std::strtol(token.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && parsed >= 0 && parsed < kSafetyCategoryCount) {
              id = static_cast<int>(parsed);
            }
          }
          if (id < 0) {
            std::fprintf(stderr, "unknown safety category: %s\n", token.c_str());
            return false;
          }
          mask |= static_cast<uint16_t>(1u << id);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
      request.pointCategoryMask = mask;
    } else if (arg == "--route") {
      const char* v = next();
      if (!v) return false;
      request.routePath = v;
    } else if (arg == "--fit-route") {
      request.fitRoute = true;
    } else if (arg == "--hatch") {
      request.drawHatch = true;
    } else if (arg == "--no-marker") {
      request.drawMarker = false;
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
  // Both known now. The device does exactly this lookup per frame
  // (MapModeMasks::forMode), so a run with --mode reads off the card precisely
  // the classes that mode draws at that rung -- and nothing the style hides
  // there.
  if (modeGiven) request.classMask = MapModeMasks{}.forMode(request.mode, request.zoom);
  // --fit-route takes the anchor off the route, so a coordinate is not only
  // unnecessary there, asking for one invites a mismatch between the two.
  if (request.fitRoute && !request.routePath.empty()) return haveTiles;
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
                 "[--tile <col>/<row>] [--hatch] [--route <file.tir>] [--fit-route] [--out <file>]\n"
                 "       map_preview --tiles <dir> --route <file.tir> --fit-route\n"
                 "       map_preview --zoom-ladder\n");
    return 1;
  }
  if (request.zoom < 0 || request.zoom >= MapViewport::kZoomStepCount) {
    std::fprintf(stderr, "zoom must be 0-%d\n", MapViewport::kZoomStepCount - 1);
    return 1;
  }

  PpmCanvas canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
  const MapPreviewResult preview = renderMapPreview(request, canvas);

  std::printf("z%u col %u..%u row %u..%u: loaded %d tiles (%d missing, mask 0x%x), %u ways, %u places\n",
              preview.lodZoom, preview.col0, preview.col1, preview.row0, preview.row1, preview.tilesLoaded,
              preview.tilesMissing, preview.missingMask, preview.waysDrawn, preview.placesDrawn);
  std::printf("marker y=%d, class mask 0x%08x, %u ways dropped by it\n", preview.markerY, request.classMask,
              preview.waysFiltered);
  std::printf("place labels: %u drawn, %u dropped (no room)\n", preview.labelsPlaced, preview.labelsDropped);
  std::printf("tile size on disk: %ld..%ld bytes, %u bytes actually read\n", preview.smallestTileBytes,
              preview.largestTileBytes, preview.bytesRead);
  if (request.drawPoints) {
    std::printf("points: %u drawn, %u shard(s) opened, %u missing, %u corrupt, %u bytes read\n", preview.pointsDrawn,
                preview.pointShardsOpened, preview.pointShardsMissing, preview.pointShardsCorrupt,
                preview.pointBytesRead);
  }
  if (!request.routePath.empty()) {
    if (preview.routeLoaded) {
      std::printf("route \"%s\": %u points, %u bytes read\n", preview.routeName.c_str(), preview.routePoints,
                  preview.routeBytesRead);
    } else {
      std::printf("route %s: REFUSED, nothing drawn\n", request.routePath.c_str());
    }
  }
  if (preview.routeFitRan) {
    std::printf("fit: heading %u, zoom step %u (%.0f m/px), anchor %.5f,%.5f -- %s\n", preview.routeFitHeading,
                preview.routeFitZoomStep, MapViewport::kZoomLadder[preview.routeFitZoomStep].mpp, preview.routeFitLat,
                preview.routeFitLon,
                preview.routeFits ? "whole route on screen" : "too long for the ladder, showing the middle");
    if (!preview.routeFits) {
      std::printf("     needed %.1f m/px, the coarsest rung is %.0f\n", preview.routeFitRequiredMpp,
                  MapViewport::kZoomLadder[MapViewport::kZoomStepCount - 1].mpp);
    }
  }
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
