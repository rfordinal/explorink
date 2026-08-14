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

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace freeink {

// Fixed 21-byte wire format written to the position characteristic, little
// endian, no padding -- docs/architecture-plan.md, "Revised packet":
//
//   [0..3]   lat        int32,  degrees * 1e7
//   [4..7]   lon        int32,  degrees * 1e7
//   [8..11]  utc        uint32, unix seconds (0 = sender has no clock)
//   [12..13] tz_offset  int16,  minutes east of UTC
//   [14]     heading    0-15, a MapHeading value
//   [15]     seq        rolling counter; the device redraws when it changes
//   [16]     flags      bit0 = off-route warning, bit1 = altitude present
//   [17]     accuracy   metres, saturating
//   [18]     speed      km/h, saturating
//   [19..20] altitude   int16, metres above sea level; valid only if flags
//                        bit1 is set. No fix has "altitude zero" as a
//                        natural default (sea level is a real place), so a
//                        flag bit carries "no vertical fix" instead of a
//                        sentinel value stealing part of the range.
//
// This replaced a 19-byte format with no altitude. Before that, a 12-byte
// format had no time, no accuracy and no speed, and carried heading as 0-7;
// it is not accepted any more, nor is a 19-byte write now that 21 is
// current -- a write of the wrong length is dropped rather than read as a
// truncated or padded version of the new one.
//
// `utc`, `tz_offset` and `accuracy` are wired and stored; nothing draws them
// yet -- see docs/architecture-plan.md, "Time is mandatory in the payload".
// `speed` is auto zoom's input and auto zoom is a later phase. `altitude` is
// hike mode's future input, same reasoning: wired now so the packet stops
// changing, not drawn or acted on yet.
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
  int16_t altitudeM = 0;     // valid only if hasAltitude
  bool hasAltitude = false;  // from flags bit1
};

// Exact size of the packet above. A write of any other length is ignored.
inline constexpr size_t kPositionPacketBytes = 21;

// The advertised name. The phone matches on it (ScanFilter.setDeviceName) and
// the CompanionDeviceManager association dialog shows it to the rider, so it is
// wire-visible and must not change casually.
inline constexpr const char* kBleDeviceName = "XteinkX4Map";

class BlePositionServer {
 public:
  static BlePositionServer& getInstance();

  // Starts advertising a GATT peripheral with one write characteristic.
  // No pairing/bonding -- this is a short-range (~0.5-1m), low-stakes
  // channel; keeping the phone-side BLE code to "connect + write" is worth
  // more than pairing security here. Safe to call once; returns false if
  // BLE init failed or the capability is compiled out.
  bool begin(const char* deviceName = kBleDeviceName);

  // Fully tears down the BLE stack so its RAM is returned to the heap --
  // call this in MapActivity::onExit(), not just disconnect(), same reason
  // BleKeyboardHost::end() exists (see its header comment).
  void end();

  bool isRunning() const { return begun_; }

  // --- Advertising state, and why it is the activity task's problem ------
  //
  // NimBLE stops advertising when a central connects and does not resume it,
  // so the disconnect callback restarts it. That restart can fail, and the
  // most likely cause right after a link drop is "host not synced"
  // (NimBLEAdvertising.cpp:189-192).
  //
  // **A failed restart must not be retried inside the callback.** The
  // disconnect callback runs on the NimBLE host task, and the sync event that
  // clears "host not synced" is dispatched on that same task. A retry loop
  // with a delay in it therefore blocks the event it is waiting for: every
  // attempt fails by construction and the radio stays deaf until the screen
  // is left and re-entered. That is exactly what the old 5x50 ms
  // vTaskDelay loop did (docs/ble-review-2026-08.md item 3).
  //
  // So: one attempt in the callback, and on failure this flag. The retry runs
  // on the activity task, which is a different task and cannot block the host.

  // True while advertising is believed to be off with nothing connected.
  bool advertisingDown() const { return advertisingDown_; }

