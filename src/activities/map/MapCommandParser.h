#pragma once

#include <cstdint>
#include <string_view>

#include "MapRideMode.h"
#include "PinCatalog.h"

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
//   have
//   stale <z> <col> <row>
//   checked <n>|unknown
//   push <n>
//   fake <missing> <held>
//   info
//   stats
//   pin set <key> <lat> <lon> [<utc>]
//   pin del <key>
//   pin list
//   pin log [<offset>]
//
// `tiles` reports the current viewport. `missing` reports the persisted
// list of every tile the device has ever hatched, which is a different and
// much longer thing -- so it is paged: one command answers at most
// kMissingPageSize entries starting at <offset> (default 0) and says where
// the next page starts. See MapConsoleState::writeMissing().
//
// `have`, `stale` and `checked` are the freshness exchange (docs/tile-freshness.md,
// ../../docs/ble-map-transfer-protocol.md). `have` reports the viewport's tiles
// with the content_id each is held at; the phone reads the CDN's index and
// answers `stale` per differing tile, then `checked <n>` -- or `checked unknown`
// when it could not reach the index. **`unknown` is not zero.** A phone with no
// signal cannot tell a current tile from a stale one, and treating its silence
// as "all current" would bury exactly the bug this exists to find.
//
// `push <n>` is the phone announcing a batch it is about to send. Sent once per
// connection, before that connection's first begin frame: a dropped link takes
// the run with it, so a phone that comes back and simply resumes pushing lands
// its bytes on a screen that has gone back to believing there is nothing to do. Everything
// else on this channel is reactive: the device hatched a square, wrote it down
// and asks for it back. Ground the rider has never been on is on no list, so a
// pre-trip push of a whole city arrives with the sync screen believing there is
// nothing to do -- the bytes land, and every progress element stays dark
// because the screen sized itself from an empty missing list
// (`docs/missing-tiles.md`, "A batch the device never asked for"). The count is
// the only thing the device needs: it does not need to know which tiles, only
// how many files are coming, so it can draw a bar and finish the run.
//
// Write-only, and it answers nothing but `OK`. It reveals no position, no
// route and no stored data, so it adds no exposure the transfer channel that
// follows it does not already have.
//
// `stats` is the power meter: battery millivolts, CPU clock time, panel
// refresh counts, loop duty cycle. Same keys as the SD card's power.csv
// (src/PowerLog.cpp), deliberately -- one vocabulary, two ways out, so a script
// reading a live device and a script reading a card's log parse the same
// names. It is a separate command from `info` because it is polled on a
// different cadence: `info` answers what is on screen and is asked once,
// `stats` is asked every few minutes across a whole ride.
//
// `skip` is the one command that exists for the phone rather than for a human
// at a terminal: it is how the supplier of tiles says "I cannot get you this
// one", so the device's fetch progress can count it as failed instead of
// waiting for a file that will never arrive. `<reason>` is one free-form word
// (no spaces), for the log -- the device shows a count, not a reason.
//
// `pin` is the pins feature's whole wire path (../../docs/pins-plan.md). The
// device has no keyboard and no GPS, so `pin set` is the only way to place a pin
// at a coordinate the rider is not standing on -- a distant pin gets tested
// without driving to it, and a phone can eventually push one. `<utc>` is unix
// seconds and defaults to 0, which the log records as "the sender had no clock"
// rather than inventing a time. `<key>` must be a catalogue key (PinCatalog.h);
// anything else is `unknown_pin`, so a typo cannot occupy a slot.
//
// `pin log` pages exactly like `missing [<offset>]`, and for the same reason: a
// whole history does not fit in one reply.
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
  Have,
  Stale,
  Checked,
  Push,
  Info,
  Stats,
  Fake,
  Pin,
  Error,  // see MapCommand::error
};

// Sub-verb of `pin`. One command type with a verb rather than four types: the
// four share the key field and every caller switches on the pair anyway.
enum class MapPinVerb : uint8_t {
  Set,
  Del,
  List,
  Log,
};

