// Native preview for MapRenderer: builds a mock MapViewState, renders it
// through PpmCanvas, and dumps a PPM image. No PlatformIO/ESP32 toolchain
// involved -- see docs/firmware-implementation-plan.md Phase 1 in the
// parent xteink repo.
//
// Usage: map_preview [output.ppm]  (defaults to map_preview.ppm in cwd)

#include <cstdio>

#include "MapRenderer.h"
#include "MockMapData.h"
#include "PpmCanvas.h"

namespace {

// Matches the real panel's logical Portrait resolution (FreeInkDisplay::
// DISPLAY_WIDTH/HEIGHT is 800x480 landscape; Portrait swaps to 480x800).
constexpr int SCREEN_WIDTH = 480;
constexpr int SCREEN_HEIGHT = 800;

}  // namespace

int main(int argc, char** argv) {
  const std::string outputPath = argc > 1 ? argv[1] : "map_preview.ppm";

  PpmCanvas canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
  // Same mock data MapActivity::onEnter() draws -- this preview always
  // shows exactly what the real firmware's Map screen will show.
  const MapViewState state = buildMockMapViewState(SCREEN_WIDTH, SCREEN_HEIGHT);
  MapRenderer::render(canvas, state);

  if (!canvas.writePpm(outputPath)) {
    std::fprintf(stderr, "Failed to write %s\n", outputPath.c_str());
    return 1;
  }
  std::printf("Wrote %s (%dx%d)\n", outputPath.c_str(), SCREEN_WIDTH, SCREEN_HEIGHT);
  return 0;
}
