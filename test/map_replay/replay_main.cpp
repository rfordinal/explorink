// Replay recorded rides through the real MapFollow::decide(), on the host.
//
// No serial port, no /dev/ttyACM0, no X4 lock, no device of any kind: this
// links the firmware's own decision code (MapFollow.cpp), projection
// (MapProjection.cpp) and viewport ladders (MapViewport.cpp) into a laptop
// binary and walks a ride's packet stream through them. A ride is milliseconds,
// so a threshold can be swept across every ride in one run instead of one
// reflash per value.
//
// See firmware/explorink/docs/map-follow.md, "Sweeping the thresholds off the
// device", for what it measures and what it cannot.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "ReplayEngine.h"
#include "RideLog.h"

namespace {

struct Options {
  std::vector<std::string> ridePaths;
  ReplayEngine::Config config;
  std::vector<int> sweepMinMoves;
  std::vector<int> sweepDrift;
  std::vector<int> sweepBudget;
  std::string checkPath;
  bool showEvents = false;
  std::string framesDir;
  std::string trackDir;
};

void usage() {
  std::printf(
      "map_replay [options] <ride.jsonl>...\n"
      "\n"
      "  --zoom N          zoom ladder step (default %d, 20 m/px)\n"
      "  --marker N        marker ladder step (default %d)\n"
      "  --width N         screen width in px (default %d)\n"
      "  --height N        screen height in px (default %d)\n"
      "  --decimals N      decimals the fix is rounded to (default %d, what\n"
      "                    tools/replay_ride.py types at the map console)\n"
      "  --drift N         kMaxHeadingDriftSteps for this run (default %u)\n"
      "  --min-moves N     kMinPartialMovesForHeadingReAnchor (default %u)\n"
      "  --budget N        kMaxPartialMoves (default %u)\n"
      "\n"
      "  --sweep-min-moves 0,1,2,3,4,6,8    sweep the movement floor\n"
      "  --sweep-drift 3,4,5                sweep the heading drift limit\n"
      "  --sweep-budget 8,12,16             sweep the ghosting budget\n"
      "        Any --sweep-* switches to the sweep table. Several sweep axes\n"
      "        together run their full cross product.\n"
      "\n"
      "  --check FILE      replay against hardware-measured numbers and diff\n"
      "  --events          print every packet's outcome, one row each, per ride\n"
      "                    (baseline mode only -- ignored under --sweep-*/--check)\n"
      "  --frames DIR      write <ride>.frames.csv per ride: one row per full\n"
      "                    redraw (ReAnchor) -- packet,t_utc_ms,lat,lon,heading --\n"
      "                    for tools/render_ride_video.py (parent repo) to turn\n"
      "                    into PNGs and a video. MoveMarker/Skip are not real\n"
      "                    redraws so are not rows here.\n"
      "  --track DIR       write <ride>.track.csv per ride: one row per packet\n"
      "                    (skip/move/reanchor all included) -- x,y,fix_heading,\n"
      "                    anchor_heading -- for render_ride_video.py --track,\n"
      "                    which draws a marker on every frame, not just redraws.\n",
      ReplayEngine::Config{}.zoomStep, ReplayEngine::Config{}.markerStep, ReplayEngine::Config{}.screenWidth,
      ReplayEngine::Config{}.screenHeight, ReplayEngine::Config{}.coordDecimals,
      (unsigned)ReplayEngine::Config{}.headingDriftLimitSteps,
      (unsigned)ReplayEngine::Config{}.minPartialMovesForHeadingReAnchor,
      (unsigned)ReplayEngine::Config{}.partialMoveBudget);
}

bool parseIntList(const std::string& text, std::vector<int>& out) {
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (item.empty()) return false;
    out.push_back(std::atoi(item.c_str()));
  }
  return !out.empty();
}

