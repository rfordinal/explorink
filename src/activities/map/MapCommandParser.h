#pragma once

#include <cstdint>
#include <string_view>

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
//   info
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

enum class MapRideMode : uint8_t { Ride, Hike, Cycle };

struct MapCommand {
  MapCommandType type = MapCommandType::Empty;
  MapCommandError error = MapCommandError::None;

  int32_t latE7 = 0;  // Pos
  int32_t lonE7 = 0;  // Pos
  uint16_t speedKmh = 0;
  uint8_t heading = 0;  // 0-15, Heading and optional on Pos
  uint8_t zoom = 0;     // 0-4
  uint8_t marker = 0;   // 0-4
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
