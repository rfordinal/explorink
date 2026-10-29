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

  // Which caller asked for a window. One counter used to merge three call
  // sites, and that is why a 4 h walk's ref_window could not be read: a marker
  // move happens per frame over a small rectangle, the debug overlay fires on
  // its own 5 s timer, and closing chrome repaints most of the screen. Three
  // rectangles at three rates, averaged into one number with no way back
  // (T-277).
  enum class WindowSite : uint8_t {
    Marker,   // the position marker moved -- the per-frame case
    Overlay,  // the debug overlay, on its own 5 s timer
    Status,   // header strip, hike elevation line: periodic text repaints
    Chrome,   // a popup, menu, notice or side panel closing back onto the map
    Other,    // not yet named; a non-map caller
    Count,
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

    // Per-waveform busy time. panelBusyMs is one merged total, and a merged
    // total cannot answer the question this instrument exists for: a row
    // holding both a window and a whole-panel push divides its time between
    // them at whatever ratio the reader assumes. Recomputing run7 by hand on
    // 2026-09-09 needed rows where exactly one counter moved, which threw away
    // 90 % of the samples and still left the whole-panel population at n=1
    // inside the one walk that mattered. These four make each waveform's cost
    // readable from any row.
    uint32_t busyFullMs = 0;
    uint32_t busyHalfMs = 0;
    uint32_t busyFastMs = 0;
    uint32_t busyWindowMs = 0;

    // Window requests split by call site (WindowSite), so the marker-move rate
    // is separable from the overlay timer and from closing chrome.
    uint32_t windowBySite[static_cast<uint8_t>(WindowSite::Count)] = {};

    // How often an "async" refresh actually completed inline. On a driver with
    // no supportsAsyncDisplay() override the facade takes the blocking path,
    // so the caller paid the full refresh inside a call that used to be billed
    // at 0 ms. Zero here means the async accounting is honest; anything else
    // means panel time went missing (see onAsyncCompletedInline).
    uint32_t asyncCompletedInline = 0;

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

  // Called by HalDisplay around every panel refresh.
  //
  // Microseconds, not milliseconds, and the name says so on purpose: this used
  // to take ms, and a caller left on the old unit would under-report by 1000x
  // while still compiling and still producing plausible numbers. The unit
  // matters because millis() quantisation is ~0.2 % of one refresh but the
  // question being settled is a 2x that has to survive being split four ways
  // by waveform and four more by call site.
  //
  // `site` is read only for Refresh::Window and ignored otherwise.
  void onRefreshUs(Refresh kind, uint32_t busyUs, WindowSite site = WindowSite::Other);

  // Time spent blocked in waitRefreshComplete(), i.e. the tail of an async
  // refresh already counted by onRefreshUs(). Adds to panelBusyMs without
  // adding a second refresh to the tally.
  void onPanelWaitUs(uint32_t busyUs);

  // An async refresh that the driver completed inline, so its cost was paid
  // inside the call that started it. Counted rather than assumed away: the
  // claim that this never happens is load-bearing for every per-refresh number
  // in power.csv, and a claim nothing can falsify is not a measurement.
  void onAsyncCompletedInline(uint32_t busyUs);

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

  // uint64_t: a microsecond total overflows uint32_t after 71 minutes of panel
  // time, and the 2026-09-07 walk alone spent 824 s in the panel path across
  // 4 h 08 min. The snapshot narrows to ms, where uint32_t is 49 days.
  uint64_t busyUs_[4] = {};
  uint32_t windowSite_[static_cast<uint8_t>(WindowSite::Count)] = {};
  uint32_t asyncCompletedInline_ = 0;

  uint32_t loopIters_ = 0;
  uint32_t loopBusyMs_ = 0;
  uint32_t loopMaxMs_ = 0;
};

#define POWER_TELEMETRY PowerTelemetry::getInstance()
