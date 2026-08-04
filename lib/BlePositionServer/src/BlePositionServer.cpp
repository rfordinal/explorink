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
// The map file transfer channel. Same service again -- a phone that already
// found the position service has both of these with it, and the connection
// it is already holding for position updates is the one the file rides on.
constexpr const char* kTransferCharUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0004";
constexpr const char* kTransferStatusCharUuid = "5a1e6d00-73a4-4f1e-9b8f-2c6e1a8f0005";

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_positionChar = nullptr;
NimBLECharacteristic* g_commandChar = nullptr;
NimBLECharacteristic* g_transferChar = nullptr;
NimBLECharacteristic* g_transferStatusChar = nullptr;

// Signaled from CommandCharCallbacks::onStatus when the peer actually
// confirms an indication -- see sendCommandReply for why this matters more
// than indicate()'s own return value.
SemaphoreHandle_t g_indicateAckSem = nullptr;

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

  // Fires once per indicate() call when the peer's ATT-level confirm
  // arrives (or the indication times out) -- `code` is BLE_HS_EDONE on
  // success. Either way the one-indication-in-flight slot is free again.
  void onStatus(NimBLECharacteristic*, NimBLEConnInfo&, int) override {
    if (g_indicateAckSem) xSemaphoreGive(g_indicateAckSem);
  }
};

// The file transfer data channel. No onStatus here on purpose: this
// characteristic never indicates, so it must not touch the command channel's
// one-in-flight semaphore.
//
// This callback runs synchronously inside NimBLE's GATT access handler and the
// ATT write response is sent only once it returns (NimBLEServer.cpp,
// BLE_GATT_ACCESS_OP_WRITE_CHR -> writeEvent). That is what makes the write
// response mean "the bytes are on the card", so a slow SD write here is the
// flow control working, not a stall.
//
// getValue() returns by value, so every chunk costs one malloc/memcpy/free of
// the payload. Known and accepted: the copy is freed before the next one is
// made, so it reuses the same block rather than fragmenting the heap, and it
// is microseconds against a millisecond SD write. NimBLE's protected
// getAttVal() is the no-copy version and is not reachable from here.
class TransferCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue value = characteristic->getValue();
    self().onTransferIngest(value.data(), value.size());
  }
};

// The file transfer status channel. Only job is remembering whether anybody
// is listening -- there is no getSubscribedCount() in NimBLE-Arduino, and a
// transfer must not start with its verdict going nowhere.
class TransferStatusCharCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    // bit1 is indications, bit0 notifications. Only indications count here.
    self().onTransferSubscribe((subValue & 0x0002) != 0);
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  // NimBLE-Arduino stops advertising once a central connects and does not
  // resume automatically -- restart it on disconnect so a dropped link
  // (out of range, phone Bluetooth toggled) can reconnect without a reboot.
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    // Before advertising again: a file transfer in flight is dead the moment
    // the link drops. Resuming across a reconnect is deliberately not built
    // (docs/ble-map-transfer-brief.md, non-goals), so the half-written file
    // has to go now rather than sit on the card looking complete.
    self().onCentralDisconnect();
    self().onTransferSubscribe(false);
    NimBLEDevice::getAdvertising()->start();
  }
};

PositionCharCallbacks g_charCallbacks;
CommandCharCallbacks g_commandCallbacks;
TransferCharCallbacks g_transferCallbacks;
TransferStatusCharCallbacks g_transferStatusCallbacks;
ServerCallbacks g_serverCallbacks;

}  // namespace