  // One restart attempt, rate-limited to kAdvertisingRetryMs so a permanently
  // dead radio costs one call per second instead of one per loop() tick.
  // **Call from an activity task, never from a NimBLE callback.**
  void retryAdvertising();

  // The single per-tick advertising owner. Every activity that runs the BLE
  // server calls exactly this one line from its loop(); anything the activity
  // task has to do about advertising state belongs in here, not as a second
  // call bolted on next to it. Today that is the failed-restart retry, the
  // fast->slow interval switch below and the connection parameter requests
  // (T6.2) further down.
  //
  // `transferActive` is the caller's own MapTransferReceiver::status().active
  // -- this library carries bytes only and does not know MapTransferReceiver
  // exists (see TransferHooks below), so the activity passes the answer in
  // rather than this class reaching for it. Cheap either way: the caller
  // already reads that Status every tick for its own progress UI.

  // --- Two-phase advertising interval -----------------------------------
  //
  // NimBLE's fast default (BLE_GAP_ADV_FAST_INTERVAL1, 30-60 ms) is right for
  // the "just opened the map, phone should notice fast" moment, and wasteful
  // forever after: a map screen left open with no phone in range advertises
  // at that rate indefinitely, ~10x the TX events a 200-300 ms interval would
  // cost for the same "is anybody there" job
  // (docs/ble-review-2026-08.md, "Power"). docs/ble-advertising.md (parent
  // repo) has the interval numbers and the wake-latency trade.
  //
  // Fast for the first kFastAdvertisingMs after begin() and after every
  // disconnect -- reconnect UX stays snappy, which matters more right after
  // the rider was just connected than it does after 30 s of nobody around.
  // Slow once that window elapses with nothing connected.
  //
  // --- Connection parameters while connected (T6.2) ---------------------
  //
  // Advertising above is about being found; this is about what happens once
  // a phone is on the link. Connected-idle otherwise runs at whatever
  // interval the phone chose to connect with -- often its ~30 ms default --
  // to carry a single 1 Hz position write, ~33 radio events/s for one write
  // (docs/ble-review-2026-08.md, "Power": "device never requests connection
  // parameters"). serviceAdvertising() now also asks the central to relax
  // that, and to tighten it back up while a file transfer is actually moving
  // bytes. See BlePositionServer.cpp, serviceConnParams(), for the two
  // parameter sets and the arithmetic behind the supervision timeouts.
  void serviceAdvertising(bool transferActive);

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

  // Sends an already-composed block of newline-terminated reply lines as **one**
  // indication. `len` must fit commandPayloadBytes() or the controller drops
  // the tail; the caller does the packing, because only it knows where a line
  // boundary is (MapBleConsole).
  //
  // One indication per line was the old shape and it is what made a multi-line
  // reply fragile: every line costs a full round trip to the peer's confirm, so
  // a five-line listing spent seconds on the link and each hop was a chance to
  // lose one. A `have` reply for a four-tile viewport fits one 256-byte
  // indication whole.
  bool sendCommandBlock(const char* text, size_t len);

  // True when the most recent sendCommandBlock/sendCommandReply's 3 s confirm
  // wait expired instead of being answered -- i.e. the peer stopped
  // confirming indications (hung, walked out of range, backgrounded past
  // whatever keeps its GATT callback alive). Cleared the next time a send
  // *is* confirmed, so this reflects only the latest attempt, not a sticky
  // fault. Callers that would otherwise send one more indication on their way
  // out (FETCH_CANCEL on exit) check this first: a peer that is not
  // confirming will not hear that one either, so the exit's own 3 s wait for
  // it buys nothing but a frozen screen.
  bool lastConfirmTimedOut() const { return lastConfirmTimedOut_; }

