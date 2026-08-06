#pragma once

// BLE peripheral (GATT server) for the map/nav feature — receives
// position+heading updates written by the Android companion app (BLE
// central/writer). Opposite role from BleKeyboardHost (central/HID-host).
//
// Capability-gated: the real NimBLE implementation compiles only when
// FREEINK_CAP_BLE_PERIPHERAL is set (and the firmware adds NimBLE-Arduino to
// its lib_deps); otherwise every method links a stub so callers need no
// #ifdefs and no BLE code is pulled in. This header is deliberately
// NimBLE-free. See docs/architecture-plan.md (parent xteink repo) for the
// wire format and protocol rationale.

#include <cstddef>
#include <cstdint>

namespace freeink {

// Fixed 19-byte wire format written to the position characteristic, little
// endian, no padding -- docs/architecture-plan.md, "Revised packet":
//
//   [0..3]   lat        int32,  degrees * 1e7
//   [4..7]   lon        int32,  degrees * 1e7
//   [8..11]  utc        uint32, unix seconds (0 = sender has no clock)
//   [12..13] tz_offset  int16,  minutes east of UTC
//   [14]     heading    0-15, a MapHeading value
//   [15]     seq        rolling counter; the device redraws when it changes
//   [16]     flags      bit0 = off-route warning
//   [17]     accuracy   metres, saturating
//   [18]     speed      km/h, saturating
//
// This replaced a 12-byte format that had no time, no accuracy and no speed,
// and carried heading as 0-7. It is not accepted any more: a 12-byte write
// is dropped rather than read as a truncated 19. The phone app does not
// exist yet, so there is nothing in the field to be compatible with, and
// silently reading old bytes into new fields is how a wire format rots.
//
// `utc`, `tz_offset` and `accuracy` are wired and stored; nothing draws them
// yet -- see docs/architecture-plan.md, "Time is mandatory in the payload".
// `speed` is auto zoom's input and auto zoom is a later phase. All of them
// are here now so the packet stops changing.
struct PositionUpdate {
  int32_t lat = 0;
  int32_t lon = 0;
  uint32_t utc = 0;
  int16_t tzOffsetMin = 0;
  uint8_t heading = 0;  // 0-15
  uint8_t seq = 0;
  uint8_t flags = 0;
  uint8_t accuracyM = 0;
  uint8_t speedKmh = 0;
};

// Exact size of the packet above. A write of any other length is ignored.
inline constexpr size_t kPositionPacketBytes = 19;

class BlePositionServer {
 public:
  static BlePositionServer& getInstance();

  // Starts advertising a GATT peripheral with one write characteristic.
  // No pairing/bonding -- this is a short-range (~0.5-1m), low-stakes
  // channel; keeping the phone-side BLE code to "connect + write" is worth
  // more than pairing security here. Safe to call once; returns false if
  // BLE init failed or the capability is compiled out.
  bool begin(const char* deviceName = "XteinkX4Map");

  // Fully tears down the BLE stack so its RAM is returned to the heap --
  // call this in MapActivity::onExit(), not just disconnect(), same reason
  // BleKeyboardHost::end() exists (see its header comment).
  void end();

  bool isRunning() const { return begun_; }

  // Copies the most recently received update. Returns false if nothing has
  // been received yet since begin().
  bool getLatest(PositionUpdate& out) const;

  // --- Command channel (P5) -------------------------------------------
  //
  // A second characteristic carrying the same ASCII command lines the USB
  // serial console takes -- `pos`, `zoom`, `mode`, `info` and the rest. The
  // grammar is not repeated anywhere: MapBleConsole feeds these bytes to the
  // same MapCommandParser the serial console uses (docs/prototype-plan.md,
  // P5). **This is the channel that ships**; USB never leaves the desk.
  //
  // Writes are queued in a small ring rather than parsed in the callback,
  // because the callback runs on the NimBLE host task and parsing a command
  // can end in a full-screen redraw. Draining happens on the activity's own
  // loop().

