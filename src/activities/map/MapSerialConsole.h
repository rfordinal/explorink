#pragma once

#include "MapCommandConsole.h"

// USB-serial front end for the map command console (P3).
//
// Non-blocking by construction: poll() drains only the bytes already
// buffered and returns. A half-typed line stays in this channel's own
// assembler across loop() calls -- it never waits for a line to finish,
// because loop() is also what services the buttons and the BLE stack.
//
// The command *state* is not owned here: MapActivity owns one
// MapConsoleState and passes it to this channel and to MapBleConsole, so a
// `zoom 3` typed over USB and a `zoom 3` written over BLE land on the same
// number. See MapCommandConsole.h.
//
// **Every reply is prefixed with '<'.** logSerial IS Serial (Logging.h:35),
// so every LOG_ERR/LOG_INF/LOG_DBG in the firmware lands on the same UART
// as these replies and interleaves with them. A sender that reads "the next
// line after the command" works on a quiet desk and fails the first time a
// log line arrives in between. A sender that filters on the marker does
// not. tools/mapcmd.py filters on it. BLE needs no marker -- one
// notification is one line, and nothing else writes to that characteristic.
class MapSerialConsole {
 public:
  explicit MapSerialConsole(MapConsoleState& state);

  // Reads whatever is buffered, runs any completed lines, writes replies.
  // Returns true if the screen needs redrawing.
  bool poll();

  // Line prefix for every reply. Chosen because nothing in the LOG_* format
  // starts with it.
  static constexpr char kReplyMarker = '<';

 private:
  MapCommandConsole console_;
};