  // What fits in one indication on this link: the ATT MTU minus its 3-byte
  // header. 20 while the MTU is unknown -- the 23-byte default every link
  // starts at, which is the safe assumption rather than an optimistic one.
  uint16_t commandPayloadBytes() const { return mtu_ > 3 ? static_cast<uint16_t>(mtu_ - 3) : 20; }

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
  // Unlike sendCommandReply this **never waits** for anything: it is called
  // from inside the transfer write callback, i.e. on the NimBLE host task, and
  // that same task is the one that delivers the confirm which frees the
  // indication slot. Any wait here -- for the confirm, or a retry loop with a
  // vTaskDelay in it -- blocks the task that would end the wait, so it
  // deadlocks by construction.
  //
  // There is **one indication slot per connection**, shared with the command
  // channel, so a line can find it busy. The old justification ("the command
  // channel is not in use while a file is being pushed") was wrong from the
  // day the map screen started running freshness listings and stale-tile
  // pushes together: a `have` listing can hold the slot for up to
  // kConfirmTimeoutMs = 3000 ms (BlePositionServer.cpp, sendCommandBlock),
  // against which any retry budget short enough not to stall the host task
  // loses. So a busy slot parks the line in a tiny pending buffer instead of
  // being retried or dropped, and the activity task sends it later --
  // flushTransferStatus() below.
  //
  // Returns true when the line went out immediately, false when it was parked
  // or could not be composed. A parked line is not a failure: `false` here
  // means "not on the link yet", and no caller retries on it.
  bool sendTransferStatus(const char* line);

  // Sends one parked status line if the indication slot is free now.
  //
  // **Call from an activity task, never from a NimBLE callback** -- same rule
  // as retryAdvertising(), same reason: the host task cannot wait on itself.
  // One attempt per call, no wait of any kind; loop() comes back in
  // milliseconds and the buffer holds at most two lines, so there is nothing
  // to drain in a hurry.
  void flushTransferStatus();

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
  // first real fetch measured. The central owns this number, so a peripheral's
  // request can be ignored or overridden outright -- request it anyway and log
  // the outcome (docs/ble-review-2026-08.md, "Power": "device never requests
  // connection parameters... often honoured despite the header's pessimism;
  // free when ignored"). The phone also asks for a fast link while a sync
  // runs, on top of whatever this device requested, and this is how the
  // device can tell
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
  // Internal: the result of an advertising->start() call. `true` from the
  // connect callback too -- a central that connected is proof advertising did
  // its job, and NimBLE stops it while connected by design.
  void onAdvertisingState(bool up);
  // Internal: called on connect and on disconnect (the latter after the
  // NimBLE-level interval is reset back to fast). Restarts the fast-phase
  // window -- "advertising just mattered again" resets the clock rather than
  // resuming a countdown that was already most of the way to slow.
  void resetAdvertisingPhase();

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

  // Status lines that found the indication slot busy, waiting for the activity
  // task to put them on the link (flushTransferStatus). Fixed size, no heap --
  // this is written from the NimBLE host task, where an allocation is the last
  // thing wanted, and the realistic worst case is exactly two lines: the `RDY`
  // that opens a transfer and the `OK`/`ERR` that closes it. 64 bytes is
  // sendTransferStatus's own compose buffer, so nothing can be parked that
  // would not have fitted on the link.
  //
  // Head and tail are monotonic counters, not indices, and that is
  // load-bearing: the activity task copies the line at `tail` out, sends it
  // outside the critical section, and only then advances the tail *if it is
  // still the same one*. An overflow drop on the host task in that window
  // moves the tail, the compare fails, and the next line is not eaten by a
  // send that was not about it.
  //
  // Plain, not volatile and not atomic. **Every** access to these two counters
  // and to the two arrays above is inside a portENTER_CRITICAL(&g_mux) section
  // (BlePositionServer.cpp:329-330, :387-388, :616-617, :690-703, :725-731,
  // :741-744) -- and the critical section is the synchronisation, not the
  // qualifier. On this single-core target portENTER_CRITICAL is an out-of-line
  // call to vPortEnterCritical() (FreeRTOS portmacro.h:530, riscv, and no LTO
  // in platformio.ini), so the compiler cannot move a plain access across it
  // either.
  //
  // `volatile` is not a synchronisation primitive and bought nothing here
  // except a -Wvolatile deprecation warning on `++counter` (C++20). Atomics
  // would be worse than nothing: they would advertise a lock-free protocol that
  // does not exist. Contrast advertisingDown_ below, which is atomic precisely
  // because it *is* read outside any lock, by serviceAdvertising().
  static constexpr size_t kPendingStatusSlots = 2;
  static constexpr size_t kPendingStatusBytes = 64;
  char pendingStatus_[kPendingStatusSlots][kPendingStatusBytes] = {};
  uint8_t pendingStatusLen_[kPendingStatusSlots] = {};
  uint32_t pendingStatusHead_ = 0;  // next write, host task
  uint32_t pendingStatusTail_ = 0;  // next send, activity task

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

