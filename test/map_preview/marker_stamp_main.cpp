// Stamps the real MapRenderer::drawMarker() glyph onto an already-rendered
// PPM, at an arbitrary position and heading -- for turning a held background
// (rendered once at a real redraw) into one video frame per packet without
// re-running the tile/geometry pass for every one of them.
//
// Usage:
//   marker_stamp --in <bg.ppm> --out <frame.ppm> --x <px> --y <px> --heading 0-15
//
// This is the same glyph map_preview itself draws at the anchor for a fresh
// frame (MapRenderer.cpp's drawMarker() -- puck + arrow, MapStyleDefaults.h's
// numbers) -- not an approximation invented for this tool, and not what a
// real ride-mode marker looks like either: MapRenderer.cpp:342-351 says the
// device draws its own mode-specific marker (ring + dot/arrow, sized per
// hike/cycle/ride) in MapActivity, separately, after this call returns.
// map_preview already shows this "puck" in place of that for every frame it
// renders (there is no host build of MapActivity to call instead), so using
// it here too keeps every frame in a video visually consistent with the
// others -- right by the same measure the rest of this tool already is,
// not by being pixel-identical to a real ride's marker.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MapRenderer.h"
#include "MapStyleDefaults.h"
#include "PpmCanvas.h"

namespace {
constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 800;
}  // namespace

int main(int argc, char** argv) {
  std::string inPath, outPath;
  int x = 0, y = 0, heading = 0;
  bool haveIn = false, haveOut = false, haveX = false, haveY = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (arg == "--in") {
      const char* v = next();
      if (!v) return 2;
      inPath = v;
      haveIn = true;
    } else if (arg == "--out") {
      const char* v = next();
      if (!v) return 2;
      outPath = v;
      haveOut = true;
    } else if (arg == "--x") {
      const char* v = next();
      if (!v) return 2;
      x = std::atoi(v);
      haveX = true;
    } else if (arg == "--y") {
      const char* v = next();
      if (!v) return 2;
      y = std::atoi(v);
      haveY = true;
    } else if (arg == "--heading") {
      const char* v = next();
      if (!v) return 2;
      heading = std::atoi(v) & 0x0F;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return 2;
    }
  }

  if (!haveIn || !haveOut || !haveX || !haveY) {
    std::fprintf(stderr, "usage: marker_stamp --in <bg.ppm> --out <frame.ppm> --x <px> --y <px> --heading 0-15\n");
    return 2;
  }

  PpmCanvas canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
  if (!canvas.readPpm(inPath)) {
    std::fprintf(stderr, "marker_stamp: could not read %s as a %dx%d P6 PPM\n", inPath.c_str(), SCREEN_WIDTH,
                 SCREEN_HEIGHT);
    return 1;
  }

  MapRenderer::drawMarker(canvas, static_cast<int16_t>(x), static_cast<int16_t>(y),
                           static_cast<MapHeading>(heading), kDefaultMapStyle);

  if (!canvas.writePpm(outPath)) {
    std::fprintf(stderr, "marker_stamp: could not write %s\n", outPath.c_str());
    return 1;
  }
  return 0;
}
