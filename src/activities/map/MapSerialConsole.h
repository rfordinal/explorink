#pragma once

#include "MapCommandConsole.h"

// USB-serial front end for the map command console (P3).
//
// Non-blocking by construction: poll() drains only the bytes already
// buffered and returns. A half-typed line stays in the assembler across
// loop() calls -- it never waits for a line to finish, because loop() is
// also what services the buttons and the BLE stack.
//
// **Every reply is prefixed with '<'.** logSerial IS Serial (Logging.h:35),
// so every LOG_ERR/LOG_INF/LOG_DBG in the firmware lands on the same UART
// as these replies and interleaves with them. A sender that reads "the next
// line after the command" works on a quiet desk and fails the first time a
// log line arrives in between. A sender that filters on the marker does
// not. tools/mapcmd.py filters on it.
class MapSerialConsole {
 public:
  MapSerialConsole();

  // Reads whatever is buffered, runs any completed lines, writes replies.
  // Returns true if the screen needs redrawing.
  bool poll();

  const MapConsoleState& state() const { return console_.state(); }

  // Line prefix for every reply. Chosen because nothing in the LOG_* format
  // starts with it.
  static constexpr char kReplyMarker = '<';

 private:
  MapCommandConsole console_;
};