bool parseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (arg == "--help" || arg == "-h") return false;
    if (arg == "--zoom") {
      const char* v = value();
      if (!v) return false;
      options.config.zoomStep = std::atoi(v);
    } else if (arg == "--marker") {
      const char* v = value();
      if (!v) return false;
      options.config.markerStep = std::atoi(v);
    } else if (arg == "--width") {
      const char* v = value();
      if (!v) return false;
      options.config.screenWidth = static_cast<int16_t>(std::atoi(v));
    } else if (arg == "--height") {
      const char* v = value();
      if (!v) return false;
      options.config.screenHeight = static_cast<int16_t>(std::atoi(v));
    } else if (arg == "--decimals") {
      const char* v = value();
      if (!v) return false;
      options.config.coordDecimals = std::atoi(v);
    } else if (arg == "--drift") {
      const char* v = value();
      if (!v) return false;
      options.config.headingDriftLimitSteps = static_cast<uint8_t>(std::atoi(v));
    } else if (arg == "--min-moves") {
      const char* v = value();
      if (!v) return false;
      options.config.minPartialMovesForHeadingReAnchor = static_cast<uint16_t>(std::atoi(v));
    } else if (arg == "--budget") {
      const char* v = value();
      if (!v) return false;
      options.config.partialMoveBudget = static_cast<uint16_t>(std::atoi(v));
    } else if (arg == "--sweep-min-moves") {
      const char* v = value();
      if (!v || !parseIntList(v, options.sweepMinMoves)) return false;
    } else if (arg == "--sweep-drift") {
      const char* v = value();
      if (!v || !parseIntList(v, options.sweepDrift)) return false;
    } else if (arg == "--sweep-budget") {
      const char* v = value();
      if (!v || !parseIntList(v, options.sweepBudget)) return false;
    } else if (arg == "--check") {
      const char* v = value();
      if (!v) return false;
      options.checkPath = v;
    } else if (arg == "--events") {
      options.showEvents = true;
    } else if (arg == "--frames") {
      const char* v = value();
      if (!v) return false;
      options.framesDir = v;
    } else if (arg == "--track") {
      const char* v = value();
      if (!v) return false;
      options.trackDir = v;
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return false;
    } else {
      options.ridePaths.push_back(arg);
    }
  }
  return !options.ridePaths.empty();
}

std::vector<RideLog::Ride> loadRides(const std::vector<std::string>& paths) {
  std::vector<RideLog::Ride> rides;
  for (const std::string& path : paths) {
    RideLog::Ride ride;
    int skipped = 0;
    if (!RideLog::read(path, ride, skipped)) {
      std::fprintf(stderr, "cannot read %s\n", path.c_str());
      continue;
    }
    if (skipped > 0) std::fprintf(stderr, "%s: %d unparseable line(s) skipped\n", path.c_str(), skipped);
    if (ride.packets.empty()) {
      std::fprintf(stderr, "%s: no ok packets, skipping\n", path.c_str());
      continue;
    }
    rides.push_back(std::move(ride));
  }
  return rides;
}

void printConfig(const ReplayEngine::Config& config) {
  std::printf("zoom step %d, marker step %d, %dx%d px, fix to %d decimals\n", config.zoomStep, config.markerStep,
              (int)config.screenWidth, (int)config.screenHeight, config.coordDecimals);
}

// --- baseline table -------------------------------------------------------

