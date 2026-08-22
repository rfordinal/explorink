// The device window: a recorded ride playing live, at 480x800, in what the
// panel would actually have shown.
//
// Two host binaries already existed and answered half of this each:
// `map_replay` walks a ride through the real `MapFollow::decide()` and prints
// totals; `tools/render_ride_video.py` (parent repo) renders those decisions
// into an MP4 -- but only the full redraws, "Scope cut, deliberate: only
// ReAnchor frames are rendered". So neither one lets you sit and watch the
// marker crawl and see a partial refresh happen.
//
// This joins them, live:
//
//   RideLog  ->  ReplayEngine::Stepper  ->  MapFollow::decide()   (the decision)
//                                       ->  renderMapPreview()    (the picture)
//                                       ->  MapRenderer::drawMarker()
//                                       ->  SDL2 window
//
// Nothing about the decisions or the drawing is reimplemented here. The stepper
// is the same one `map_replay --check` gates against three real X4 rides, and
// the renderer is the firmware's own.
//
//   map_window --ride docs/rides/<ride>.jsonl --tiles mapbuilder/cdn
//
// Keys: space pause, +/- speed, `,` `.` step, r restart, d dirty rect, q quit.
//
// What this is NOT: a device. It is x86, so nothing here says what a redraw
// costs on a 160 MHz C3, and there is no heap ceiling, no waveform and no
// ghosting. See docs/virtual-device.md for what only an emulated C3 or the real
// panel can answer.
#include <SDL2/SDL.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "MapMarkerMetrics.h"
#include "MapPreviewPipeline.h"
#include "MapRenderer.h"
#include "MapStyleDefaults.h"
#include "MapViewport.h"
#include "PpmCanvas.h"
#include "ReplayEngine.h"
#include "RideLog.h"

namespace {

constexpr int kPanelW = 480;
constexpr int kPanelH = 800;
constexpr int kSidebarW = 300;
constexpr int kWindowW = kPanelW + kSidebarW;
constexpr int kWindowH = kPanelH;

// Sidebar chrome, drawn over the blitted pixels rather than into either canvas:
// the panel half of this window must stay exactly the 1-bit picture the device
// would hold, so an overlay that helps a human read it does not get to write
// into it.
constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kRectInk = 0xFFE04030u;  // dirty-rect outline
constexpr uint32_t kSeparator = 0xFFB0B0B0u;

// What one refresh costs the panel, measured on the X4 2026-08-05 and recorded
// in docs/map-follow.md:250-251: a windowed refresh and a full one both spend
// 500 ms in the waveform, and the windowed one costs that "whatever its area"
// (MapMarkerMetrics.h). So refresh count times this is a real number, and it is
// the currency this window exists to let someone reduce.
//
// Deliberately NOT modelled here: the render half. On the device that is
// 400-1,750 ms depending on the rung and how dense the area is, and ~8,300 ms
// in Bratislava (docs/map-follow.md:364-374) -- a single constant would be
// wrong nearly everywhere. The sidebar shows this host's own render time
// instead, labelled as such.
constexpr double kPanelRefreshMs = 500.0;

// Wall-clock gap to use between two packets when the log carries no timestamp
// (RideLog::Packet::tUtcMs == 0). The phone's bridge sends about one packet a
// second.
constexpr int64_t kFallbackPacketGapMs = 1000;

struct Options {
  std::string ridePath;
  std::string tilesDir;
  int zoomStep = 4;    // MapViewport::kZoomLadder index; 4 = 20 m/px
  int markerStep = 2;  // what tools/replay_ride.py sends
  double speed = 1.0;
  bool showRect = true;
  // Quit when the last packet has been stepped instead of holding the final
  // frame on screen. For scripted runs -- the summary this prints on exit is
  // what gets compared against `map_replay` on the same ride.
  bool exitAtEnd = false;
};

bool parseArgs(int argc, char** argv, Options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (arg == "--ride") {
      const char* v = next();
      if (!v) return false;
      out.ridePath = v;
    } else if (arg == "--tiles") {
      const char* v = next();
      if (!v) return false;
      out.tilesDir = v;
    } else if (arg == "--zoom") {
      const char* v = next();
      if (!v) return false;
      out.zoomStep = std::atoi(v);
    } else if (arg == "--marker") {
      const char* v = next();
      if (!v) return false;
      out.markerStep = std::atoi(v);
    } else if (arg == "--speed") {
      const char* v = next();
      if (!v) return false;
      out.speed = std::atof(v);
    } else if (arg == "--no-rect") {
      out.showRect = false;
    } else if (arg == "--exit-at-end") {
      out.exitAtEnd = true;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return false;
    }
  }
  return !out.ridePath.empty() && !out.tilesDir.empty();
}

