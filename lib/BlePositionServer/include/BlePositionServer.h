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

// Fixed 12-byte wire format written to the position characteristic:
// [0..3] lat (int32, degrees * 1e7), [4..7] lon (int32, degrees * 1e7),
// [8] heading (0-7, MapHeading value), [9] seq (rolling counter),
// [10] flags (bit0 = off-route warning), [11] reserved.
struct PositionUpdate {
  int32_t lat = 0;
  int32_t lon = 0;
  uint8_t heading = 0;
  uint8_t seq = 0;
  uint8_t flags = 0;
};

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

  // Internal: called by the NimBLE backend (not for app use). Keeps the
  // public header free of NimBLE types.
  void onWriteIngest(const uint8_t* data, size_t len);

 private:
  BlePositionServer() = default;
  BlePositionServer(const BlePositionServer&) = delete;
  BlePositionServer& operator=(const BlePositionServer&) = delete;

  PositionUpdate latest_;
  volatile bool hasUpdate_ = false;
  bool begun_ = false;
};

}  // namespace freeink

#define BlePosition ::freeink::BlePositionServer::getInstance()
