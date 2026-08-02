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

}  // namespace

MapSerialConsole::MapSerialConsole() { console_.state().setFreeHeapProvider(&freeHeap); }

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
