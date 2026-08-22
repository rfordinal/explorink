// The device window: a recorded ride playing live at 480x800, with a menu for
// everything the rider's own map menu changes -- zoom rung, ride mode, layers.
//
// Two host binaries already existed and answered half of this each:
// `map_replay` walks a ride through the real `MapFollow::decide()` and prints
// totals; `tools/render_ride_video.py` (parent repo) renders those decisions
// into an MP4 -- but only the full redraws, "Scope cut, deliberate: only
// ReAnchor frames are rendered". So neither one lets you sit and watch the
// marker crawl, see a partial refresh happen, or change a setting and watch
// what it costs.
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
// The sidebar is a mouse menu, not a key chart: click a zoom rung, a mode, a
// layer. **Every setting change re-anchors and is counted as a full refresh**,
// because that is what the device does -- "A viewport reset -- a ladder step, a
// mode switch, a Refresh" (MapActivity.h). So the cost of changing a setting is
// on screen next to the setting.
//
// What this is NOT: a device. It is x86, so nothing here says what a redraw
// costs on a 160 MHz C3, and there is no heap ceiling, no waveform and no
// ghosting. It also links the map code, not the UI stack, so it draws the map
// screen and no other activity. See docs/virtual-device.md.
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "MapMarkerMetrics.h"
#include "MapModeMask.h"
#include "MapPointTypes.h"
#include "MapPreviewPipeline.h"
#include "MapRenderer.h"
#include "MapRideMode.h"
#include "MapStyleDefaults.h"
#include "MapViewport.h"
#include "PpmCanvas.h"
#include "ReplayEngine.h"
#include "RideLog.h"