void runBaseline(const std::vector<RideLog::Ride>& rides, const ReplayEngine::Config& config) {
  printConfig(config);
  std::printf("drift limit %u, movement floor %u, ghosting budget %u\n\n", (unsigned)config.headingDriftLimitSteps,
              (unsigned)config.minPartialMovesForHeadingReAnchor, (unsigned)config.partialMoveBudget);
  std::printf("%-32s %8s %7s %7s %8s %8s %7s %8s %7s\n", "ride", "packets", "skips", "moves", "redraws", "heading",
              "budget", "keep-in", "thrash");

  ReplayEngine::Result totals;
  for (const RideLog::Ride& ride : rides) {
    const ReplayEngine::Result result = ReplayEngine::replay(ride.packets, config);
    std::printf("%-32s %8d %7d %7d %8d %8d %7d %8d %7d\n", ride.name.c_str(), result.packets, result.skips,
                result.moves, result.reAnchors, result.headingAnchors, result.budgetAnchors, result.keepInAnchors,
                result.thrashAnchors);
    totals.packets += result.packets;
    totals.skips += result.skips;
    totals.moves += result.moves;
    totals.reAnchors += result.reAnchors;
    totals.headingAnchors += result.headingAnchors;
    totals.budgetAnchors += result.budgetAnchors;
    totals.keepInAnchors += result.keepInAnchors;
    totals.thrashAnchors += result.thrashAnchors;
    totals.headingMovesIn.insert(totals.headingMovesIn.end(), result.headingMovesIn.begin(),
                                 result.headingMovesIn.end());
  }

  std::printf("%-32s %8d %7d %7d %8d %8d %7d %8d %7d\n", "TOTAL", totals.packets, totals.skips, totals.moves,
              totals.reAnchors, totals.headingAnchors, totals.budgetAnchors, totals.keepInAnchors,
              totals.thrashAnchors);

  if (totals.headingAnchors > 0) {
    std::printf("\nmoves since the last frame, per heading redraw:\n");
    // The distribution the thrash count summarises: how far the marker had
    // actually got when the heading swung past the limit.
    std::vector<int> histogram(16, 0);
    int over = 0;
    for (const int movesIn : totals.headingMovesIn) {
      if (movesIn < 15) {
        ++histogram[movesIn];
      } else {
        ++over;
      }
    }
    for (size_t bucket = 0; bucket < histogram.size() - 1; ++bucket) {
      if (histogram[bucket] == 0) continue;
      std::printf("  %2zu moves in  %3d\n", bucket, histogram[bucket]);
    }
    if (over > 0) std::printf("  15+ moves in %3d\n", over);
    std::printf("  <=1 moves in %3d of %d (%.0f%%)\n", totals.thrashAnchors, totals.headingAnchors,
                100.0 * totals.thrashAnchors / totals.headingAnchors);
  }
}

// --- per-packet events -----------------------------------------------------

void runEvents(const std::vector<RideLog::Ride>& rides, ReplayEngine::Config config) {
  config.recordEvents = true;
  printConfig(config);
  std::printf("drift limit %u, movement floor %u, ghosting budget %u\n", (unsigned)config.headingDriftLimitSteps,
              (unsigned)config.minPartialMovesForHeadingReAnchor, (unsigned)config.partialMoveBudget);

  for (const RideLog::Ride& ride : rides) {
    const ReplayEngine::Result result = ReplayEngine::replay(ride.packets, config);
    std::printf("\n%s\n", ride.name.c_str());
    std::printf("%6s  %-8s %-8s %6s %6s %7s\n", "pkt", "action", "reason", "x", "y", "moves");
    for (const ReplayEngine::Event& event : result.events) {
      std::printf("%6d  %-8s %-8s %6d %6d %7d\n", event.packetIndex, event.action, event.reason, (int)event.x,
                  (int)event.y, event.movesIn);
    }
  }
}

// --- frame list, for a video ------------------------------------------------

// One CSV row per real redraw (ReAnchor) -- the only moments the device's
// picture actually changes wholesale. MoveMarker slides a 64x64 patch inside
// a frame that is otherwise identical, which a video of "what full picture
// was on screen" has no reason to hold a separate frame for; Skip touches
// the panel not at all. See tools/render_ride_video.py's module docstring
// (parent repo) for what turns these rows into PNGs and a video, and for the
// scope cut this implies (marker-only motion is not shown).
void runFrames(const std::vector<RideLog::Ride>& rides, ReplayEngine::Config config, const std::string& dir) {
  config.recordEvents = true;
  for (const RideLog::Ride& ride : rides) {
    const ReplayEngine::Result result = ReplayEngine::replay(ride.packets, config);
    const std::string path = dir + "/" + ride.name + ".frames.csv";
    std::ofstream out(path);
    if (!out) {
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      continue;
    }
    out << "packet,t_utc_ms,lat,lon,heading,reason,moves_in\n";
    int written = 0;
    for (const ReplayEngine::Event& event : result.events) {
      if (std::strcmp(event.action, "reanchor") != 0) continue;
      const RideLog::Packet& packet = ride.packets[event.packetIndex];
      out << event.packetIndex << ',' << packet.tUtcMs << ',' << packet.lat << ',' << packet.lon << ','
          << (int)packet.headingStep << ',' << event.reason << ',' << event.movesIn << '\n';
      ++written;
    }
    std::printf("%s: %d frame(s) -> %s\n", ride.name.c_str(), written, path.c_str());
  }
}

