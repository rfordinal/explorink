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

// What the commands actually do, and the only thing that knows it. Holds no
// channel and no hardware, so both P3's serial console and P5's BLE
// characteristic run the same lines through the same object and get the
// same behaviour by construction.
//
// P3 implements pos, heading, redraw and info. zoom, marker, mode and tiles
// parse correctly and answer `ERR unimplemented` -- their behaviour is P5
// and P4's job; the grammar is fixed here so it is fixed once.
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
  // Bumped by every command that changes what is on screen. A caller that
  // only redraws on change can compare it; poll() returning true is the
  // simpler way.
  uint32_t seq() const { return seq_; }

  // Optional platform hook for `info`. Left unset (the default) the heap
  // line is simply omitted, which is what the native tests want.
  void setFreeHeapProvider(uint32_t (*provider)()) { freeHeapProvider_ = provider; }

 private:
  void writeInfo(IMapReplyWriter& out) const;

  bool hasPosition_ = false;
  int32_t latE7_ = 0;
  int32_t lonE7_ = 0;
  uint8_t heading_ = 0;
  uint16_t speedKmh_ = 0;
  uint32_t seq_ = 0;
  uint32_t (*freeHeapProvider_)() = nullptr;
};

// Line assembler + parser + state, wired together. Feed it bytes.
class MapCommandConsole {
 public:
  // One received byte. Returns true if a line completed and the screen
  // needs redrawing.
  bool feed(char c, IMapReplyWriter& out);

  MapConsoleState& state() { return state_; }
  const MapConsoleState& state() const { return state_; }

 private:
  MapLineAssembler assembler_;
  MapConsoleState state_;
};
