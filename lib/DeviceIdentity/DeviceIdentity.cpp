#include "DeviceIdentity.h"

#include <BoardConfig.h>

namespace DeviceIdentity {

// A `default:` case, not an exhaustive switch, on purpose: this file builds
// against two different `BoardConfig::Board` enums with different members --
// the real one (freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h)
// on device, and the simulator's own reduced one
// (explorink-simulator/src/BoardConfig.h) in the simulator build. Same
// constraint as bleDeviceNameForActiveBoard()
// (lib/BlePositionServer/src/BlePositionServer.cpp) -- keep the two in sync
// when adding a board.
const char* activeBoardId() {
  using BoardConfig::Board;
  switch (BoardConfig::ACTIVE.board) {
    case Board::XteinkX4:
      return "X4";
    // Same DeviceType::X3 as far as WiFi/BLE naming needs to know -- Uc8279
    // is a newer X3 production run's panel controller, not a different
    // device (BoardConfig.h:369).
    case Board::XteinkX3:
    case Board::XteinkX3Uc8279:
      return "X3";
    case Board::XteinkX4Pro:
      return "X4Pro";
    case Board::LilyGoT5S3:
      return "T5S3Pro";
    default:
      // Not an ExplorInk target device today -- generic fallback name at the
      // call site rather than adding an id nobody uses.
      return "";
  }
}

}  // namespace DeviceIdentity