// --- full per-packet track, for a video's marker overlay --------------------

// One CSV row per packet -- skip and move included, not just reanchor -- with
// the frame's own heading at that moment alongside the fix's. A screen
// position (x, y) plus (fix_heading - anchor_heading) steps off "up" is
// where the rider actually was and faced on that packet's frame, whether or
// not the device redrew or moved its own marker to show it. See
// tools/render_ride_video.py's --track mode (parent repo): it holds the last
// real redraw's rendered picture and draws a marker on a copy of it for
// every packet, which is the thing runFrames() above deliberately does not
// do (a real MoveMarker's 64x64 patch is not a full render, and this is not
// one either -- it is a compositing shortcut for a video, not a claim about
// what displayBufferWindow() draws).
void runTrack(const std::vector<RideLog::Ride>& rides, ReplayEngine::Config config, const std::string& dir) {
  config.recordEvents = true;
  for (const RideLog::Ride& ride : rides) {
    const ReplayEngine::Result result = ReplayEngine::replay(ride.packets, config);
    const std::string path = dir + "/" + ride.name + ".track.csv";
    std::ofstream out(path);
    if (!out) {
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      continue;
    }
    out << "packet,t_utc_ms,action,reason,x,y,fix_heading,anchor_heading,moves_in,lat,lon\n";
    for (const ReplayEngine::Event& event : result.events) {
      const RideLog::Packet& packet = ride.packets[event.packetIndex];
      out << event.packetIndex << ',' << packet.tUtcMs << ',' << event.action << ',' << event.reason << ','
          << event.x << ',' << event.y << ',' << (int)packet.headingStep << ',' << (int)event.anchorHeadingStep
          << ',' << event.movesIn << ',' << packet.lat << ',' << packet.lon << '\n';
    }
    std::printf("%s: %d row(s) -> %s\n", ride.name.c_str(), (int)result.events.size(), path.c_str());
  }
}

// --- sweep table ----------------------------------------------------------

void runSweep(const std::vector<RideLog::Ride>& rides, const Options& options) {
  std::vector<int> minMoves = options.sweepMinMoves;
  std::vector<int> drifts = options.sweepDrift;
  std::vector<int> budgets = options.sweepBudget;
  if (minMoves.empty()) minMoves.push_back(options.config.minPartialMovesForHeadingReAnchor);
  if (drifts.empty()) drifts.push_back(options.config.headingDriftLimitSteps);
  if (budgets.empty()) budgets.push_back(options.config.partialMoveBudget);

  printConfig(options.config);
  std::printf("\n%-32s %6s %6s %7s %8s %8s %7s %8s %7s\n", "ride", "floor", "drift", "budget", "redraws", "heading",
              "budget", "keep-in", "thrash");

  for (const int drift : drifts) {
    for (const int budget : budgets) {
      for (const int floor : minMoves) {
        ReplayEngine::Config config = options.config;
        config.minPartialMovesForHeadingReAnchor = static_cast<uint16_t>(floor);
        config.headingDriftLimitSteps = static_cast<uint8_t>(drift);
        config.partialMoveBudget = static_cast<uint16_t>(budget);

        int totalReAnchors = 0, totalHeading = 0, totalBudget = 0, totalKeepIn = 0, totalThrash = 0, totalPackets = 0;
        for (const RideLog::Ride& ride : rides) {
          const ReplayEngine::Result result = ReplayEngine::replay(ride.packets, config);
          std::printf("%-32s %6d %6d %7d %8d %8d %7d %8d %7d\n", ride.name.c_str(), floor, drift, budget,
                      result.reAnchors, result.headingAnchors, result.budgetAnchors, result.keepInAnchors,
                      result.thrashAnchors);
          totalPackets += result.packets;
          totalReAnchors += result.reAnchors;
          totalHeading += result.headingAnchors;
          totalBudget += result.budgetAnchors;
          totalKeepIn += result.keepInAnchors;
          totalThrash += result.thrashAnchors;
        }
        std::printf("%-32s %6d %6d %7d %8d %8d %7d %8d %7d   %.1f%% of %d packets\n", "TOTAL", floor, drift, budget,
                    totalReAnchors, totalHeading, totalBudget, totalKeepIn, totalThrash,
                    100.0 * totalReAnchors / totalPackets, totalPackets);
        std::printf("\n");
      }
    }
  }
}

