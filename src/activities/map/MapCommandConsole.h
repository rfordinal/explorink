#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "MapCommandParser.h"

// The channel-free half of the map command console: line assembly, the
// state the grammar drives, and the replies. Pure -- no Arduino, no serial,
// no activity. MapSerialConsole is the USB front end over this; P5's BLE
// characteristic is the second one, over the same object.

// Where replies go. One line per call, no marker prefix and no newline --
// the channel adds both, because the framing differs per channel (serial
// prefixes a marker, BLE will not need one).
class IMapReplyWriter {
 public:
  virtual ~IMapReplyWriter() = default;
  virtual void reply(const char* line) = 0;
};

// Bytes in, whole lines out. Handles the three things a real UART does that
// a desk test does not: a line split across several poll() calls, CR/LF or
// bare LF or bare CR, and a line longer than the buffer.
//
// An over-long line is discarded to its own end of line and reported once,
// as Overflow, at that terminator. It never overflows the buffer and it
// never leaves half a line to be parsed as if it were whole.
class MapLineAssembler {
 public:
  enum class Result : uint8_t {
    Pending,   // byte consumed, line not finished
    Line,      // line() is now valid (possibly empty)
    Overflow,  // the line that just ended was too long and was discarded
  };

  Result feed(char c);

  // Valid until the next feed(), after feed() returned Line.
  std::string_view line() const { return std::string_view(buf_, lineLen_); }

  void reset();

  static constexpr size_t kMaxLine = 95;

 private:
  char buf_[kMaxLine + 1] = {};
  size_t len_ = 0;
  size_t lineLen_ = 0;
  bool discarding_ = false;
};

// One tile's outcome in the most recent viewport reset -- what the `tiles`
// command prints one line per index for. Pushed, never polled: MapActivity
// is the only thing that knows the tile range, so it calls setTileRange()
// right after every reset (MapActivity.cpp, renderViewport()).
//
// Convention matches MapViewport::TileRange and MapTileSource::
// unavailableMask(): column-major index over col0..col1 then row0..row1,
// one bit per index, set means absent/truncated/crc-mismatched.
struct MapTileRangeSnapshot {
  bool valid = false;  // false until the first reset
  uint8_t z = 0;
  uint32_t col0 = 0;
  uint32_t row0 = 0;
  uint32_t col1 = 0;
  uint32_t row1 = 0;
  uint32_t unavailableMask = 0;
};

// What the commands actually do, and the only thing that knows it. Holds no
// channel and no hardware, so P3's serial console and P5's BLE
// characteristic run the same lines through the same object and get the
// same behaviour by construction.
//
// **One instance per map screen, shared by every channel.** MapActivity owns
// it and hands a reference to each transport. Two states would mean the map
// had two zooms, and which one you got would depend on which cable was
// plugged in.
//
// Implements the whole grammar: pos, heading, zoom, marker, mode, redraw,
// tiles, info. zoom/marker/mode only move the numbers here -- applying them
// to the projection, the class mask and the settings file is MapActivity's
// job, which is what keeps this half free of hardware.
class MapConsoleState {
 public:
  // Runs one command, writes exactly the reply lines it produces, and
  // returns true if the screen needs redrawing.
  bool execute(const MapCommand& cmd, IMapReplyWriter& out);

  bool hasPosition() const { return hasPosition_; }
  int32_t latE7() const { return latE7_; }
  int32_t lonE7() const { return lonE7_; }
  uint8_t heading() const { return heading_; }  // 0-15, see MapHeading.h
  uint16_t speedKmh() const { return speedKmh_; }

  // Ladder steps and travel mode, as last set by a command or pushed back by
  // MapActivity. MapActivity reads these after a command lands and applies
  // them; nothing here knows what a step means in pixels or metres.
  uint8_t zoomStep() const { return zoomStep_; }
  uint8_t markerStep() const { return markerStep_; }
  MapRideMode mode() const { return mode_; }
  // Bumped by every command that changes what is on screen. A caller that
  // only redraws on change can compare it; poll() returning true is the
  // simpler way.
  uint32_t seq() const { return seq_; }

