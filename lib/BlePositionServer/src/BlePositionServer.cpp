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

#include <NimBLEDevice.h>

#include <cstring>

namespace freeink {
namespace {

// Custom 128-bit UUIDs generated for this project (not an upstream FreeInk
// or Bluetooth SIG UUID) -- unique enough for a short-range, single-purpose
// link; the Android app must use these same two values.
constexpr const char* kServiceUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0001";
constexpr const char* kPositionCharUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0002";

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_positionChar = nullptr;

BlePositionServer& self() { return BlePositionServer::getInstance(); }

class PositionCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue value = characteristic->getValue();
    self().onWriteIngest(value.data(), value.size());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  // NimBLE-Arduino stops advertising once a central connects and does not
  // resume automatically -- restart it on disconnect so a dropped link
  // (out of range, phone Bluetooth toggled) can reconnect without a reboot.
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override { NimBLEDevice::getAdvertising()->start(); }
};

PositionCharCallbacks g_charCallbacks;
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

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->start();

  portENTER_CRITICAL(&g_mux);
  hasUpdate_ = false;
  latest_ = PositionUpdate{};
  portEXIT_CRITICAL(&g_mux);

  begun_ = true;
  return true;
}

void BlePositionServer::end() {
  if (!begun_) return;
  begun_ = false;

  NimBLEDevice::getAdvertising()->stop();
  g_positionChar = nullptr;
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
  // Fixed 12-byte payload (see BlePositionServer.h) -- ignore anything else
  // rather than guess at a partial/malformed write.
  if (!data || len < 12) return;

  PositionUpdate update;
  memcpy(&update.lat, data + 0, sizeof(update.lat));
  memcpy(&update.lon, data + 4, sizeof(update.lon));
  update.heading = data[8];
  update.seq = data[9];
  update.flags = data[10];
  // data[11] reserved, ignored.

  portENTER_CRITICAL(&g_mux);
  latest_ = update;
  hasUpdate_ = true;
  portEXIT_CRITICAL(&g_mux);
}

}  // namespace freeink

#else  // !FREEINK_CAP_BLE_PERIPHERAL -- stub bodies, no BLE code linked.

namespace freeink {

bool BlePositionServer::begin(const char*) { return false; }
void BlePositionServer::end() {}
bool BlePositionServer::getLatest(PositionUpdate&) const { return false; }
void BlePositionServer::onWriteIngest(const uint8_t*, size_t) {}

}  // namespace freeink

#endif  // FREEINK_CAP_BLE_PERIPHERAL
