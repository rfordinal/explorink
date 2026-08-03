#include "BlePositionServer.h"

#ifndef FREEINK_CAP_BLE_PERIPHERAL
#define FREEINK_CAP_BLE_PERIPHERAL 0
#endif

namespace freeink {

BlePositionServer& BlePositionServer::getInstance() {
  static BlePositionServer instance;
  return instance;
}

}  // namespace freeink

#if FREEINK_CAP_BLE_PERIPHERAL

#include <Logging.h>
#include <NimBLEDevice.h>

#include <cstdio>
#include <cstring>

namespace freeink {
namespace {

// Custom 128-bit UUIDs generated for this project (not an upstream FreeInk
// or Bluetooth SIG UUID) -- unique enough for a short-range, single-purpose
// link; the Android app must use these same two values.
constexpr const char* kServiceUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0001";
constexpr const char* kPositionCharUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0002";
// The command channel (P5). Same service, second characteristic -- a phone
// that already found the position service finds this one with it.
constexpr const char* kCommandCharUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0003";

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_positionChar = nullptr;
NimBLECharacteristic* g_commandChar = nullptr;

BlePositionServer& self() { return BlePositionServer::getInstance(); }

class PositionCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue value = characteristic->getValue();
    self().onWriteIngest(value.data(), value.size());
  }
};

class CommandCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue value = characteristic->getValue();
    self().onCommandIngest(value.data(), value.size());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  // NimBLE-Arduino stops advertising once a central connects and does not
  // resume automatically -- restart it on disconnect so a dropped link
  // (out of range, phone Bluetooth toggled) can reconnect without a reboot.
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override { NimBLEDevice::getAdvertising()->start(); }
};

PositionCharCallbacks g_charCallbacks;
CommandCharCallbacks g_commandCallbacks;
ServerCallbacks g_serverCallbacks;

}  // namespace