  // Copies out up to `max` queued bytes. Returns how many. Never blocks.
  size_t readCommandBytes(char* out, size_t max);

  // Sends one reply line as an indication, newline included. Returns false
  // if BLE is down, nobody has subscribed, the line does not fit, or the
  // peer never confirmed it within the retry budget. Indication over
  // notification is deliberate: a GATT notification is unacknowledged, so a
  // multi-line reply sent faster than the connection interval drains it can
  // have its tail silently dropped by the controller with no error --
  // confirmed on real hardware sending BlePositionServer.cpp's history. No
  // '<' marker: one indication is one line and nothing else writes here, so
  // the marker the UART needs has no job on this channel.
  bool sendCommandReply(const char* line);

  // --- Map file transfer channel --------------------------------------
  //
  // Two more characteristics on the same service: `...0004` takes binary
  // transfer frames, `...0005` carries ASCII status lines back.
  //
  // `...0004` is a plain WRITE, i.e. write-with-response. That response is
  // the whole flow control: the central cannot send the next chunk until the
  // device has answered the previous one, and the device answers only after
  // the bytes are on the card. So there is no per-chunk ack of our own to
  // design, and no chunk queue to size -- the ATT layer already guarantees
  // delivery and order on one link.
  //
  // The wire format and the file writing live in MapTransferReceiver. This
  // class only carries bytes: no HalStorage, no SD, no map knowledge in the
  // BLE library.
  struct TransferHooks {
    void* ctx = nullptr;
    // One BLE write, one call. Runs on the NimBLE host task.
    void (*onFrame)(void* ctx, const uint8_t* data, size_t len) = nullptr;
    // The central went away, so any transfer in flight is dead. Also the
    // NimBLE host task.
    void (*onDisconnect)(void* ctx) = nullptr;
  };

  // Registered by MapActivity while the map screen is up. Passing a default
  // TransferHooks{} unregisters, and end() does that too. Plain function
  // pointers, not std::function -- firmware CLAUDE.md, "Template and
  // std::function Bloat".
  void setTransferHooks(const TransferHooks& hooks);

  // Sends one status line as an indication, newline included -- same
  // reasoning as sendCommandReply's indication-over-notification.
  //
  // Unlike sendCommandReply this does **not** wait for the peer's confirm:
  // it is called from inside the transfer write callback, i.e. on the NimBLE
  // host task, and that same task is the one that delivers the confirm.
  // Waiting for it there would deadlock. Safe because the protocol never
  // sends two status lines back to back -- one "ready", then one verdict,
  // seconds of file transfer apart. That is the opposite of the 18-lines-in-
  // 3ms burst that made the command channel need the confirm wait.
  bool sendTransferStatus(const char* line);

  // True once a central has subscribed to the **command** characteristic --
  // i.e. somebody is actually listening for `NEED_TILES` and will answer it.
  //
  // sendCommandReply() cannot answer this question. NimBLE's indicate() accepts
  // a line into its one-slot queue whether or not a peer is subscribed, so a
  // reply "succeeding" is not evidence that anything heard it. A screen that
  // waits for a phone has to ask here instead, or it sits at 0 of N forever
  // with nothing to show for it.
  bool isCommandSubscribed() const { return commandSubscribed_; }

  // The link's negotiated ATT MTU, or 0 before a central has connected and
  // exchanged one.
  //
  // **The central initiates the exchange; the device only states a preference**
  // (CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU, sdkconfig.defaults). So this is the
  // only way to know what the link actually is -- and it decides the whole
  // transfer speed: 23 bytes leaves 15 bytes of file payload per write, 256
  // leaves 248 (docs/optimization/03-ble-link.md). A phone-side developer cannot
  // see this number from their end at all, which is why `info` reports it.
  uint16_t negotiatedMtu() const { return mtu_; }