// --- correctness gate -----------------------------------------------------

struct Expectation {
  std::string stem;
  int packets = 0;
  int redraws = 0;
  int heading = 0;
  int budget = 0;
  int keepIn = 0;
};

bool readExpectations(const std::string& path, std::vector<Expectation>& out) {
  std::ifstream file(path);
  if (!file) return false;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    Expectation e;
    if (!(stream >> e.stem >> e.packets >> e.redraws >> e.heading >> e.budget >> e.keepIn)) continue;
    out.push_back(e);
  }
  return true;
}

int runCheck(const std::vector<RideLog::Ride>& rides, const Options& options) {
  std::vector<Expectation> expectations;
  if (!readExpectations(options.checkPath, expectations)) {
    std::fprintf(stderr, "cannot read %s\n", options.checkPath.c_str());
    return 2;
  }

  // The hardware numbers were measured before the movement floor existed, so
  // the gate replays with it at 0. The other two thresholds stay at their
  // constants deliberately: if either moves, this gate should fail loudly
  // rather than quietly re-baseline itself.
  ReplayEngine::Config config = options.config;
  config.minPartialMovesForHeadingReAnchor = 0;
  printConfig(config);
  std::printf("drift limit %u, movement floor 0 (pre-gate firmware), ghosting budget %u\n\n",
              (unsigned)config.headingDriftLimitSteps, (unsigned)config.partialMoveBudget);
  std::printf("%-32s %-8s %8s %8s %8s %8s %8s\n", "ride", "", "packets", "redraws", "heading", "budget", "keep-in");

  int failures = 0;
  int matched = 0;
  for (const Expectation& e : expectations) {
    const auto found =
        std::find_if(rides.begin(), rides.end(), [&](const RideLog::Ride& ride) { return ride.name == e.stem; });
    if (found == rides.end()) {
      std::printf("%-32s %-8s (not among the rides given)\n", e.stem.c_str(), "SKIP");
      continue;
    }
    ++matched;
    const ReplayEngine::Result result = ReplayEngine::replay(found->packets, config);
    const bool ok = result.packets == e.packets && result.reAnchors == e.redraws &&
                    result.headingAnchors == e.heading && result.budgetAnchors == e.budget &&
                    result.keepInAnchors == e.keepIn;
    if (!ok) ++failures;
    std::printf("%-32s %-8s %8d %8d %8d %8d %8d\n", e.stem.c_str(), ok ? "match" : "DIFFERS", result.packets,
                result.reAnchors, result.headingAnchors, result.budgetAnchors, result.keepInAnchors);
    if (!ok) {
      std::printf("%-32s %-8s %8d %8d %8d %8d %8d   <- measured on the X4\n", "", "hardware", e.packets, e.redraws,
                  e.heading, e.budget, e.keepIn);
    }
  }

  std::printf("\n%d of %d rides match the hardware run%s\n", matched - failures, matched,
              failures == 0 ? "." : " -- the harness has a bug, do not trust a sweep until it is fixed.");
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseArgs(argc, argv, options)) {
    usage();
    return 2;
  }

  const std::vector<RideLog::Ride> rides = loadRides(options.ridePaths);
  if (rides.empty()) {
    std::fprintf(stderr, "no rides to replay\n");
    return 2;
  }

  if (!options.checkPath.empty()) return runCheck(rides, options);
  if (!options.sweepMinMoves.empty() || !options.sweepDrift.empty() || !options.sweepBudget.empty()) {
    runSweep(rides, options);
    return 0;
  }
  runBaseline(rides, options.config);
  if (options.showEvents) runEvents(rides, options.config);
  if (!options.framesDir.empty()) runFrames(rides, options.config, options.framesDir);
  if (!options.trackDir.empty()) runTrack(rides, options.config, options.trackDir);
  return 0;
}