bool BlePositionServer::begin(const char* deviceName) {
  if (begun_) return true;

  // Self-heal a partial teardown, same reasoning as BleKeyboardHost::begin().
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (!NimBLEDevice::init(deviceName ? deviceName : "XteinkX4Map")) {
    return false;
  }

  // No pairing/bonding -- short range (~0.5-1m, see architecture-plan.md),
  // low-stakes write-only channel. Keeping the phone-side BLE code to
  // "connect + write" (no pairing UI/passkey flow) is worth more here than
  // link security.
  NimBLEDevice::setSecurityAuth(/*bonding=*/false, /*mitm=*/false, /*sc=*/false);

  g_server = NimBLEDevice::createServer();
  if (!g_server) {
    NimBLEDevice::deinit(true);
    return false;
  }
  g_server->setCallbacks(&g_serverCallbacks, /*deleteCallbacks=*/false);

  // NimBLEService::start() is deprecated -- services start implicitly when
  // the server starts (i.e. once advertising begins below).
  NimBLEService* service = g_server->createService(kServiceUuid);
  g_positionChar = service->createCharacteristic(kPositionCharUuid, NIMBLE_PROPERTY::WRITE);
  g_positionChar->setCallbacks(&g_charCallbacks);

  // WRITE carries a command line in, NOTIFY carries each reply line out. The
  // sender subscribes once and reads replies the same way it would read them
  // off the UART, one line per notification.
  g_commandChar = service->createCharacteristic(kCommandCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  g_commandChar->setCallbacks(&g_commandCallbacks);

  // Explicit, though NimBLEAdvertising::start() would call it: the GATT table
  // is built here, and building it before advertising is announced is the
  // difference between a central seeing both characteristics and seeing
  // whatever the previous session registered.
  if (!g_server->start()) {
    LOG_ERR("BLEPOS", "GATT server failed to start");
    NimBLEDevice::deinit(true);
    return false;
  }

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->start();

  portENTER_CRITICAL(&g_mux);
  hasUpdate_ = false;
  latest_ = PositionUpdate{};
  commandHead_ = 0;
  commandTail_ = 0;
  portEXIT_CRITICAL(&g_mux);

  begun_ = true;
  return true;
}

void BlePositionServer::end() {
  if (!begun_) return;
  begun_ = false;

  NimBLEDevice::getAdvertising()->stop();
  g_positionChar = nullptr;
  g_commandChar = nullptr;
  g_server = nullptr;  // owned by the NimBLE stack; freed by deinit() below

  // Return the NimBLE host + BT controller RAM to the heap. Retry once if
  // the stack didn't fully tear down, same pattern as BleKeyboardHost::end().
  NimBLEDevice::deinit(true);
  if (NimBLEDevice::isInitialized()) {
    vTaskDelay(pdMS_TO_TICKS(50));
    NimBLEDevice::deinit(true);
  }

  portENTER_CRITICAL(&g_mux);
  hasUpdate_ = false;
  commandHead_ = 0;
  commandTail_ = 0;
  portEXIT_CRITICAL(&g_mux);
}

bool BlePositionServer::getLatest(PositionUpdate& out) const {
  bool got = false;
  portENTER_CRITICAL(&g_mux);
  if (hasUpdate_) {
    out = latest_;
    got = true;
  }
  portEXIT_CRITICAL(&g_mux);
  return got;
}

void BlePositionServer::onWriteIngest(const uint8_t* data, size_t len) {
  // Fixed 19-byte payload (see BlePositionServer.h) -- ignore anything else
  // rather than guess at a partial/malformed write. A 12-byte packet from the
  // old format lands here and is dropped, deliberately.
  if (!data || len < kPositionPacketBytes) return;

  // memcpy for every multi-byte field: `data` is a NimBLE attribute buffer
  // with no alignment guarantee, and the ESP32-C3 faults on an unaligned
  // wide load (firmware CLAUDE.md, "RISC-V Alignment").
  PositionUpdate update;
  memcpy(&update.lat, data + 0, sizeof(update.lat));
  memcpy(&update.lon, data + 4, sizeof(update.lon));
  memcpy(&update.utc, data + 8, sizeof(update.utc));
  memcpy(&update.tzOffsetMin, data + 12, sizeof(update.tzOffsetMin));
  update.heading = data[14];
  update.seq = data[15];
  update.flags = data[16];
  update.accuracyM = data[17];
  update.speedKmh = data[18];

  portENTER_CRITICAL(&g_mux);
  latest_ = update;
  hasUpdate_ = true;
  portEXIT_CRITICAL(&g_mux);
}

void BlePositionServer::onCommandIngest(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;

  portENTER_CRITICAL(&g_mux);
  const size_t head = commandHead_;
  const size_t tail = commandTail_;
  // One slot is always left empty so head == tail can only ever mean empty.
  const size_t free = (tail + kCommandRingBytes - head - 1) % kCommandRingBytes;
  if (len <= free) {
    for (size_t i = 0; i < len; ++i) {
      commandRing_[(head + i) % kCommandRingBytes] = static_cast<char>(data[i]);
    }
    commandHead_ = (head + len) % kCommandRingBytes;
  }
  // Else: dropped whole. Writing the part that fits would leave a truncated
  // command in the ring, and a truncated command can still parse.
  portEXIT_CRITICAL(&g_mux);
}

size_t BlePositionServer::readCommandBytes(char* out, size_t max) {
  if (!out || max == 0) return 0;

  size_t count = 0;
  portENTER_CRITICAL(&g_mux);
  size_t tail = commandTail_;
  const size_t head = commandHead_;
  while (count < max && tail != head) {
    out[count++] = commandRing_[tail];
    tail = (tail + 1) % kCommandRingBytes;
  }
  commandTail_ = tail;
  portEXIT_CRITICAL(&g_mux);
  return count;
}

bool BlePositionServer::sendCommandReply(const char* line) {
  if (!begun_ || g_commandChar == nullptr || line == nullptr) return false;

  // One notification per line, newline included so a receiver that
  // concatenates them gets the same stream the UART produces.
  char buf[128];
  const int written = snprintf(buf, sizeof(buf), "%s\n", line);
  if (written <= 0) return false;
  const size_t length = static_cast<size_t>(written) < sizeof(buf) ? static_cast<size_t>(written) : sizeof(buf) - 1;

  g_commandChar->setValue(reinterpret_cast<const uint8_t*>(buf), length);
  g_commandChar->notify();
  return true;
}

}  // namespace freeink

#else  // !FREEINK_CAP_BLE_PERIPHERAL -- stub bodies, no BLE code linked.

namespace freeink {

bool BlePositionServer::begin(const char*) { return false; }
void BlePositionServer::end() {}
bool BlePositionServer::getLatest(PositionUpdate&) const { return false; }
void BlePositionServer::onWriteIngest(const uint8_t*, size_t) {}
void BlePositionServer::onCommandIngest(const uint8_t*, size_t) {}
size_t BlePositionServer::readCommandBytes(char*, size_t) { return 0; }
bool BlePositionServer::sendCommandReply(const char*) { return false; }

}  // namespace freeink

#endif  // FREEINK_CAP_BLE_PERIPHERAL
