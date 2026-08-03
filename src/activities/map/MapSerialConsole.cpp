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
    const int byte = logSerial.read();
    if (byte < 0) break;
    if (console_.feed(static_cast<char>(byte), out)) redraw = true;
  }

  return redraw;
}