  // File payload per transfer chunk on this link: MTU minus 3 bytes of ATT
  // header and 5 of chunk header. 0 while the MTU is unknown.
  uint16_t transferPayloadBytes() const { return mtu_ > 8 ? static_cast<uint16_t>(mtu_ - 8) : 0; }

  // The link's connection interval in 1.25 ms units, 0 before a central
  // connects.
  //
  // **This, not the MTU, is what caps a tile transfer.** A chunk is written
  // with-response, so it spends one interval going out and one coming back: at
  // 50 ms and a 248-byte payload that is 2.4 kB/s, which is exactly what the
  // first real fetch measured. The central owns this number -- a peripheral's
  // request for faster parameters is usually ignored by Android -- so the phone
  // asks for a fast link while a sync runs, and this is how the device can tell
  // whether it got one.
  uint16_t connIntervalUnits() const { return connIntervalUnits_; }
  // The same in milliseconds, rounded down. 0 while nothing is connected.
  uint16_t connIntervalMs() const { return static_cast<uint16_t>(connIntervalUnits_ * 5 / 4); }

  // The link's RSSI in dBm, read fresh from the radio (not cached) via a raw
  // HCI Read RSSI command against the active connection handle. 0 while
  // nothing is connected -- 0 dBm is not a real BLE RSSI reading (every real
  // one is negative), so it doubles as the sentinel.
  int8_t rssi() const;

  // Internal: called by the NimBLE backend on the MTU exchange.
  void onMtuChanged(uint16_t mtu);
  // Internal: on connect and on every mid-connection parameter update.
  void onConnIntervalChanged(uint16_t units);
  // Internal: on connect, so rssi() has a handle to query.
  void onConnHandleChanged(uint16_t connHandle);

  // True once the central has subscribed to the status characteristic. A
  // transfer started before this has nowhere to report its verdict to, so
  // MapTransferReceiver refuses one -- same check sendCommandReply's channel
  // relies on, made explicit because here it decides whether to begin.
  bool isTransferSubscribed() const { return transferSubscribed_; }

  // Internal: called by the NimBLE backend (not for app use). Keeps the
  // public header free of NimBLE types.
  void onWriteIngest(const uint8_t* data, size_t len);
  void onCommandIngest(const uint8_t* data, size_t len);
  void onTransferIngest(const uint8_t* data, size_t len);
  void onTransferSubscribe(bool subscribed);
  void onCommandSubscribe(bool subscribed);
  void onCentralDisconnect();

 private:
  // Bytes of command text buffered between the BLE callback and the next
  // loop(). One line is at most 96 bytes (MapLineAssembler::kMaxLine), so
  // this holds a couple of commands even if loop() is busy in a redraw. A
  // write that would overflow it is dropped whole, not truncated: half a
  // command can still parse, which is worse than none.
  static constexpr size_t kCommandRingBytes = 256;
  char commandRing_[kCommandRingBytes] = {};
  volatile size_t commandHead_ = 0;  // write index, BLE task
  volatile size_t commandTail_ = 0;  // read index, activity task

  BlePositionServer() = default;
  BlePositionServer(const BlePositionServer&) = delete;
  BlePositionServer& operator=(const BlePositionServer&) = delete;

  // Written by the activity task, read by the NimBLE host task. Copied out
  // under the same critical section the ring uses before being called, so a
  // hook can't be swapped out between the null check and the call.
  TransferHooks transferHooks_;
  volatile bool transferSubscribed_ = false;
  volatile bool commandSubscribed_ = false;
  volatile uint16_t mtu_ = 0;
  volatile uint16_t connIntervalUnits_ = 0;
  volatile uint16_t connHandle_ = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE, kept out of this NimBLE-free header

  PositionUpdate latest_;
  volatile bool hasUpdate_ = false;
  bool begun_ = false;
};

}  // namespace freeink

#define BlePosition ::freeink::BlePositionServer::getInstance()
