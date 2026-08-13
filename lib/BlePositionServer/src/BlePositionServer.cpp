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
#include <esp_system.h>    // esp_get_free_heap_size() -- the begin()/end() heap bracket below
#include <host/ble_gap.h>  // ble_gap_conn_rssi() -- no NimBLEServer/NimBLEConnInfo wrapper exists for it

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

  // Whether anybody is listening at all. Same reason the status channel tracks
  // it (below): NimBLE-Arduino has no getSubscribedCount(), and indicate()
  // succeeds into an empty room, so this is the only way a screen can tell a
  // connected phone from no phone.
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    // bit1 is indications, bit0 notifications. **Either one counts.**
    //
    // This characteristic offers both (see createCharacteristic below), and a
    // central picks. BlueZ picks notify, so every Linux host does; the Android
    // app happens to pick indicate. Counting only indications therefore meant a
    // whole class of perfectly working clients was invisible: replies reached
    // them (onStatus fires for a notification too, so sendCommandReply's
    // confirm wait returns normally), but isCommandSubscribed() answered false,
    // and every unsolicited ask -- NEED_TILES from the sync screen and from the
    // map's autosync -- was silently withheld.
    //
    // Measured on hardware 2026-08-07: a bleak client subscribed, read the
    // viewport with `tiles` and got all four reply lines, while the device
    // logged "command channel unsubscribed" and never asked for anything.
    self().onCommandSubscribe((subValue & 0x0003) != 0);
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
    // bit1 is indications, bit0 notifications. Only indications count here,
    // and unlike the command channel that is not a choice: this characteristic
    // is created INDICATE-only, so notify is not a subscription a central can
    // even make.
    self().onTransferSubscribe((subValue & 0x0002) != 0);
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  // What the link actually negotiated. Every throughput number for the transfer
  // channel depends on these two and neither is knowable from a return value --
  // the central drives both (docs/optimization/03-ble-link.md, step 1).
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    self().onConnIntervalChanged(info.getConnInterval());
    self().onConnHandleChanged(info.getConnHandle());
    // A central got in, so advertising was up whatever the last start() said.
    // NimBLE stops advertising for the duration of the connection by design,
    // and that is not a failure to retry.
    self().onAdvertisingState(true);
    // Interval is in 1.25 ms units.
    LOG_INF("BLEPOS", "connected: interval %u units (%u ms), latency %u, timeout %u",
            static_cast<unsigned>(info.getConnInterval()), static_cast<unsigned>(info.getConnInterval() * 5 / 4),
            static_cast<unsigned>(info.getConnLatency()), static_cast<unsigned>(info.getConnTimeout()));
  }

  // The central can change the interval mid-connection, and it does: the phone
  // app asks for a fast one while a tile sync runs and gives it back afterwards
  // (android/README.md). Without this hook the log only ever showed the interval
  // agreed at connect, so there was no way to tell whether that request landed --
  // and the interval, not the MTU, is what caps the transfer.
  void onConnParamsUpdate(NimBLEConnInfo& info) override {
    self().onConnIntervalChanged(info.getConnInterval());
    LOG_INF("BLEPOS", "conn params: interval %u units (%u ms), latency %u, timeout %u",
            static_cast<unsigned>(info.getConnInterval()), static_cast<unsigned>(info.getConnInterval() * 5 / 4),
            static_cast<unsigned>(info.getConnLatency()), static_cast<unsigned>(info.getConnTimeout()));
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    self().onMtuChanged(mtu);
    // The second number is the one that matters: 3 bytes of ATT header plus the
    // 5-byte chunk header come off every write, so a 23-byte MTU carries 15
    // bytes of file per transaction and a 256-byte one carries 248.
    LOG_INF("BLEPOS", "MTU now %u, file payload %u bytes per chunk", static_cast<unsigned>(mtu),
            static_cast<unsigned>(mtu > 8 ? mtu - 8 : 0));
  }

  // NimBLE-Arduino stops advertising once a central connects and does not
  // resume automatically -- restart it on disconnect so a dropped link
  // (out of range, phone Bluetooth toggled) can reconnect without a reboot.
  //
  // start() can fail (e.g. transient "host not synced" right after a link
  // drop) and an unchecked call is how BLE goes silently, permanently deaf --
  // a mid-ride incident showed exactly that while the rest of the device
  // stayed responsive (docs/power-management.md, "BLE stopped accepting
  // connections mid-ride").
  //
  // **One attempt, no retry loop, nothing blocking.** This method runs on the
  // NimBLE host task, and the sync event that clears the most likely failure
  // cause ("host not synced", NimBLEAdvertising.cpp:189-192) is dispatched on
  // that same task. The 5x50 ms vTaskDelay loop this replaces therefore
  // blocked the very event it was waiting for -- all five attempts failed by
  // construction and nothing retried afterwards, so the radio stayed dead
  // until the rider left the screen and came back
  // (docs/ble-review-2026-08.md item 3). A failure is handed to the activity
  // task instead, via BlePositionServer::serviceAdvertising().
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    // Before advertising again: a file transfer in flight is dead the moment
    // the link drops. Resuming across a reconnect is deliberately not built
    // (docs/ble-map-transfer-brief.md, non-goals), so the half-written file
    // has to go now rather than sit on the card looking complete.
    self().onCentralDisconnect();
    self().onTransferSubscribe(false);

    self().onAdvertisingState(NimBLEDevice::getAdvertising()->start());
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

  // The NimBLE host + BT controller are the single biggest heap consumer on the
  // map screen: measured on hardware 2026-08-10, entering the map costs 75 KB of
  // a 124 KB free heap and only 7.7 KB of that is the tile source
  // (docs/map-memory.md). This bracket is what splits the two, so the split is a
  // number instead of a subtraction. Same reasoning as MapActivity's own
  // before/after log around the tile source allocation.
  const uint32_t heapBeforeBegin = esp_get_free_heap_size();

  // Self-heal a partial teardown, same reasoning as BleKeyboardHost::begin().
  if (NimBLEDevice::isInitialized()) {
    LOG_DBG("BLEPOS", "begin: self-heal deinit");
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    LOG_DBG("BLEPOS", "begin: self-heal deinit done");
  }

  LOG_DBG("BLEPOS", "begin: calling NimBLEDevice::init");
  if (!NimBLEDevice::init(deviceName ? deviceName : kBleDeviceName)) {
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
  // The name goes in the scan response, not the advertisement, and it has to be
  // set here rather than relying on init(): NimBLEDevice::init()'s name only
  // reaches the GAP Device Name characteristic (readable after connecting), and
  // NimBLE 2.x defaults scan response off (NimBLEAdvertising.cpp:44). So before
  // this the device advertised flags plus one service UUID and no name at all --
  // and the phone's name filter (BleLink.kt, ScanFilter.setDeviceName) could
  // never match. Scan response rather than the advertisement because the payload
  // is full: flags (3 B) plus a 128-bit service UUID (18 B) is 21 of 31 bytes and
  // the name needs 13 more. An active scan reads the scan response, which is what
  // Android does. Why it matters: the phone app wakes itself when this screen
  // opens, and the CompanionDeviceManager association dialog names the device to
  // the rider off this string (../../docs/ble-app-wake.md in the parent repo).
  advertising->enableScanResponse(true);
  advertising->setName(deviceName ? deviceName : kBleDeviceName);
  if (!advertising->start()) {
    LOG_ERR("BLEPOS", "advertising failed to start");
    NimBLEDevice::deinit(true);
    return false;
  }

  portENTER_CRITICAL(&g_mux);
  hasUpdate_ = false;
  latest_ = PositionUpdate{};
  commandHead_ = 0;
  commandTail_ = 0;
  portEXIT_CRITICAL(&g_mux);
  transferSubscribed_ = false;
  commandSubscribed_ = false;
  // Advertising is up as of the start() above, so no retry is owed. Cleared
  // here rather than trusted from the last session: end() and begin() can be
  // seconds apart across a screen exit and re-entry.
  advertisingDown_ = false;
  lastAdvertisingAttemptMs_ = 0;

  begun_ = true;
  const uint32_t heapAfterBegin = esp_get_free_heap_size();
  LOG_INF("BLEPOS", "heap: %lu before begin, %lu after, delta %ld", (unsigned long)heapBeforeBegin,
          (unsigned long)heapAfterBegin, (long)heapBeforeBegin - (long)heapAfterBegin);
  return true;
}

