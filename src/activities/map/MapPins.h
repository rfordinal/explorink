#pragma once

#include <cstdint>
#include <string_view>

#include "MapCommandConsole.h"
#include "PinLog.h"
#include "PinStore.h"

// The device's pins: the active set, the history on the card, and the one rule
// that ties them together.
//
// **The log is written first, and the active set moves only if that worked.**
// Both mutations below hold that order. The opposite order can leave RAM
// claiming a pin the card never recorded, which a replay cannot repair -- the
// rider would see a Camp until the next reboot and then lose it silently. This
// class exists so that rule lives in one place instead of once per caller (the
// console today, the popups in phase 3).
//
// Owned by MapActivity for the map screen's lifetime, and it *is* the
// IMapPinsSource the console talks to.
//
// RAM: one PinStore (fourteen fixed entries, ~450 bytes) inside the activity.
// No heap, no vector -- the count is bounded by the catalogue and cannot grow at
// runtime (PinCatalog.h).
class MapPins final : public IMapPinsSource {
 public:
  // Rebuilds the active pins from the card. Called once, from onEnter(). False
  // when the card could not be read at all; the pins are then empty and a save
  // will fail loudly rather than write a history with a hole in it.
  bool begin();

  const PinStore& store() const { return store_; }

  // Local wall clock is not what a record stores -- see BlePositionServer::
  // utcNow(). 0 means the device genuinely does not know the time, which the log
  // records as such rather than inventing one.
  static uint32_t utcNowOrZero();

  // IMapPinsSource -- also the UI's path in phase 3, deliberately: a pin saved
  // from a popup and one pushed over BLE must produce the same record.
  bool pinSet(std::string_view key, int32_t latE7, int32_t lonE7, uint32_t utc) override;
  bool pinDelete(std::string_view key) override;
  size_t pinCount() const override;
  PinEntry pinAt(size_t index) const override;
  uint32_t pinLogPage(uint32_t offset, uint32_t maxCount, IPinLogVisitor& visitor) override;

  // How much of the last replay was refused. Reported by the map's debug readout
  // rather than swallowed: a card quietly dropping records is exactly what this
  // feature must not hide.
  const PinReplayStats& replayStats() const { return replayStats_; }

 private:
  PinStore store_;
  PinReplayStats replayStats_;
  bool replayed_ = false;
};