bool BlePositionServer::begin(const char* deviceName) {
  LOG_DBG("BLEPOS", "begin: start, begun_=%d, isInitialized=%d", (int)begun_, (int)NimBLEDevice::isInitialized());
  if (begun_) return true;

  // Self-heal a partial teardown, same reasoning as BleKeyboardHost::begin().
  if (NimBLEDevice::isInitialized()) {
    LOG_DBG("BLEPOS", "begin: self-heal deinit");
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    LOG_DBG("BLEPOS", "begin: self-heal deinit done");
  }

  LOG_DBG("BLEPOS", "begin: calling NimBLEDevice::init");
  if (!NimBLEDevice::init(deviceName ? deviceName : "XteinkX4Map")) {
    LOG_DBG("BLEPOS", "begin: NimBLEDevice::init failed");
    return false;
  }
  LOG_DBG("BLEPOS", "begin: NimBLEDevice::init done");

  if (!g_indicateAckSem) {
    g_indicateAckSem = xSemaphoreCreateBinary();
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

  // WRITE carries a command line in, INDICATE carries each reply line out.
  // NOTIFY was tried first and doesn't work for this: a GATT notification is
  // unacknowledged by spec, so a back-to-back burst (info's 17 lines + OK)
  // can queue faster than the connection interval drains it, and the
  // controller is allowed to silently drop the overflow -- notify() still
  // returns true because that return value only means "accepted into the
  // host's send queue", not "the peer got it". Confirmed on real hardware:
  // the host-side retry-on-false loop below saw every single one of 18
  // notify() calls succeed on the first attempt, OK included, while the
  // central still never received it. INDICATE requires the peer to confirm
  // each one before NimBLE will send the next, so the host naturally
  // serializes them and a busy/still-pending indicate() actually returns
  // false for the retry loop to catch. The sender subscribes once and reads
  // replies the same way it would read them off the UART, one line per
  // indication.
  g_commandChar = service->createCharacteristic(
      kCommandCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::INDICATE);
  g_commandChar->setCallbacks(&g_commandCallbacks);

  // The transfer pair. WRITE (with response) in, INDICATE out -- see the
  // TransferHooks comment in BlePositionServer.h for why the write response
  // is the only flow control this needs.
  g_transferChar = service->createCharacteristic(kTransferCharUuid, NIMBLE_PROPERTY::WRITE);
  g_transferChar->setCallbacks(&g_transferCallbacks);
  g_transferStatusChar = service->createCharacteristic(kTransferStatusCharUuid, NIMBLE_PROPERTY::INDICATE);
  g_transferStatusChar->setCallbacks(&g_transferStatusCallbacks);

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
  transferSubscribed_ = false;

  begun_ = true;
  return true;
}

void BlePositionServer::end() {
  if (!begun_) return;
  begun_ = false;

  NimBLEDevice::getAdvertising()->stop();
  g_positionChar = nullptr;
  g_commandChar = nullptr;
  g_transferChar = nullptr;
  g_transferStatusChar = nullptr;
  g_server = nullptr;  // owned by the NimBLE stack; freed by deinit() below

  // Nothing can call a hook once the stack is down, and leaving a pointer to
  // an activity that is about to be deleted registered is how a callback
  // outlives its owner.
  portENTER_CRITICAL(&g_mux);
  transferHooks_ = TransferHooks{};
  portEXIT_CRITICAL(&g_mux);
  transferSubscribed_ = false;

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

  // One indication per line, newline included so a receiver that
  // concatenates them gets the same stream the UART produces.
  char buf[128];
  const int written = snprintf(buf, sizeof(buf), "%s\n", line);
  if (written <= 0) return false;
  const size_t length = static_cast<size_t>(written) < sizeof(buf) ? static_cast<size_t>(written) : sizeof(buf) - 1;

  g_commandChar->setValue(reinterpret_cast<const uint8_t*>(buf), length);

  // Confirmed on real hardware: indicate() returning true only means this
  // indication was accepted into NimBLE's single-slot pending queue, not
  // that the peer received it -- a burst of 18 back-to-back indicate()
  // calls (info's 17 lines + OK) all returned true within about 3ms, yet
  // the phone-equivalent client only ever saw the first and the last one.
  // Each new call overwrites whatever the previous one queued before it
  // could even be sent. The actual per-indication confirm comes later, out
  // of band, via CommandCharCallbacks::onStatus -- so wait for that
  // signal before returning, which serializes replies at the pace the
  // link can really deliver them instead of the pace we can call indicate().
  constexpr int kMaxAttempts = 40;
  constexpr uint32_t kRetryDelayMs = 25;
  constexpr uint32_t kConfirmTimeoutMs = 500;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    // Drain any stale give() (e.g. a previous reply's confirm landing after
    // its own wait already timed out) so this wait can't return instantly
    // on a signal that isn't for this indication.
    xSemaphoreTake(g_indicateAckSem, 0);
    if (g_commandChar->indicate()) {
      xSemaphoreTake(g_indicateAckSem, pdMS_TO_TICKS(kConfirmTimeoutMs));
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
  }
  LOG_ERR("BLEPOS", "reply indicate failed after %d attempts: %.*s", kMaxAttempts, static_cast<int>(length), buf);
  return false;
}

void BlePositionServer::setTransferHooks(const TransferHooks& hooks) {
  portENTER_CRITICAL(&g_mux);
  transferHooks_ = hooks;
  portEXIT_CRITICAL(&g_mux);
}

void BlePositionServer::onTransferSubscribe(bool subscribed) {
  transferSubscribed_ = subscribed;
  LOG_DBG("BLEPOS", "transfer status %s", subscribed ? "subscribed" : "unsubscribed");
}

void BlePositionServer::onTransferIngest(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;

  // Copied out under the lock, called outside it: the hook writes to the SD
  // card, which takes a mutex, and taking a mutex inside a critical section
  // is a crash.
  portENTER_CRITICAL(&g_mux);
  const TransferHooks hooks = transferHooks_;
  portEXIT_CRITICAL(&g_mux);

  if (hooks.onFrame) hooks.onFrame(hooks.ctx, data, len);
}

void BlePositionServer::onCentralDisconnect() {
  portENTER_CRITICAL(&g_mux);
  const TransferHooks hooks = transferHooks_;
  portEXIT_CRITICAL(&g_mux);

  if (hooks.onDisconnect) hooks.onDisconnect(hooks.ctx);
}

bool BlePositionServer::sendTransferStatus(const char* line) {
  if (!begun_ || g_transferStatusChar == nullptr || line == nullptr) return false;

  // Status lines are short by design (a verdict and two numbers), so this
  // buffer is generous. Same newline as the command channel so a receiver
  // that concatenates indications reads a normal line stream.
  char buf[64];
  const int written = snprintf(buf, sizeof(buf), "%s\n", line);
  if (written <= 0) return false;
  const size_t length = static_cast<size_t>(written) < sizeof(buf) ? static_cast<size_t>(written) : sizeof(buf) - 1;

  // No confirm wait -- see the header. A retry still helps: indicate()
  // returns false while some other indication is pending, and the only other
  // indicating characteristic is the command channel, which a script that is
  // pushing a file is not using at that moment.
  constexpr int kMaxAttempts = 8;
  constexpr uint32_t kRetryDelayMs = 25;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (g_transferStatusChar->indicate(reinterpret_cast<const uint8_t*>(buf), length)) return true;
    vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
  }
  LOG_ERR("BLEPOS", "transfer status indicate failed after %d attempts: %.*s", kMaxAttempts, static_cast<int>(length),
          buf);
  return false;
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
void BlePositionServer::setTransferHooks(const TransferHooks&) {}
bool BlePositionServer::sendTransferStatus(const char*) { return false; }
void BlePositionServer::onTransferIngest(const uint8_t*, size_t) {}
void BlePositionServer::onTransferSubscribe(bool) {}
void BlePositionServer::onCentralDisconnect() {}

}  // namespace freeink

#endif  // FREEINK_CAP_BLE_PERIPHERAL