void BlePositionServer::end() {
  if (!begun_) return;
  begun_ = false;

  // The other half of begin()'s bracket. The deinit below claims to return the
  // host + controller RAM; this is what checks that it does.
  const uint32_t heapBeforeEnd = esp_get_free_heap_size();

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
  commandSubscribed_ = false;
  // Nothing is advertising once the stack is down, so there is nothing to be
  // down about. Left set, the flag would be the first thing a re-entered
  // screen acted on -- against a stack that had not been built yet.
  advertisingDown_ = false;

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

  const uint32_t heapAfterEnd = esp_get_free_heap_size();
  LOG_INF("BLEPOS", "heap: %lu before end, %lu after, returned %ld", (unsigned long)heapBeforeEnd,
          (unsigned long)heapAfterEnd, (long)heapAfterEnd - (long)heapBeforeEnd);
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
  // Fixed 21-byte payload (see BlePositionServer.h) -- ignore anything else
  // rather than guess at a partial/malformed write. A 12-byte or 19-byte
  // packet from an older format lands here and is dropped, deliberately.
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
  memcpy(&update.altitudeM, data + 19, sizeof(update.altitudeM));
  update.hasAltitude = (update.flags & 0x02) != 0;

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
  if (line == nullptr) return false;

  // Newline included so a receiver that concatenates indications gets the same
  // stream the UART produces.
  char buf[128];
  const int written = snprintf(buf, sizeof(buf), "%s\n", line);
  if (written <= 0) return false;
  const size_t length = static_cast<size_t>(written) < sizeof(buf) ? static_cast<size_t>(written) : sizeof(buf) - 1;
  return sendCommandBlock(buf, length);
}

