#include "MapBleConsole.h"

#include <Arduino.h>
#include <BlePositionServer.h>
#include <Logging.h>

namespace {

// Bytes drained per poll(). Same reasoning as the serial console: loop()
// also services buttons and the display, and a sender that writes a block of
// commands gets handled over several loops rather than holding the activity.
constexpr size_t kBytesPerPoll = 128;

class BleReplyWriter final : public IMapReplyWriter {
 public:
  void reply(const char* line) override { freeink::BlePositionServer::getInstance().sendCommandReply(line); }
};

// Same hook the serial console uses, for the same reason: every received
// line is logged before its reply. Here it goes to the UART while the reply
// goes out over BLE, which is exactly what makes a `pio device monitor` a
// usable window onto a BLE session.
//
// %.*s with an explicit length, never .data() as a C string -- a
// string_view is not null-terminated (firmware CLAUDE.md).
void logLine(std::string_view line) {
  (void)line;  // LOG_DBG compiles away entirely in release and slim builds
  LOG_DBG("MAPBLE", "rx: %.*s", static_cast<int>(line.size()), line.data());
}

uint32_t freeHeap() { return static_cast<uint32_t>(ESP.getFreeHeap()); }

}  // namespace

MapBleConsole::MapBleConsole(MapConsoleState& state) : console_(state) {
  // Both channels set this to the same function. Harmless, and it keeps
  // either channel working on its own if the other is ever compiled out.
  console_.state().setFreeHeapProvider(&freeHeap);
  console_.setLineObserver(&logLine);
}

bool MapBleConsole::poll() {
  BleReplyWriter out;
  bool redraw = false;

  char buffer[kBytesPerPoll];
  const size_t count = freeink::BlePositionServer::getInstance().readCommandBytes(buffer, sizeof(buffer));
  for (size_t i = 0; i < count; ++i) {
    if (console_.feed(buffer[i], out)) redraw = true;
  }

  return redraw;
}