enum class MapCommandError : uint8_t {
  None,
  UnknownCommand,
  BadArity,
  BadNumber,
  OutOfRange,
  BadMode,
  UnknownPin,   // `pin set`/`pin del` with a key that is not in the catalogue
  LineTooLong,  // not produced by the parser; the line assembler raises it
};

struct MapCommand {
  MapCommandType type = MapCommandType::Empty;
  MapCommandError error = MapCommandError::None;

  int32_t latE7 = 0;  // Pos
  int32_t lonE7 = 0;  // Pos
  uint16_t speedKmh = 0;
  int16_t altitudeM = 0;  // Pos, optional; metres above sea level
  uint8_t heading = 0;    // 0-15, Heading and optional on Pos
  uint8_t zoom = 0;       // 0-4
  uint8_t marker = 0;     // 0-4
  // Missing: first entry of the page to print. uint16 because the store is
  // capped at 200 entries (MissingTilesStore::kMaxEntries); an offset past
  // the end is legal and answers an empty page rather than an error, which
  // is what makes a paging loop's last request harmless.
  uint16_t missingOffset = 0;
  // Skip and Stale: which tile, and (Skip only) why. Shared fields because the
  // two carry the same coordinate triple and never occur in one command.
  uint8_t skipZ = 0;
  uint32_t skipCol = 0;
  uint32_t skipRow = 0;
  // Checked: how many tiles the phone reported stale. Meaningless unless
  // checkedKnown -- `checked unknown` means the phone could not read the index
  // and is claiming nothing, which is a different answer from `checked 0`.
  uint16_t checkedCount = 0;
  bool checkedKnown = false;
  // Push: how many files the phone says it is about to send. uint16 because the
  // bound below is far inside it -- see kMaxPushCount.
  uint16_t pushCount = 0;
  // Fake: how many synthetic missing tiles and how many synthetic held tiles to
  // seed around the current viewport. A layout affordance, not a protocol one --
  // it exists so the tile sync screen's grid can be looked at on the panel
  // without a card that happens to have the right holes in it
  // (docs/tile-freshness.md, "Seeding a grid to look at").
  uint16_t fakeMissing = 0;
  uint16_t fakeHeld = 0;
  // Copied out of the line, not a view into it: a MapCommand outliving the
  // buffer it was parsed from is a use-after-free waiting to happen, and a
  // string_view here would invite exactly that. Truncated rather than
  // rejected -- a long reason word is still a legitimate skip.
  static constexpr size_t kSkipReasonBytes = 16;
  char skipReason[kSkipReasonBytes] = {};
  // Pin: which sub-verb, which slot, and the two numbers only one verb each
  // uses. latE7/lonE7 above carry `pin set`'s coordinate -- the same fields
  // `pos` uses, deliberately, so one line of a log means one thing wherever a
  // coordinate came from. Copied, not viewed, for the same reason as skipReason.
  MapPinVerb pinVerb = MapPinVerb::List;
  char pinKey[kPinKeyBytes] = {};
  uint32_t pinUtc = 0;         // `pin set`, optional; 0 = the sender had no clock
  uint16_t pinLogOffset = 0;   // `pin log`, default 0 = the newest page
  MapRideMode mode = MapRideMode::Ride;
  bool hasHeading = false;   // Pos carried a heading
  bool hasSpeed = false;     // Pos carried a speed
  bool hasAltitude = false;  // Pos carried an altitude
};

// Parses one line. Never fails hard: a bad line comes back as
// MapCommandType::Error with the reason in .error. Leading and trailing
// whitespace is ignored; an empty or whitespace-only line is Empty.
MapCommand parseMapCommand(std::string_view line);

// Reason word for an ERR reply. Stable, greppable, lowercase_underscore.
const char* mapCommandErrorText(MapCommandError error);