  // Set and read only inside sendCommandBlock and by lastConfirmTimedOut()'s
  // callers, all on the activity task that calls sendCommandBlock -- no other
  // task touches this, so a plain bool is enough (same reasoning as begun_).
  bool lastConfirmTimedOut_ = false;

  // Minimum gap between advertising restart attempts from the activity task.
  // A loop() tick is milliseconds; a start() that failed on host state fails
  // again immediately and nothing this task does can hurry the host along.
  static constexpr uint32_t kAdvertisingRetryMs = 1000;

  // Written by the NimBLE host task (onDisconnect/onConnect) and by the
  // activity task (retryAdvertising), read by both. Atomic rather than
  // volatile because two tasks write it -- the C3 is single-core, so a bool
  // store is already indivisible, but this states the requirement instead of
  // depending on the core count.
  std::atomic<bool> advertisingDown_{false};
  // Stamped by whichever task last tried a start(). Same two writers.
  std::atomic<uint32_t> lastAdvertisingAttemptMs_{0};

  // --- Two-phase advertising interval, state for the header comment above
  //
  // 30 s of fast advertising, then slow until the next connect/disconnect.
  static constexpr uint32_t kFastAdvertisingMs = 30000;
  // 320 units * 0.625 ms = 200 ms .. 480 units * 0.625 ms = 300 ms (the
  // controller's advertising interval field is in 0.625 ms units, same as
  // BLE_GAP_ADV_FAST_INTERVAL1/2 in the vendored NimBLE's ble_gap.h).
  static constexpr uint16_t kSlowMinIntervalUnits = 0x140;
  static constexpr uint16_t kSlowMaxIntervalUnits = 0x1E0;

  // Start of the current fast-phase window, in millis(). Reset at begin(),
  // at connect and at disconnect (resetAdvertisingPhase()) -- each is
  // "advertising just mattered again" and earns a fresh kFastAdvertisingMs
  // rather than picking up wherever the last window left off. Read by
  // serviceAdvertising() on the activity task; written from there too
  // (begin()) and from the NimBLE host task (onConnect/onDisconnect), so
  // atomic, same reasoning as lastAdvertisingAttemptMs_ above.
  std::atomic<uint32_t> phaseStartMs_{0};
  // True once the radio has actually been switched to the slow interval for
  // the current phase window. Sits alongside phaseStartMs_ so
  // maybeEnterSlowAdvertising() only issues the stop()/setInterval/start()
  // sequence once per window instead of every tick past kFastAdvertisingMs.
  // Same two writers as phaseStartMs_ (activity task sets it true, the host
  // task's resetAdvertisingPhase() sets it back false), so atomic too.
  std::atomic<bool> advertisingSlow_{false};

  // Switches the radio to the slow interval once kFastAdvertisingMs has
  // elapsed with nothing connected. Called only from serviceAdvertising() --
  // this and retryAdvertising() are the only two places that touch
  // advertising state from the activity task, so there is exactly one owner.
  void maybeEnterSlowAdvertising();

