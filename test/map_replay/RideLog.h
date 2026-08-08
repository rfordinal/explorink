#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A recorded ride's `packet` stream, read off a phone bridge log
// (`docs/rides/*.jsonl` in the parent repo, one JSON object per line).
//
// The `packet` records are what the phone really sent over BLE -- already
// heading-snapped to 0-15 by `HeadingTrend.kt`, already reason-tagged. The
// `fix` records in the same file are the raw GPS stream the phone filtered
// *down* to those packets, so replaying `fix` would feed the device a ride it
// never saw. `tools/replay_ride.py:117-125` (parent repo) makes the same
// choice, which is what makes this harness's output comparable with a real
// device run.
namespace RideLog {

struct Packet {
  double lat = 0.0;
  double lon = 0.0;
  uint8_t headingStep = 0;  // 0-15, 22.5 degrees per step, clockwise from north
  // Wall-clock ms the phone sent this at, straight off the log line. 0 if the
  // line had none -- only test/map_replay's --frames (video timing) reads
  // this; the decision replay itself has no notion of real time.
  int64_t tUtcMs = 0;
};

struct Ride {
  std::string name;  // file stem, for tables
  std::vector<Packet> packets;
};

// Every `type=packet` record with `ok=true`, in file order.
//
// `ok=false` means the BLE write failed and the device never saw that packet,
// so replaying it would invent a fix the ride did not have
// (`tools/replay_ride.py:122-125`).
//
// Returns false only if the file cannot be opened; a malformed line is skipped
// and counted in `skippedLines`.
bool read(const std::string& path, Ride& out, int& skippedLines);

}  // namespace RideLog
