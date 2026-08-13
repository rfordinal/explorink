#include "MapBleConsole.h"

#include <Arduino.h>
#include <BlePositionServer.h>
#include <Logging.h>

#include <cstring>

namespace {

// Bytes drained per poll(). Same reasoning as the serial console: loop()
// also services buttons and the display, and a sender that writes a block of
// commands gets handled over several loops rather than holding the activity.
constexpr size_t kBytesPerPoll = 128;

}  // namespace

// Collects reply lines into MapBleConsole's batch buffer instead of putting
// each one on the link by itself. One indication per line is what made a
// multi-line reply lossy -- see MapBleConsole.h and BlePositionServer's
// sendCommandBlock.
class BatchingReplyWriter final : public IMapReplyWriter {
 public:
  explicit BatchingReplyWriter(MapBleConsole& owner) : owner_(owner) {}
  void reply(const char* line) override { owner_.appendReply(line); }

 private:
  MapBleConsole& owner_;
};

namespace {

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

void MapBleConsole::appendReply(const char* line) {
  if (line == nullptr) return;
  const size_t lineLen = strlen(line);
  if (lineLen == 0) return;

  auto& ble = freeink::BlePositionServer::getInstance();
  size_t budget = ble.commandPayloadBytes();
  if (budget > kBatchBytes) budget = kBatchBytes;

  // Longer than one indication can carry: nothing to pack it with, so send it
  // alone and let sendCommandReply's own 128-byte buffer truncate it the way
  // it always has.
  if (lineLen + 1 > budget) {
    flushReplies();
    ble.sendCommandReply(line);
    return;
  }

  if (batchLen_ + lineLen + 1 > budget) flushReplies();
  memcpy(batch_ + batchLen_, line, lineLen);
  batchLen_ += lineLen;
  batch_[batchLen_++] = '\n';
}

bool MapBleConsole::flushReplies() {
  if (batchLen_ == 0) return true;
  const size_t len = batchLen_;
  // Cleared before the send, not after: an unconfirmed indication must not
  // leave the same bytes queued to go out again on the next flush.
  batchLen_ = 0;
  return freeink::BlePositionServer::getInstance().sendCommandBlock(batch_, len);
}

bool MapBleConsole::poll() {
  BatchingReplyWriter out(*this);
  bool redraw = false;

  char buffer[kBytesPerPoll];
  const size_t count = freeink::BlePositionServer::getInstance().readCommandBytes(buffer, sizeof(buffer));
  for (size_t i = 0; i < count; ++i) {
    if (console_.feed(buffer[i], out)) redraw = true;
  }

  // Whatever the last command produced and did not fill a batch with. Done
  // here rather than per command so a reply of five short lines is one
  // indication, which is the whole point.
  flushReplies();

  return redraw;
}