  // One indication, one confirm wait -- the unit sendCommandBlock() splits a
  // block into. `len` must already fit commandPayloadBytes(); the caller
  // (sendCommandBlock) is the one that sizes chunks. Returns false on a
  // failed indicate() or an unconfirmed send within the retry budget.
  bool sendCommandChunk(const char* text, size_t len);

  // --- Connection parameter requests (T6.2), state for serviceConnParams()
  //
  // Every field below is read and written only inside serviceConnParams(),
  // called only from serviceAdvertising() on the activity task -- same house
  // rule as phaseStartMs_/advertisingSlow_ above, opposite conclusion: a
  // plain member is correct here because *no other task ever touches these*,
  // where phaseStartMs_/advertisingSlow_ are atomic because the host task
  // writes them too. connHandle_ and connIntervalUnits_ (below) are the only
  // cross-task reads serviceConnParams() does, and both are pre-existing.
  //
  // A new connHandle_ (including the transition to/from
  // BLE_HS_CONN_HANDLE_NONE) means the previous connection's request
  // bookkeeping does not apply -- 0xFFFF here is that sentinel, spelled out
  // rather than including the NimBLE header this class is deliberately free
  // of (BlePositionServer.h's own top-of-file comment).
  uint16_t connParamsLastHandle_ = 0xFFFF;
  // Start of the current "quiet" window: set at a new connection and again
  // every time a transfer ends. The idle set is requested once this window
  // has run kConnParamsIdleDelayMs with no transfer active.
  uint32_t connParamsQuietSinceMs_ = 0;
  // Whether the idle set has already been requested for the current window,
  // so a tick that changes nothing does not call updateConnParams() again
  // every 10-30 ms -- NimBLE has no "already at this value" short circuit of
  // its own to rely on here. The fast set has no equivalent flag: it is
  // requested exactly once per transfer, on the begin edge that
  // connParamsTransferWasActive_ below detects, so there is nothing further
  // to gate on a later tick.
  bool connParamsIdleRequested_ = false;
  // transferActive as of the previous call, so a begin/end is a detected edge
  // rather than something re-fired every tick a transfer happens to still be
  // running (begin) or still be quiet (end already handled by the quiet-window
  // check above).
  bool connParamsTransferWasActive_ = false;

  // 5 s after connect, or 5 s after the last transfer's end, whichever the
  // clock above is timing -- long enough that a transfer that is about to
  // start (autosync asking again within the same visit) does not bounce the
  // link between the two sets, short enough that "phone connected, nothing
  // moving" settles onto the cheap interval well inside one map redraw.
  static constexpr uint32_t kConnParamsIdleDelayMs = 5000;