namespace {

constexpr int kPanelW = 480;
constexpr int kPanelH = 800;
constexpr int kSidebarW = 360;
constexpr int kWindowW = kPanelW + kSidebarW;
constexpr int kWindowH = kPanelH;

// Window chrome, drawn over the blitted pixels rather than into either canvas:
// the panel half of this window must stay exactly the 1-bit picture the device
// would hold, so an overlay that helps a human read it does not get to write
// into it.
constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kRectInk = 0xFFE04030u;  // dirty-rect outline
constexpr uint32_t kSeparator = 0xFFB0B0B0u;
constexpr uint32_t kHoverInk = 0xFF3070C0u;

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

// Menu geometry, in sidebar-local pixels.
constexpr int kPad = 12;
constexpr int kChipPadX = 7;
constexpr int kChipPadY = 4;
constexpr int kChipGap = 5;
constexpr int kTextPx = 14;
constexpr int kSmallPx = 12;

enum class Act {
  None,
  PlayPause,
  Restart,
  SpeedDown,
  SpeedUp,
  Zoom,       // arg = ladder step
  Mode,       // arg = MapRideMode
  Poi,        // whole layer
  PoiCat,     // arg = MapSafetyCategory id
  Buildings,  // cycles rung / on / off
  Hatch,
  Route,
  Rect,
};

struct Hit {
  SDL_Rect box{};  // window coordinates
  Act act = Act::None;
  int arg = 0;
};

struct Options {
  std::string ridePath;
  std::string tilesDir;
  std::string routePath;
  int zoomStep = -1;    // -1 follows the mode's own default rung
  int markerStep = -1;  // -1 follows the mode's own default rung
  MapRideMode mode = MapRideMode::Ride;
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
    } else if (arg == "--route") {
      const char* v = next();
      if (!v) return false;
      out.routePath = v;
    } else if (arg == "--zoom") {
      const char* v = next();
      if (!v) return false;
      out.zoomStep = std::atoi(v);
    } else if (arg == "--marker") {
      const char* v = next();
      if (!v) return false;
      out.markerStep = std::atoi(v);
    } else if (arg == "--mode") {
      const char* v = next();
      if (!v) return false;
      if (std::strcmp(v, "ride") == 0) {
        out.mode = MapRideMode::Ride;
      } else if (std::strcmp(v, "hike") == 0) {
        out.mode = MapRideMode::Hike;
      } else if (std::strcmp(v, "cycle") == 0) {
        out.mode = MapRideMode::Cycle;
      } else {
        std::fprintf(stderr, "mode must be ride|hike|cycle\n");
        return false;
      }
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

// The sidebar is drawn with the firmware's own font tables through PpmCanvas --
// no SDL_ttf, and the same rasterizer the map labels use.
class Sidebar {
 public:
  Sidebar(PpmCanvas& canvas, std::vector<Hit>& hits, int mouseX, int mouseY)
      : canvas_(canvas), hits_(hits), mouseX_(mouseX - kPanelW), mouseY_(mouseY) {
    canvas_.fillRoundedRect(0, 0, kSidebarW, kPanelH, 0, MapInk::White);
  }

  void text(const char* s, int sizePx = kTextPx, bool bold = false) {
    int w = 0, h = 0;
    if (!canvas_.measureText(s, sizePx, bold, w, h)) return;
    canvas_.drawText(kPad, y_, s, sizePx, bold, MapInk::Black);
    y_ += h + 3;
  }

  void gap(int px) { y_ += px; }

  void rule() {
    canvas_.fillRoundedRect(kPad, y_ + 4, kSidebarW - 2 * kPad, 1, 0, MapInk::Black);
    y_ += 12;
  }

  void newRow() {
    if (rowHeight_ > 0) y_ += rowHeight_ + kChipGap;
    x_ = kPad;
    rowHeight_ = 0;
  }

  // One clickable chip. `selected` inverts it, the way a chosen row reads on the
  // panel itself -- black box, white text.
  void chip(const char* label, bool selected, Act act, int arg = 0, int sizePx = kTextPx) {
    int tw = 0, th = 0;
    if (!canvas_.measureText(label, sizePx, false, tw, th)) return;
    const int w = tw + 2 * kChipPadX;
    const int h = th + 2 * kChipPadY;
    if (x_ + w > kSidebarW - kPad) newRow();

    if (selected) {
      canvas_.fillRoundedRect(x_, y_, w, h, 3, MapInk::Black);
      canvas_.drawText(x_ + kChipPadX, y_ + kChipPadY, label, sizePx, false, MapInk::White);
    } else {
      canvas_.fillRoundedRect(x_, y_, w, h, 3, MapInk::Black);
      canvas_.fillRoundedRect(x_ + 1, y_ + 1, w - 2, h - 2, 3, MapInk::White);
      canvas_.drawText(x_ + kChipPadX, y_ + kChipPadY, label, sizePx, false, MapInk::Black);
    }

    hits_.push_back({SDL_Rect{x_ + kPanelW, y_, w, h}, act, arg});
    if (mouseX_ >= x_ && mouseX_ < x_ + w && mouseY_ >= y_ && mouseY_ < y_ + h) {
      hover_ = SDL_Rect{x_ + kPanelW, y_, w, h};
      haveHover_ = true;
    }

    x_ += w + kChipGap;
    if (h > rowHeight_) rowHeight_ = h;
  }

  bool hover(SDL_Rect& out) const {
    out = hover_;
    return haveHover_;
  }

 private:
  PpmCanvas& canvas_;
  std::vector<Hit>& hits_;
  int mouseX_ = -1;
  int mouseY_ = -1;
  int x_ = kPad;
  int y_ = kPad;
  int rowHeight_ = 0;
  SDL_Rect hover_{};
  bool haveHover_ = false;
};

void blitCanvas(const PpmCanvas& canvas, int canvasW, int canvasH, uint32_t* out, int outStride, int originX) {
  const std::vector<uint8_t>& pixels = canvas.pixels();
  for (int y = 0; y < canvasH; ++y) {
    uint32_t* row = out + static_cast<size_t>(y) * outStride + originX;
    const uint8_t* src = pixels.data() + static_cast<size_t>(y) * canvasW;
    for (int x = 0; x < canvasW; ++x) row[x] = src[x] ? kBlack : kWhite;
  }
}

void strokeRect(uint32_t* out, int outStride, int x, int y, int w, int h, uint32_t ink, int clipW) {
  const auto put = [&](int px, int py) {
    if (px < 0 || py < 0 || px >= clipW || py >= kPanelH) return;
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
                 "usage: map_window --ride <ride.jsonl> --tiles <dir> [--route <route.tir>]\n"
                 "                  [--mode ride|hike|cycle] [--zoom 0-%d] [--marker 0-%d]\n"
                 "                  [--speed X] [--no-rect] [--exit-at-end]\n",
                 MapViewport::kZoomStepCount - 1, kMapMarkerStepCount - 1);
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

  // Each mode remembers its own rung on both ladders, exactly as the device
  // does -- "the chosen step is stored per mode, so switching ride to hike and
  // back returns each to what it was" (MapRideMode.h). The CLI overrides only
  // the mode it starts in.
  int zoomStepFor[kMapRideModeCount];
  int markerStepFor[kMapRideModeCount];
  for (int m = 0; m < kMapRideModeCount; ++m) {
    zoomStepFor[m] = kDefaultZoomStepForMode[m];
    markerStepFor[m] = kDefaultMarkerStepForMode[m];
  }
  MapRideMode mode = options.mode;
  const int modeIndex = static_cast<int>(mode);
  if (options.zoomStep >= 0 && options.zoomStep < MapViewport::kZoomStepCount) zoomStepFor[modeIndex] = options.zoomStep;
  if (options.markerStep >= 0 && options.markerStep < kMapMarkerStepCount) markerStepFor[modeIndex] = options.markerStep;

  const MapModeMasks modeMasks;

  // Layer state, all of it a MapPreviewRequest field.
  bool drawPoints = true;
  uint16_t pointCategoryMask = 0xFFFFu;
  std::optional<bool> drawBuildings;  // empty follows the rung, which is the device's default
  bool drawHatch = true;
  bool drawRoute = !options.routePath.empty();

  ReplayEngine::Config config;
  config.zoomStep = zoomStepFor[modeIndex];
  config.markerStep = markerStepFor[modeIndex];
  config.screenWidth = kPanelW;
  config.screenHeight = kPanelH;

  PpmCanvas background(kPanelW, kPanelH);  // the held frame, no marker on it
  PpmCanvas frame(kPanelW, kPanelH);       // background + the marker where it is now
  PpmCanvas sidebarCanvas(kSidebarW, kPanelH);
  std::vector<uint32_t> pixels(static_cast<size_t>(kWindowW) * kWindowH, kWhite);
  std::vector<Hit> hits;

  bool running = true;
  bool paused = false;
  bool stepOnce = false;
  bool showRect = options.showRect;
  double speed = options.speed;
  int mouseX = -1, mouseY = -1;

  // Rewindable playback state, all reset together by Restart.
  ReplayEngine::Stepper stepper(config);
  size_t nextPacket = 0;
  int fullRefreshes = 0, partialRefreshes = 0, skips = 0;
  int64_t rideStartMs = 0, rideNowMs = 0;
  std::string lastAction = "-", lastReason;
  int16_t markerX = 0, markerY = 0;
  uint8_t markerHeading = 0;
  int rectX = 0, rectY = 0, rectW = 0, rectH = 0;
  bool haveRect = false;
  double lastRenderMs = 0.0;
  MapPreviewResult lastPreview;
  uint64_t dueTicks = SDL_GetTicks64();

  // Renders the held frame for a view. Every full refresh goes through here,
  // whether a decision or the menu asked for it.
  const auto renderFrame = [&](double lat, double lon, uint8_t headingStep, int16_t anchorRow) {
    MapPreviewRequest request;
    request.tilesDir = options.tilesDir;
    request.lat = lat;
    request.lon = lon;
    request.heading = headingStep;
    request.zoom = zoomStepFor[static_cast<int>(mode)];
    request.markerY = anchorRow;
    request.classMask = modeMasks.forMode(mode);
    request.drawPoints = drawPoints;
    request.pointCategoryMask = pointCategoryMask;
    request.drawBuildings = drawBuildings;
    request.drawHatch = drawHatch;
    if (drawRoute) request.routePath = options.routePath;
    // The marker is stamped separately, once per packet, exactly the way
    // marker_stamp does it for the video tool -- with this left on, the held
    // background carries a puck baked in at the anchor and every later packet's
    // frame would show two.
    request.drawMarker = false;

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
  };

  const auto stampMarker = [&]() {
    frame = background;
    MapRenderer::drawMarker(frame, markerX, markerY, static_cast<MapHeading>(markerHeading), kDefaultMapStyle);
  };

  // A setting changed. The device answers a menu action with a viewport reset
  // (MapActivity.h), so this does too -- and the full refresh it costs lands in
  // the counter next to the setting that caused it.
  const auto applySettingChange = [&](const char* what) {
    stepper.setZoomStep(zoomStepFor[static_cast<int>(mode)]);
    stepper.setMarkerStep(markerStepFor[static_cast<int>(mode)]);
    ReplayEngine::Stepper::Step step;
    if (!stepper.reAnchorOnLastFix(step)) return;  // no fix yet, nothing to redraw
    markerX = step.x;
    markerY = step.y;
    markerHeading = step.markerHeadingStep;
    renderFrame(step.frameLat, step.frameLon, step.frameHeadingStep, step.y);
    stampMarker();
    lastAction = "reanchor";
    lastReason = what;
  };

  const auto restart = [&]() {
    config.zoomStep = zoomStepFor[static_cast<int>(mode)];
    config.markerStep = markerStepFor[static_cast<int>(mode)];
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

  const auto onClick = [&](const Hit& hit) {
    switch (hit.act) {
      case Act::PlayPause: paused = !paused; break;
      case Act::Restart: restart(); break;
      case Act::SpeedDown: speed = speed <= 0.125 ? 0.125 : speed / 2.0; break;
      case Act::SpeedUp: speed = speed >= 64.0 ? 64.0 : speed * 2.0; break;
      case Act::Zoom:
        zoomStepFor[static_cast<int>(mode)] = hit.arg;
        applySettingChange("zoom");
        break;
      case Act::Mode:
        mode = static_cast<MapRideMode>(hit.arg);
        applySettingChange("mode");
        break;
      case Act::Poi:
        drawPoints = !drawPoints;
        applySettingChange("points");
        break;
      case Act::PoiCat:
        pointCategoryMask ^= static_cast<uint16_t>(1u << hit.arg);
        applySettingChange("points");
        break;
      case Act::Buildings:
        // rung -> on -> off -> rung
        if (!drawBuildings) {
          drawBuildings = true;
        } else if (*drawBuildings) {
          drawBuildings = false;
        } else {
          drawBuildings.reset();
        }
        applySettingChange("buildings");
        break;
      case Act::Hatch:
        drawHatch = !drawHatch;
        applySettingChange("hatch");
        break;
      case Act::Route:
        drawRoute = !drawRoute;
        applySettingChange("route");
        break;
      case Act::Rect: showRect = !showRect; break;
      case Act::None: break;
    }
  };

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
      if (event.type == SDL_MOUSEMOTION) {
        mouseX = event.motion.x;
        mouseY = event.motion.y;
      }
      if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
        for (const Hit& hit : hits) {
          if (event.button.x >= hit.box.x && event.button.x < hit.box.x + hit.box.w && event.button.y >= hit.box.y &&
              event.button.y < hit.box.y + hit.box.h) {
            onClick(hit);
            break;
          }
        }
      }
      if (event.type != SDL_KEYDOWN) continue;
      // Keys stay for the two things a mouse is bad at: pausing without
      // aiming, and stepping one packet at a time.
      switch (event.key.keysym.sym) {
        case SDLK_q:
        case SDLK_ESCAPE: running = false; break;
        case SDLK_SPACE: paused = !paused; break;
        case SDLK_PERIOD: stepOnce = true; break;
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
      markerX = step.x;
      markerY = step.y;
      markerHeading = step.markerHeadingStep;

      if (step.frameChanged) {
        renderFrame(step.frameLat, step.frameLon, step.frameHeadingStep, step.y);
      } else if (std::strcmp(step.action, "move") == 0) {
        ++partialRefreshes;
        const int box = markerMetricsFor(MapViewport::kZoomLadder[zoomStepFor[static_cast<int>(mode)]].markerScale8).box;
        rectX = step.x - box / 2;
        rectY = step.y - box / 2;
        rectW = box;
        rectH = box;
        haveRect = true;
      } else {
        ++skips;
        haveRect = false;  // a skip changes nothing on the panel, so nothing is dirty
      }

      // Every packet redraws the marker onto a fresh copy of the held frame: on
      // a skip the device shows the marker where it already was, which is what
      // the copy reproduces.
      stampMarker();

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
    hits.clear();
    Sidebar bar(sidebarCanvas, hits, mouseX, mouseY);
    char line[160];

    bar.text(ride.name.c_str(), kTextPx, true);
    std::snprintf(line, sizeof(line), "packet %zu / %zu     +%02d:%02d:%02d", nextPacket, ride.packets.size(),
                  static_cast<int>(rideNowMs / 3600000), static_cast<int>((rideNowMs / 60000) % 60),
                  static_cast<int>((rideNowMs / 1000) % 60));
    bar.text(line, kSmallPx);
    bar.rule();

    std::snprintf(line, sizeof(line), "FULL %d    PARTIAL %d    SKIP %d", fullRefreshes, partialRefreshes, skips);
    bar.text(line, kTextPx, true);
    std::snprintf(line, sizeof(line), "panel %.1f s at 500 ms/refresh (X4)",
                  (fullRefreshes + partialRefreshes) * kPanelRefreshMs / 1000.0);
    bar.text(line, kSmallPx);
    std::snprintf(line, sizeof(line), "host %.0f ms render -- laptop, not device", lastRenderMs);
    bar.text(line, kSmallPx);
    std::snprintf(line, sizeof(line), "last %s%s%s", lastAction.c_str(), lastReason.empty() ? "" : " / ",
                  lastReason.c_str());
    bar.text(line, kSmallPx);
    if (haveRect) {
      std::snprintf(line, sizeof(line), "rect %d,%d %dx%d", rectX, rectY, rectW, rectH);
      bar.text(line, kSmallPx);
    }
    bar.rule();

    bar.text("playback", kSmallPx, true);
    bar.newRow();
    bar.chip(paused ? "Play" : "Pause", false, Act::PlayPause);
    bar.chip("Restart", false, Act::Restart);
    bar.chip("-", false, Act::SpeedDown);
    std::snprintf(line, sizeof(line), "%gx", speed);
    bar.chip(line, true, Act::None);
    bar.chip("+", false, Act::SpeedUp);
    bar.newRow();
    bar.gap(6);

    bar.text("zoom, metres per pixel", kSmallPx, true);
    bar.newRow();
    for (int step = 0; step < MapViewport::kZoomStepCount; ++step) {
      std::snprintf(line, sizeof(line), "%g", MapViewport::kZoomLadder[step].mpp);
      bar.chip(line, step == zoomStepFor[static_cast<int>(mode)], Act::Zoom, step);
    }
    bar.newRow();
    bar.gap(6);

    bar.text("mode", kSmallPx, true);
    bar.newRow();
    for (int m = 0; m < kMapRideModeCount; ++m) {
      bar.chip(mapRideModeName(static_cast<MapRideMode>(m)), m == static_cast<int>(mode), Act::Mode, m);
    }
    bar.newRow();
    bar.gap(6);

    bar.text("layers", kSmallPx, true);
    bar.newRow();
    bar.chip("POI", drawPoints, Act::Poi);
    const char* buildingsLabel = !drawBuildings ? "buildings: rung" : (*drawBuildings ? "buildings: on" : "buildings: off");
    bar.chip(buildingsLabel, drawBuildings.has_value() && *drawBuildings, Act::Buildings);
    bar.chip("missing hatch", drawHatch, Act::Hatch);
    if (!options.routePath.empty()) bar.chip("route", drawRoute, Act::Route);
    bar.chip("dirty rect", showRect, Act::Rect);
    bar.newRow();
    bar.gap(6);

    if (drawPoints) {
      bar.text("POI categories", kSmallPx, true);
      bar.newRow();
      // Category 0 is Unknown, which no shard should carry a mark for -- skip it
      // rather than offer a toggle for nothing (MapPointTypes.h).
      for (int cat = 1; cat < kSafetyCategoryCount; ++cat) {
        const bool on = (pointCategoryMask & (1u << cat)) != 0;
        bar.chip(kSafetyCategoryNames[cat], on, Act::PoiCat, cat, kSmallPx);
      }
      bar.newRow();
      bar.gap(6);
    }

    std::snprintf(line, sizeof(line), "tiles %d in, %d missing   ways %u", lastPreview.tilesLoaded,
                  lastPreview.tilesMissing, lastPreview.waysDrawn);
    bar.text(line, kSmallPx);
    std::snprintf(line, sizeof(line), "points %u   labels %u placed, %u dropped", lastPreview.pointsDrawn,
                  lastPreview.labelsPlaced, lastPreview.labelsDropped);
    bar.text(line, kSmallPx);
    bar.text("space pause   . step   q quit", kSmallPx);

    // ── present ──────────────────────────────────────────────────────────────
    blitCanvas(frame, kPanelW, kPanelH, pixels.data(), kWindowW, 0);
    blitCanvas(sidebarCanvas, kSidebarW, kPanelH, pixels.data(), kWindowW, kPanelW);
    for (int row = 0; row < kPanelH; ++row) pixels[static_cast<size_t>(row) * kWindowW + kPanelW] = kSeparator;
    if (showRect && haveRect) strokeRect(pixels.data(), kWindowW, rectX, rectY, rectW, rectH, kRectInk, kPanelW);
    SDL_Rect hoverBox;
    if (bar.hover(hoverBox)) {
      strokeRect(pixels.data(), kWindowW, hoverBox.x, hoverBox.y, hoverBox.w, hoverBox.h, kHoverInk, kWindowW);
    }

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
