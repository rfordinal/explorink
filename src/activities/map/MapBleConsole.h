#pragma once

#include "MapCommandConsole.h"

// BLE front end for the map command console (P5).
//
// The mirror image of MapSerialConsole, and deliberately so: same grammar,
// same state, same replies, different transport. That is the entire reason
// MapCommandParser was written free of Arduino and of the activity -- a
// parser reachable only from a loop() cannot be reached from a BLE callback,
// and two parsers would drift within a week.
//
// **This is the channel that ships.** The phone talks BLE; USB serial is a
// desk tool. If the two ever disagree about a command, this one is right.
//
// Bytes arrive on the NimBLE host task and are queued there
// (BlePositionServer::readCommandBytes). poll() drains that queue on the
// activity's own task, so a command that ends in a full-screen refresh
// blocks the activity, never the BLE stack.
class MapBleConsole {
 public:
  explicit MapBleConsole(MapConsoleState& state);

  // Drains whatever the BLE callback has queued, runs any completed lines,
  // notifies the replies. Returns true if the screen needs redrawing.
  bool poll();

 private:
  MapCommandConsole console_;
};
