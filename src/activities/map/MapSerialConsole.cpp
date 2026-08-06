#include "MapSerialConsole.h"

#include <Arduino.h>
#include <Logging.h>

namespace {

// Bytes drained per poll(). loop() also services buttons, BLE and the
// display, so a host that pastes a large block gets handled over several
// loops instead of holding the activity for the whole paste.
constexpr int kBytesPerPoll = 256;

class SerialReplyWriter final : public IMapReplyWriter {
 public:
  void reply(const char* line) override {
    logSerial.print(MapSerialConsole::kReplyMarker);
    logSerial.println(line);
  }
};

uint32_t freeHeap() { return static_cast<uint32_t>(ESP.getFreeHeap()); }

// Fires after the line is complete and before its reply is written, so this
// lands between the command and its answer on the shared UART. Deliberate:
// it is what makes the '<' marker do work on every single command instead
// of only when some other subsystem happens to log at the right moment.
//
// %.*s with an explicit length, never .data() as a C string -- a
// string_view is not null-terminated (firmware CLAUDE.md).
void logLine(std::string_view line) {
  (void)line;  // LOG_DBG compiles away entirely in release and slim builds
  LOG_DBG("MAPCON", "rx: %.*s", static_cast<int>(line.size()), line.data());
}

}  // namespace

MapSerialConsole::MapSerialConsole(MapConsoleState& state) : console_(state) {
  console_.state().setFreeHeapProvider(&freeHeap);
  console_.setLineObserver(&logLine);
}

bool MapSerialConsole::poll() {
  SerialReplyWriter out;
  bool redraw = false;

  for (int i = 0; i < kBytesPerPoll && logSerial.available() > 0; ++i) {
    // Leave the "CMD:" namespace to main.cpp's handler.
    //
    // main.cpp guards its own side by peeking for 'C' before it consumes
    // anything (src/main.cpp, the serial command block) -- but that guard only
    // helps if a whole line is already buffered when its turn comes. This
    // console feeds byte by byte, so with bytes trickling in off USB it starts
    // consuming a line before main.cpp ever sees a 'C' at the head, and then
    // answers "<ERR unknown_command" to a command that was never addressed to
    // it. Measured 2026-08-06: CMD:SCREENSHOT and CMD:GOTO_MAP both landed in
    // MAPCON while the map screen was up, so no screenshot could be taken of
    // the one screen worth screenshotting.
    //
    // Only at a line boundary: mid-line a 'C' is just a character. No map
    // command starts with 'C' (MapCommandParser.h), so nothing legitimate is
    // withheld, and main.cpp consumes a stray 'C'-headed line either way --
    // it reads the line and discards it when the prefix does not match -- so
    // this cannot deadlock the port.
    if (atLineStart_ && logSerial.peek() == 'C') break;
    const int byte = logSerial.read();
    if (byte < 0) break;
    atLineStart_ = (byte == '\n' || byte == '\r');
    if (console_.feed(static_cast<char>(byte), out)) redraw = true;
  }

  return redraw;
}