  // Optional platform hook for `info`. Left unset (the default) the heap
  // line is simply omitted, which is what the native tests want.
  void setFreeHeapProvider(uint32_t (*provider)()) { freeHeapProvider_ = provider; }

  // Pushed by MapActivity after every viewport reset: what the zoom step it
  // actually rendered at resolves to. `info`'s zoom/lod/mpp lines read this.
  void setZoomInfo(uint8_t zoomStep, uint8_t lod, double mpp) {
    zoomStep_ = zoomStep;
    lod_ = lod;
    mpp_ = mpp;
  }

  // Pushed by MapActivity whenever it changes a ladder itself -- a button
  // press, or a `mode` command restoring that mode's stored steps. Without
  // this the console would report the last value *typed* rather than the one
  // on screen, and the two diverge the moment a button is pressed.
  void setLadders(uint8_t zoomStep, uint8_t markerStep, MapRideMode mode) {
    zoomStep_ = zoomStep;
    markerStep_ = markerStep;
    mode_ = mode;
  }

  // Pushed by MapActivity after every viewport reset -- pos, heading and
  // redraw all end in one. `info`'s tiles_ok/tiles_missing/ways/bytes lines
  // read this. Zero before the first reset, which reads correctly: nothing
  // has been drawn yet.
  void setRenderStats(uint32_t tilesOk, uint32_t tilesMissing, uint32_t ways, uint32_t bytesRead,
                      uint32_t waysFiltered = 0) {
    tilesOk_ = tilesOk;
    tilesMissing_ = tilesMissing;
    ways_ = ways;
    bytesRead_ = bytesRead;
    waysFiltered_ = waysFiltered;
  }

  // Pushed alongside setRenderStats() by the same reset. `tiles` reads this.
  void setTileRange(const MapTileRangeSnapshot& range) { tileRange_ = range; }

 private:
  void writeInfo(IMapReplyWriter& out) const;
  void writeTiles(IMapReplyWriter& out) const;

  bool hasPosition_ = false;
  int32_t latE7_ = 0;
  int32_t lonE7_ = 0;
  uint8_t heading_ = 0;
  uint16_t speedKmh_ = 0;
  uint32_t seq_ = 0;
  uint32_t (*freeHeapProvider_)() = nullptr;

  uint8_t zoomStep_ = 0;
  uint8_t markerStep_ = 0;
  MapRideMode mode_ = MapRideMode::Ride;
  uint8_t lod_ = 0;
  double mpp_ = 0.0;
  uint32_t tilesOk_ = 0;
  uint32_t tilesMissing_ = 0;
  uint32_t ways_ = 0;
  uint32_t waysFiltered_ = 0;
  uint32_t bytesRead_ = 0;
  MapTileRangeSnapshot tileRange_;
};

// Line assembler + parser + a reference to the shared state. One of these
// per channel.
//
// **The assembler is per channel and the state is not.** A UART's half-typed
// line and a BLE write's half-typed line are two different half-typed lines
// and must not interleave into one buffer; the map's zoom, on the other hand,
// is one number whichever channel set it. That split is the whole reason this
// object and MapConsoleState are separate types.
class MapCommandConsole {
 public:
  explicit MapCommandConsole(MapConsoleState& state) : state_(state) {}

  // One received byte. Returns true if a line completed and the screen
  // needs redrawing.
  bool feed(char c, IMapReplyWriter& out);

  MapConsoleState& state() { return state_; }
  const MapConsoleState& state() const { return state_; }

  // Fired the moment a line completes, before it is parsed and before any
  // reply is written. MapSerialConsole hangs a LOG_DBG off it.
  //
  // That is on purpose, not incidental: the log line then lands between the
  // command and its reply, on the same UART, which is exactly the hazard
  // the '<' marker exists to survive. Without it nothing logs in that
  // window and the interleaving is never actually exercised.
  //
  // A plain function pointer, not std::function -- see the firmware
  // CLAUDE.md on std::function's heap and binary cost.
  void setLineObserver(void (*observer)(std::string_view line)) { lineObserver_ = observer; }

 private:
  MapLineAssembler assembler_;
  MapConsoleState& state_;
  void (*lineObserver_)(std::string_view) = nullptr;
};
