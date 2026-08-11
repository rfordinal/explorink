#pragma once

#include <cstdint>

// Counters for the things that cost power, and nothing else.
//
// This device is battery powered and nothing in it has ever measured its own
// draw (docs/power-management.md). A bench meter answers "how many mA", but it
// cannot say *what* spent them -- how many panel refreshes a ride took, how
// long the CPU sat at full clock, how many loop iterations ran between two
// fixes. Those are the numbers a change has to move, so they are counted here.
//
// Deliberately dependency-free: Arduino's millis() and nothing else. The
// counting sites are in the HAL (HalDisplay, HalPowerManager) and in main.cpp's
// loop, all of which are below or beside the app layer -- a counter that pulled
// in SD or BLE could not live there. Reading the counters out is somebody
// else's job: PowerLog writes them to the card, the map console's `stats`
// command answers them over BLE.
//
// Single-threaded by assumption, not by lock. Every writer runs on the main
// task (the render task does not refresh the panel itself), and the readers
// take a snapshot that can at worst be one iteration stale. A mutex here would
// cost more than the numbers are worth.
class PowerTelemetry {
 public:
  // What a refresh cost, by waveform. The three modes are HalDisplay's, plus
  // the windowed differential update, which is a fourth thing entirely: it
  // addresses only a rectangle, so it is neither free nor a full frame.
  enum class Refresh : uint8_t {
    Full,    // FULL_REFRESH -- complete waveform, the expensive one
    Half,    // HALF_REFRESH
    Fast,    // FAST_REFRESH -- custom LUT
    Window,  // displayWindow(), the marker-move path
  };

  struct Snapshot {
    uint32_t uptimeS = 0;

    // Panel. The counts say what the ride actually did; panelBusyMs is the
    // wall-clock time spent inside a blocking refresh call, which is the best
    // proxy for panel energy this side of an inline meter.
    uint32_t refreshFull = 0;
    uint32_t refreshHalf = 0;
    uint32_t refreshFast = 0;
    uint32_t refreshWindow = 0;
    uint32_t panelBusyMs = 0;

    // Main loop. iters/busyMs together give the duty cycle: how much of the
    // ride the CPU spent doing work rather than sitting in delay().
    uint32_t loopIters = 0;
    uint32_t loopBusyMs = 0;
    uint32_t loopMaxMs = 0;

    // CPU clock. The map screen pins the CPU at full speed for as long as BLE
    // is up (MapActivity::preventAutoSleep()), so on a ride throttledMs is
    // expected to be 0 -- and proving that on hardware is the point.
    uint32_t fullClockMs = 0;
    uint32_t throttledMs = 0;
    uint16_t cpuMhz = 0;
  };

  static PowerTelemetry& getInstance();

  // Called by HalDisplay around every panel refresh. `busyMs` is how long the
  // call blocked; 0 for an async start (the wait is counted where it happens).
  void onRefresh(Refresh kind, uint32_t busyMs);

  // Time spent blocked in waitRefreshComplete(), i.e. the tail of an async
  // refresh already counted by onRefresh(). Adds to panelBusyMs without adding
  // a second refresh to the tally.
  void onPanelWait(uint32_t busyMs);

  // Called once per main-loop iteration with how long that iteration took.
  void onLoop(uint32_t busyMs);

  // Called by HalPowerManager on every actual frequency change, and once at
  // begin() to state the starting clock. Time is attributed to whichever clock
  // was in force before the change.
  void onCpuFrequency(uint16_t mhz, bool throttled);

  Snapshot snapshot() const;

  // Zeroes everything except the clock the CPU is on right now. For an A/B run:
  // reset, ride a fixed leg, read. Without this every number is since boot and
  // two legs cannot be compared without subtracting by hand.
  void reset();

 private:
  PowerTelemetry() = default;

  // Rolls elapsed time into the right clock bucket and restarts the meter.
  // Called on every change and on every snapshot, so a reader never sees the
  // current interval missing.
  void accrueClock() const;

  mutable uint32_t fullClockMs_ = 0;
  mutable uint32_t throttledMs_ = 0;
  mutable uint32_t clockSinceMs_ = 0;  // millis() at the last accrual
  uint16_t cpuMhz_ = 0;
  bool throttled_ = false;
  bool clockStarted_ = false;

  uint32_t refreshFull_ = 0;
  uint32_t refreshHalf_ = 0;
  uint32_t refreshFast_ = 0;
  uint32_t refreshWindow_ = 0;
  uint32_t panelBusyMs_ = 0;

  uint32_t loopIters_ = 0;
  uint32_t loopBusyMs_ = 0;
  uint32_t loopMaxMs_ = 0;
};

#define POWER_TELEMETRY PowerTelemetry::getInstance()
