#pragma once

#include <cstdint>
#include <string_view>

#include "MapRideMode.h"

// Line-based ASCII command grammar for the map/nav console.
//
// Pure: no I/O, no Arduino, no activity, no serial. One grammar, two
// channels -- the USB serial console (P3, MapSerialConsole) and the BLE
// command characteristic (P5) both parse through here, so the two can never
// drift apart. That is also why this does not live inside MapActivity: a
// parser reachable only from an activity cannot be reached from a BLE
// callback.
//
// Grammar, one command per line, tokens separated by spaces or tabs:
//
//   pos <lat> <lon> [[heading] <0-15>] [[speed] <kmh>]
//   heading <0-15>
//   zoom <0-4>
//   marker <0-4>
//   mode ride|hike|cycle
//   redraw
//   tiles
//   missing [<offset>]
//   skip <z> <col> <row> [<reason>]
//   info
//
// `tiles` reports the current viewport. `missing` reports the persisted
// list of every tile the device has ever hatched, which is a different and
// much longer thing -- so it is paged: one command answers at most
// kMissingPageSize entries starting at <offset> (default 0) and says where
// the next page starts. See MapConsoleState::writeMissing().
//
// `skip` is the one command that exists for the phone rather than for a human
// at a terminal: it is how the supplier of tiles says "I cannot get you this
// one", so the device's fetch progress can count it as failed instead of
// waiting for a file that will never arrive. `<reason>` is one free-form word
// (no spaces), for the log -- the device shows a count, not a reason.
//
// The optional tail of `pos` takes the value bare (`pos 48.4 17.0 4 30`) or
// behind its own keyword (`pos 48.4 17.0 heading 4 speed 30`). Both spell
// the same command; the keyword form is what docs/prototype-plan.md's
// definition of done types.
//
// Latitude and longitude are decimal degrees, exponent notation not
// accepted, digits past the 7th decimal place ignored. They are carried as
// int32 scaled by 1e7 -- the same fixed-point the BLE position packet uses,
// so nothing on the path from a typed command to the marker needs a float.
//
// Command keywords are case sensitive and lowercase.

enum class MapCommandType : uint8_t {
  Empty,  // blank line: nothing to run, nothing to reply
  Pos,
  Heading,
  Zoom,
  Marker,
  Mode,
  Redraw,
  Tiles,
  Missing,
  Skip,
  Info,
  Error,  // see MapCommand::error
};

enum class MapCommandError : uint8_t {
  None,
  UnknownCommand,
  BadArity,
  BadNumber,
  OutOfRange,
  BadMode,
  LineTooLong,  // not produced by the parser; the line assembler raises it
};

struct MapCommand {
  MapCommandType type = MapCommandType::Empty;
  MapCommandError error = MapCommandError::None;

  int32_t latE7 = 0;  // Pos
  int32_t lonE7 = 0;  // Pos
  uint16_t speedKmh = 0;
  uint8_t heading = 0;  // 0-15, Heading and optional on Pos
  uint8_t zoom = 0;     // 0-4
  uint8_t marker = 0;   // 0-4
  // Missing: first entry of the page to print. uint16 because the store is
  // capped at 200 entries (MissingTilesStore::kMaxEntries); an offset past
  // the end is legal and answers an empty page rather than an error, which
  // is what makes a paging loop's last request harmless.
  uint16_t missingOffset = 0;
  // Skip: which tile the sender is giving up on, and why.
  uint8_t skipZ = 0;
  uint32_t skipCol = 0;
  uint32_t skipRow = 0;
  // Copied out of the line, not a view into it: a MapCommand outliving the
  // buffer it was parsed from is a use-after-free waiting to happen, and a
  // string_view here would invite exactly that. Truncated rather than
  // rejected -- a long reason word is still a legitimate skip.
  static constexpr size_t kSkipReasonBytes = 16;
  char skipReason[kSkipReasonBytes] = {};
  MapRideMode mode = MapRideMode::Ride;
  bool hasHeading = false;  // Pos carried a heading
  bool hasSpeed = false;    // Pos carried a speed
};

// Parses one line. Never fails hard: a bad line comes back as
// MapCommandType::Error with the reason in .error. Leading and trailing
// whitespace is ignored; an empty or whitespace-only line is Empty.
MapCommand parseMapCommand(std::string_view line);

// Reason word for an ERR reply. Stable, greppable, lowercase_underscore.
const char* mapCommandErrorText(MapCommandError error);