  // Connected-idle set: 30-50 ms interval, slave latency 4. See
  // BlePositionServer.cpp, serviceConnParams(), for why the supervision
  // timeout below clears this pair's worst-case silence with room to spare.
  static constexpr uint16_t kConnParamsIdleMinUnits = 24;  // 24 * 1.25 ms = 30 ms
  static constexpr uint16_t kConnParamsIdleMaxUnits = 40;  // 40 * 1.25 ms = 50 ms
  // T6.2 shipped 9. Lowered here (T6.2 hardware fix) because it is
  // unmeasured savings against a real cost, not because it caused the
  // measured outage: the 57/57 disconnects (docs/power-management.md,
  // "T6.2") were all gatt_status 8 with a *late-answer* pattern -- 119
  // gatt_timeout paired with 119 gatt_late, 5.9-7.2 s after issue -- which is
  // a render/task stall, not a missed connection event. Latency 9's own
  // worst-case gap, (1+9)*50 ms = 500 ms, was never close to even the old
  // 6 s timeout, so it is not the mechanism of the outage. It is lowered
  // anyway because the power it saves has never been measured (the H8 power
  // soak, docs/ble-fix-plan.md, never ran) while the cost -- more
  // legitimately-skipped listens for a marginal-RF hiccup to stack on top
  // of -- is real and cheap to reduce: halving the skip budget costs
  // (1+4)*50 ms*2 = 500 ms of margin against a 20 s timeout that has 40x to
  // spare either way (BlePositionServer.cpp, serviceConnParams()).
  static constexpr uint16_t kConnParamsIdleLatency = 4;
  // 2000 * 10 ms = 20 s. T6.2 shipped 600 (6 s), sized only against
  // (1+9)*50 ms*2 = 1000 ms of missed-event risk -- correct arithmetic for
  // the wrong failure mode. Measured on hardware: 57/57 disconnects were
  // gatt_status 8 (supervision timeout), 28 of them with no transfer in
  // flight, i.e. against this idle set. 20 s clears the maintainer's stated
  // worst case -- a map render plus panel refresh must be assumed to reach
  // 10 s -- with a 2x margin (2.7x against the slowest render measured on the
  // bench, a 7463 ms rung-0 frame: parent repo, docs/render-gate/stream-crc/
  // manifest.json, "renderMs": 7463, and docs/street-labels-plan.md:82. That
  // is a render-gate figure, not a number from the 2026-08-14 ride, and it is
  // not in this submodule's docs/place-labels.md); stays inside the BLE
  // supervision-timeout range (0x000A-0x0C80, 100 ms-32 s) with 12 s to
  // spare; and sits 10 s below Android's own 30 s ATT transaction timeout,
  // past which Android tears the bearer down itself regardless of what this
  // device asks for, so going higher would buy nothing. Cost: a phone that
  // is actually gone (not just a render stall) now reads as "still
  // connected" for up to 20 s instead of 6 s, which delays
  // resetAdvertisingPhase() by the same amount and therefore delays the
  // start of kFastAdvertisingMs's 30 s fast-interval window -- worst case
  // ~14 s later than before this change, ~50 s total from actual departure
  // to falling back to slow advertising instead of ~36 s.
  static constexpr uint16_t kConnParamsIdleTimeoutUnits = 2000;

  // Transfer-active set: fast, no latency, while bytes are actually moving.
  static constexpr uint16_t kConnParamsFastMinUnits = 12;  // 12 * 1.25 ms = 15 ms
  static constexpr uint16_t kConnParamsFastMaxUnits = 24;  // 24 * 1.25 ms = 30 ms
  static constexpr uint16_t kConnParamsFastLatency = 0;
  // 2000 * 10 ms = 20 s, same value and reasoning as
  // kConnParamsIdleTimeoutUnits above -- 29 of the 57 measured disconnects
  // happened during an open fetch, i.e. against this fast set, and the
  // render/refresh pause that kills the link does not know or care whether
  // a transfer happens to be in flight when it hits. Same margins: 2x the
  // assumed 10 s render+refresh, 2.7x the slowest bench render (7463 ms), 12 s
  // inside the BLE range's 32 s ceiling, 10 s below Android's 30 s ATT
  // timeout. (1+0)*30 ms*2 = 60 ms against it -- clears by over 300x, wider
  // than the idle set's margin since latency is 0 here. Cost: a phone that
  // drops mid-transfer and is actually gone now reads as connected for up to
  // 20 s instead of 4 s -- 16 s later into kFastAdvertisingMs's 30 s
  // fast-advertising window than before this change.
  static constexpr uint16_t kConnParamsFastTimeoutUnits = 2000;

  // The one place either set is ever requested. Called only from
  // serviceAdvertising(), which is itself activity-task-only -- so, like
  // retryAdvertising()/maybeEnterSlowAdvertising(), never from a NimBLE
  // callback.
  void serviceConnParams(bool transferActive);

  PositionUpdate latest_;
  volatile bool hasUpdate_ = false;
  bool begun_ = false;
};

}  // namespace freeink

#define BlePosition ::freeink::BlePositionServer::getInstance()