bool BlePositionServer::sendCommandBlock(const char* text, size_t len) {
  if (!begun_ || g_commandChar == nullptr || text == nullptr || len == 0) return false;

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
  //
  // **A confirm that does not arrive is a failure, not a pause.** The wait used
  // to be 500 ms and returned `true` either way, which put the clobbering
  // straight back: the next line overwrote a slot the peer had not drained.
  // Measured 2026-08-13 against the Android app -- a five-line `have` reply
  // took 3.9 s (six waits timing out at 500 ms each) and the phone received
  // one of the four tile lines. It then reported "0 stale of 1" for a viewport
  // holding two tiles that were out of date, and the freshness check had been
  // silently answering about a fraction of the screen ever since.
  constexpr int kMaxAttempts = 40;
  constexpr uint32_t kRetryDelayMs = 25;
  // Long enough for a phone that is also servicing position writes and a tile
  // transfer on the same link. The old 500 ms was under what this Android
  // build actually takes.
  constexpr uint32_t kConfirmTimeoutMs = 3000;
  // Above this, say so: the number is a link property nothing else reports,
  // and the batching below is sized against it.
  constexpr uint32_t kSlowConfirmMs = 300;

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    // Drain any stale give() (e.g. a previous reply's confirm landing after
    // its own wait already timed out) so this wait can't return instantly
    // on a signal that isn't for this indication.
    xSemaphoreTake(g_indicateAckSem, 0);
    // indicate(data, len) copies into NimBLE's own indication buffer; the
    // no-arg indicate() sends m_value, which this characteristic is also
    // peer-writable into, so a command crossing the reply between
    // setValue() and indicate() would replace it (review doc
    // docs/ble-review-2026-08.md item 6).
    if (g_commandChar->indicate(reinterpret_cast<const uint8_t*>(text), len)) {
      const uint32_t startedMs = millis();
      if (xSemaphoreTake(g_indicateAckSem, pdMS_TO_TICKS(kConfirmTimeoutMs)) != pdTRUE) {
        // Not retried: the peer may still be about to take this one, and
        // sending it again would put a duplicate line into a listing whose
        // whole point is a count that adds up.
        LOG_ERR("BLEPOS", "reply unconfirmed after %lu ms, %u bytes dropped",
                static_cast<unsigned long>(kConfirmTimeoutMs), static_cast<unsigned>(len));
        return false;
      }
      const uint32_t waitedMs = millis() - startedMs;
      if (waitedMs >= kSlowConfirmMs) {
        LOG_DBG("BLEPOS", "reply confirm took %lu ms for %u bytes", static_cast<unsigned long>(waitedMs),
                static_cast<unsigned>(len));
      }
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
  }
  LOG_ERR("BLEPOS", "reply indicate failed after %d attempts: %.*s", kMaxAttempts, static_cast<int>(len), text);
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

void BlePositionServer::onMtuChanged(uint16_t mtu) { mtu_ = mtu; }

void BlePositionServer::onConnIntervalChanged(uint16_t units) { connIntervalUnits_ = units; }

void BlePositionServer::onConnHandleChanged(uint16_t connHandle) { connHandle_ = connHandle; }

// Read fresh, not cached: the last connection's RSSI is not a fact about the
// current one, and this is a single HCI round trip, not a channel worth
// polling and storing every frame.
int8_t BlePositionServer::rssi() const {
  if (connHandle_ == BLE_HS_CONN_HANDLE_NONE) return 0;
  int8_t value = 0;
  return ble_gap_conn_rssi(connHandle_, &value) == 0 ? value : 0;
}

void BlePositionServer::onCommandSubscribe(bool subscribed) {
  commandSubscribed_ = subscribed;
  LOG_DBG("BLEPOS", "command channel %s", subscribed ? "subscribed" : "unsubscribed");
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
  // A subscription belongs to a connection, and NimBLE does not fire
  // onSubscribe(0) when the link simply drops. Left set, a screen waiting for a
  // phone would go on believing one is there long after it walked away.
  commandSubscribed_ = false;
  transferSubscribed_ = false;
  // The MTU belongs to the connection too -- reporting the last link's number
  // while nothing is connected is how a stale figure ends up in a bug report.
  mtu_ = 0;
  connIntervalUnits_ = 0;
  connHandle_ = BLE_HS_CONN_HANDLE_NONE;

  portENTER_CRITICAL(&g_mux);
  const TransferHooks hooks = transferHooks_;
  portEXIT_CRITICAL(&g_mux);

  if (hooks.onDisconnect) hooks.onDisconnect(hooks.ctx);
}

void BlePositionServer::onAdvertisingState(bool up) {
  const bool wasDown = advertisingDown_.exchange(!up);
  if (up) return;

  // Stamped here so serviceAdvertising()'s first retry lands a full
  // kAdvertisingRetryMs after this failed attempt, not on the next loop() tick.
  lastAdvertisingAttemptMs_ = millis();
  // Once per failure, not once per retry. retryAdvertising() logs its own
  // failures at DBG, so a radio that is dead for good costs one line, not one
  // line per second for the rest of the ride.
  if (!wasDown) {
    LOG_ERR("BLEPOS", "advertising restart failed; the activity task will retry every %lu ms",
            static_cast<unsigned long>(kAdvertisingRetryMs));
  }
}

void BlePositionServer::retryAdvertising() {
  if (!begun_ || !advertisingDown_) return;

  const uint32_t now = millis();
  // Unsigned wrap is the intended arithmetic: millis() rolls over after ~49
  // days and the difference stays correct across the roll.
  if (now - lastAdvertisingAttemptMs_ < kAdvertisingRetryMs) return;
  lastAdvertisingAttemptMs_ = now;

  if (!NimBLEDevice::getAdvertising()->start()) {
    LOG_DBG("BLEPOS", "advertising retry failed, trying again in %lu ms",
            static_cast<unsigned long>(kAdvertisingRetryMs));
    return;
  }
  onAdvertisingState(true);
  LOG_INF("BLEPOS", "advertising restarted from the activity task");
}

void BlePositionServer::serviceAdvertising() {
  if (!begun_) return;
  // A live link means advertising already did its job, and NimBLE stops it
  // while a central is connected by design -- retrying into a connection would
  // be restarting something that is deliberately off. connIntervalUnits_ is
  // the same "is the phone there" test the map header draws with
  // (src/activities/map/MapActivity.cpp:914, :1194): set in onConnect, zeroed in
  // onCentralDisconnect. No second connection-state flag for this.
  if (connIntervalUnits_ != 0) return;
  if (advertisingDown_) retryAdvertising();
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
bool BlePositionServer::sendCommandBlock(const char*, size_t) { return false; }
void BlePositionServer::setTransferHooks(const TransferHooks&) {}
bool BlePositionServer::sendTransferStatus(const char*) { return false; }
void BlePositionServer::onTransferIngest(const uint8_t*, size_t) {}
void BlePositionServer::onTransferSubscribe(bool) {}
void BlePositionServer::onCommandSubscribe(bool) {}
void BlePositionServer::onMtuChanged(uint16_t) {}
void BlePositionServer::onConnIntervalChanged(uint16_t) {}
void BlePositionServer::onConnHandleChanged(uint16_t) {}
void BlePositionServer::onCentralDisconnect() {}
void BlePositionServer::onAdvertisingState(bool) {}
void BlePositionServer::retryAdvertising() {}
void BlePositionServer::serviceAdvertising() {}
int8_t BlePositionServer::rssi() const { return 0; }

}  // namespace freeink

#endif  // FREEINK_CAP_BLE_PERIPHERAL
