#pragma once

#include <cstdint>

struct GnssFix;

// One CSV row per GNSS fix onto the SD card, so a ride produces numbers instead
// of an impression.
//
// Why a file and not the serial log: the measurement that decides the heading
// gate is a rider outdoors, and a rider outdoors has no laptop. The `gnss fix:`
// line already says everything needed -- it just says it to a console nobody is
// holding. This is the same reasoning PowerLog.h gives for the power baseline,
// and this follows its format conventions deliberately: header once per boot as
// the run marker, `build` on every row, append across boots, skip any line
// starting with the first column's name.
//
// ## Off by default, and it has to be
//
// **This file is a track log.** Position, speed and time, once a second, for as
// long as the map is open. That is more than the device keeps anywhere else --
// the persisted fix is one point, this is a trail -- and on a lost or stolen
// device it is a record of where the rider went (parent CLAUDE.md, "Security").
//
// So two gates, not one: it is compiled only into a build that has a receiver
// (ENABLE_GNSS_CMD, no release env has it), and inside that build it writes
// nothing until `SETTINGS.mapGnssLog` is turned on deliberately. Turning it on
// is a decision the rider makes for one measurement, not a default.
//
// Position is in the row rather than left out, and that is the deliberate half
// of the cost. The open question the log exists to answer -- whether a speed
// gate can work at all, or whether heading has to come from displacement
// between fixes -- cannot be answered offline without the positions. A log that
// could not settle it would be a privacy cost with nothing bought.
//
// ## Cost
//
// Measured on hardware 2026-09-01: about 85 bytes a row at roughly 1.7 rows a
// second, so **about 500 KB an hour of riding**. Not 1 Hz -- Gnss::poll()
// reports a change when position, speed, course OR the clock moves, and three
// rows inside 400 ms have been seen. The first estimate here said 1 Hz and
// 320 KB, from arithmetic rather than from the device.
//
// Rows are buffered and flushed about every ten seconds rather than written per
// fix: an open/append/close per fix on this card would land inside the map's
// render loop, which is already the thing this branch spent its first step
// keeping the UART alive through. A full buffer forces the write rather than
// dropping a row -- dropping is the one outcome that would make the file lie
// about the ride.
class GnssLog {
 public:
  // Call with every fix the map accepts, from the same place applyFix() is
  // called. Returns immediately when the setting is off. `moving` and
  // `headingStep` are the derived values, not the receiver's, because what the
  // log has to answer is what the DEVICE decided and why.
  static void record(const GnssFix& fix, bool moving, uint8_t headingStep);

  // Flush whatever is buffered. Called on map exit so a ride that ends by
  // closing the map does not lose its last ten seconds.
  static void flush();

  // Bytes on the card, and whether anything is waiting in RAM. Exists so
  // "did the ride record?" is a question with an answer that can be WRONG --
  // the absence of an error line in a log nobody was reading is not evidence,
  // and a measurement trip is too expensive to find out afterwards.
  //
  // Deliberately reports sizes and not content: the rows carry the rider's
  // track, and a command that printed them would hand a position log to anyone
  // with a cable (parent CLAUDE.md, "Security"). Devel-only regardless, with
  // the rest of CMD:GNSS.
  static void status(uint32_t& bytesOnCard, uint32_t& bytesBuffered, bool& loggingDisabled);

  static constexpr uint32_t kFlushIntervalMs = 10000;
  // Under the same on-device root as the tiles; the path still says trailink
  // because the card's directory has not been renamed (parent CLAUDE.md).
  static constexpr const char* kPath = "/trailink/gnss.csv";
  static constexpr const char* kDir = "/trailink";
};