// One text row in the sidebar, using the firmware's own font tables through
// PpmCanvas -- no SDL_ttf, and the same rasterizer the map labels use.
int sidebarLine(PpmCanvas& canvas, int y, const char* text, int sizePx = 20, bool bold = false) {
  int w = 0, h = 0;
  if (!canvas.measureText(text, sizePx, bold, w, h)) return y;
  canvas.drawText(12, y, text, sizePx, bold, MapInk::Black);
  return y + h + 4;
}

void blitCanvas(const PpmCanvas& canvas, int canvasW, int canvasH, uint32_t* out, int outStride, int originX) {
  const std::vector<uint8_t>& pixels = canvas.pixels();
  for (int y = 0; y < canvasH; ++y) {
    uint32_t* row = out + static_cast<size_t>(y) * outStride + originX;
    const uint8_t* src = pixels.data() + static_cast<size_t>(y) * canvasW;
    for (int x = 0; x < canvasW; ++x) row[x] = src[x] ? kBlack : kWhite;
  }
}

void strokeRect(uint32_t* out, int outStride, int x, int y, int w, int h, uint32_t ink) {
  const auto put = [&](int px, int py) {
    if (px < 0 || py < 0 || px >= kPanelW || py >= kPanelH) return;
    out[static_cast<size_t>(py) * outStride + px] = ink;
  };
  for (int i = 0; i < w; ++i) {
    put(x + i, y);
    put(x + i, y + h - 1);
  }
  for (int i = 0; i < h; ++i) {
    put(x, y + i);
    put(x + w - 1, y + i);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseArgs(argc, argv, options)) {
    std::fprintf(stderr,
                 "usage: map_window --ride <ride.jsonl> --tiles <dir> [--zoom 0-%d] [--marker 0-4]\n"
                 "                  [--speed X] [--no-rect] [--exit-at-end]\n",
                 MapViewport::kZoomStepCount - 1);
    return 2;
  }
  if (options.zoomStep < 0 || options.zoomStep >= MapViewport::kZoomStepCount) {
    std::fprintf(stderr, "zoom must be 0-%d\n", MapViewport::kZoomStepCount - 1);
    return 2;
  }

  RideLog::Ride ride;
  int skippedLines = 0;
  if (!RideLog::read(options.ridePath, ride, skippedLines)) {
    std::fprintf(stderr, "map_window: cannot open %s\n", options.ridePath.c_str());
    return 1;
  }
  if (ride.packets.empty()) {
    std::fprintf(stderr, "map_window: %s has no usable packets\n", options.ridePath.c_str());
    return 1;
  }
  std::printf("%s: %zu packets, %d malformed lines skipped\n", ride.name.c_str(), ride.packets.size(), skippedLines);

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("ExplorInk device window", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        kWindowW, kWindowH, SDL_WINDOW_SHOWN);
  // Software fallback matters for a headless run (SDL_VIDEODRIVER=dummy), which
  // is how this gets checked against map_replay's numbers without a display.
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr;
  if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  SDL_Texture* texture =
      renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kWindowW, kWindowH)
               : nullptr;
  if (!window || !renderer || !texture) {
    std::fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // The marker's patch box at this rung -- the real dirty rectangle a marker
  // move refreshes, not a round number (MapMarkerMetrics.h: box = ring + 2 *
  // halo, 64 px at full scale).
  const int markerBox = markerMetricsFor(MapViewport::kZoomLadder[options.zoomStep].markerScale8).box;

  ReplayEngine::Config config;
  config.zoomStep = options.zoomStep;
  config.markerStep = options.markerStep;
  config.screenWidth = kPanelW;
  config.screenHeight = kPanelH;

  PpmCanvas background(kPanelW, kPanelH);  // the held frame, no marker on it
  PpmCanvas frame(kPanelW, kPanelH);       // background + the marker where it is now
  PpmCanvas sidebar(kSidebarW, kPanelH);
  std::vector<uint32_t> pixels(static_cast<size_t>(kWindowW) * kWindowH, kWhite);

  bool running = true;
  bool paused = false;
  bool stepOnce = false;
  bool showRect = options.showRect;
  double speed = options.speed;

  // Rewindable playback state, all reset together by `r`.
  ReplayEngine::Stepper stepper(config);
  size_t nextPacket = 0;
  int fullRefreshes = 0, partialRefreshes = 0, skips = 0;
  int64_t rideStartMs = 0, rideNowMs = 0;
  std::string lastAction = "-", lastReason;
  int rectX = 0, rectY = 0, rectW = 0, rectH = 0;
  bool haveRect = false;
  double lastRenderMs = 0.0;
  MapPreviewResult lastPreview;
  uint64_t dueTicks = SDL_GetTicks64();

  const auto restart = [&]() {
    stepper = ReplayEngine::Stepper(config);
    nextPacket = 0;
    fullRefreshes = partialRefreshes = skips = 0;
    rideStartMs = rideNowMs = 0;
    lastAction = "-";
    lastReason.clear();
    haveRect = false;
    lastRenderMs = 0.0;
    lastPreview = MapPreviewResult{};
    dueTicks = SDL_GetTicks64();
  };

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
      if (event.type != SDL_KEYDOWN) continue;
      switch (event.key.keysym.sym) {
        case SDLK_q:
        case SDLK_ESCAPE: running = false; break;
        case SDLK_SPACE: paused = !paused; break;
        case SDLK_PERIOD: stepOnce = true; break;
        case SDLK_COMMA: stepOnce = true; break;  // no rewind: the state machine only goes forward
        case SDLK_PLUS:
        case SDLK_EQUALS: speed = speed >= 64.0 ? 64.0 : speed * 2.0; break;
        case SDLK_MINUS: speed = speed <= 0.125 ? 0.125 : speed / 2.0; break;
        case SDLK_d: showRect = !showRect; break;
        case SDLK_r: restart(); break;
        default: break;
      }
    }

    const uint64_t now = SDL_GetTicks64();
    const bool due = !paused && now >= dueTicks;
    if ((due || stepOnce) && nextPacket < ride.packets.size()) {
      const RideLog::Packet& packet = ride.packets[nextPacket];
      if (nextPacket == 0) rideStartMs = packet.tUtcMs;
      rideNowMs = packet.tUtcMs ? packet.tUtcMs - rideStartMs : rideNowMs + kFallbackPacketGapMs;

      const ReplayEngine::Stepper::Step step = stepper.step(packet);

      if (step.frameChanged) {
        MapPreviewRequest request;
        request.tilesDir = options.tilesDir;
        request.lat = step.frameLat;
        request.lon = step.frameLon;
        request.heading = step.frameHeadingStep;
        request.zoom = options.zoomStep;
        request.markerY = step.y;
        // The marker is stamped separately, once per packet, exactly the way
        // marker_stamp does it for the video tool -- with this left on the held
        // background carries a puck baked in at the anchor and every later
        // packet's frame would show two.
        request.drawMarker = false;
        request.drawHatch = true;

        const uint64_t t0 = SDL_GetPerformanceCounter();
        background = PpmCanvas(kPanelW, kPanelH);
        lastPreview = renderMapPreview(request, background);
        const uint64_t t1 = SDL_GetPerformanceCounter();
        lastRenderMs = 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(SDL_GetPerformanceFrequency());
        ++fullRefreshes;
        rectX = 0;
        rectY = 0;
        rectW = kPanelW;
        rectH = kPanelH;
        haveRect = true;
      } else if (std::strcmp(step.action, "move") == 0) {
        ++partialRefreshes;
        rectX = step.x - markerBox / 2;
        rectY = step.y - markerBox / 2;
        rectW = markerBox;
        rectH = markerBox;
        haveRect = true;
      } else {
        ++skips;
        haveRect = false;  // a skip changes nothing on the panel, so nothing is dirty
      }

      // Every packet redraws the marker onto a fresh copy of the held frame:
      // on a skip the device shows the marker where it already was, which is
      // what the copy reproduces.
      frame = background;
      MapRenderer::drawMarker(frame, step.x, step.y, static_cast<MapHeading>(step.markerHeadingStep),
                              kDefaultMapStyle);

      lastAction = step.action;
      lastReason = step.reason;
      ++nextPacket;

      const int64_t gap = (nextPacket < ride.packets.size() && packet.tUtcMs && ride.packets[nextPacket].tUtcMs)
                              ? ride.packets[nextPacket].tUtcMs - packet.tUtcMs
                              : kFallbackPacketGapMs;
      const double scaled = static_cast<double>(gap > 0 ? gap : 0) / (speed > 0.0 ? speed : 1.0);
      dueTicks = now + static_cast<uint64_t>(scaled);
      stepOnce = false;
    } else if (stepOnce) {
      stepOnce = false;
    }

    // ── sidebar ──────────────────────────────────────────────────────────────
    sidebar.fillRoundedRect(0, 0, kSidebarW, kPanelH, 0, MapInk::White);
    char line[160];
    int y = 16;
    y = sidebarLine(sidebar, y, ride.name.c_str(), 20, true);
    y += 8;
    std::snprintf(line, sizeof(line), "packet  %zu / %zu", nextPacket, ride.packets.size());
    y = sidebarLine(sidebar, y, line);
    std::snprintf(line, sizeof(line), "t       +%02d:%02d:%02d", static_cast<int>(rideNowMs / 3600000),
                  static_cast<int>((rideNowMs / 60000) % 60), static_cast<int>((rideNowMs / 1000) % 60));
    y = sidebarLine(sidebar, y, line);
    std::snprintf(line, sizeof(line), "speed   %gx%s", speed, paused ? "  PAUSED" : "");
    y = sidebarLine(sidebar, y, line);
    y += 16;
    std::snprintf(line, sizeof(line), "FULL       %d", fullRefreshes);
    y = sidebarLine(sidebar, y, line, 20, true);
    std::snprintf(line, sizeof(line), "PARTIAL    %d", partialRefreshes);
    y = sidebarLine(sidebar, y, line, 20, true);
    std::snprintf(line, sizeof(line), "SKIP       %d", skips);
    y = sidebarLine(sidebar, y, line, 20, true);
    y += 16;
    // Panel time only. The render half is the device's own and is not modelled
    // here -- see kPanelRefreshMs.
    std::snprintf(line, sizeof(line), "panel   %.1f s", (fullRefreshes + partialRefreshes) * kPanelRefreshMs / 1000.0);
    y = sidebarLine(sidebar, y, line);
    y = sidebarLine(sidebar, y, "        500 ms/refresh, X4", 16);
    std::snprintf(line, sizeof(line), "host    %.1f ms render", lastRenderMs);
    y = sidebarLine(sidebar, y, line, 16);
    y = sidebarLine(sidebar, y, "        laptop, not device", 16);
    y += 16;
    std::snprintf(line, sizeof(line), "last    %s%s%s", lastAction.c_str(), lastReason.empty() ? "" : " / ",
                  lastReason.c_str());
    y = sidebarLine(sidebar, y, line);
    if (haveRect) {
      std::snprintf(line, sizeof(line), "rect    %d,%d %dx%d", rectX, rectY, rectW, rectH);
      y = sidebarLine(sidebar, y, line);
    }
    y += 16;
    std::snprintf(line, sizeof(line), "tiles   %d in, %d missing", lastPreview.tilesLoaded, lastPreview.tilesMissing);
    y = sidebarLine(sidebar, y, line, 16);
    std::snprintf(line, sizeof(line), "ways    %u drawn", lastPreview.waysDrawn);
    y = sidebarLine(sidebar, y, line, 16);

    y = kPanelH - 76;
    y = sidebarLine(sidebar, y, "space pause   +/- speed", 16);
    y = sidebarLine(sidebar, y, ". step   r restart", 16);
    y = sidebarLine(sidebar, y, "d rect   q quit", 16);

    // ── present ──────────────────────────────────────────────────────────────
    blitCanvas(frame, kPanelW, kPanelH, pixels.data(), kWindowW, 0);
    blitCanvas(sidebar, kSidebarW, kPanelH, pixels.data(), kWindowW, kPanelW);
    for (int row = 0; row < kPanelH; ++row) pixels[static_cast<size_t>(row) * kWindowW + kPanelW] = kSeparator;
    if (showRect && haveRect) strokeRect(pixels.data(), kWindowW, rectX, rectY, rectW, rectH, kRectInk);

    SDL_UpdateTexture(texture, nullptr, pixels.data(), kWindowW * static_cast<int>(sizeof(uint32_t)));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
    if (options.exitAtEnd && nextPacket >= ride.packets.size()) running = false;
  }

  const ReplayEngine::Result& result = stepper.result();
  std::printf("%d packets: %d skips, %d moves, %d re-anchors (%d heading, %d budget, %d keep-in)\n", result.packets,
              result.skips, result.moves, result.reAnchors, result.headingAnchors, result.budgetAnchors,
              result.keepInAnchors);

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
