#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "PinCatalog.h"

// One line of /trailink/pins/pins.log, and the encode/decode for it.
//
// Pure: no Arduino, no HAL, no SD. PinLog does the file, this does the bytes, so
// every format rule below is host-testable (test/pins/).
//
// Wire format, ASCII, one record per line:
//
//   v1|<seq>|<utc>|<uptime>|<op>|<key>|<id>|<latE7>|<lonE7>|<trip>|<crc32>
//
// | field    | notes                                                          |
// |----------|----------------------------------------------------------------|
// | v1       | format version; an unknown version is skipped, not fatal        |
// | seq      | monotonic, never reused; after a reboot it is max(seq)+1        |
// | utc      | unix seconds, 0 = the device had no clock at the time           |
// | uptime   | ms since boot; orders records inside a run that had no clock    |
// | op       | add | rep | del | res (res reserved for Restore, not written)   |
// | key      | catalogue key, stable forever (PinCatalog.h)                    |
// | id       | monotonic pin id, never reused                                  |
// | latE7    | int32, 1e7 fixed point; empty on del                            |
// | lonE7    | as latE7                                                       |
// | trip     | reserved, always empty today                                   |
// | crc32    | 8 lowercase hex, over every byte before the final separator     |
//
// ASCII rather than binary deliberately: the card gets read on a laptop, and
// /trailink/power.csv set that precedent.
//
// The CRC covers the record up to, and not including, the '|' in front of the
// checksum itself.

enum class PinOp : uint8_t {
  Add,      // "add" -- an empty slot filled
  Replace,  // "rep" -- an occupied slot moved; same pin, same id
  Delete,   // "del" -- slot cleared; nothing on the card is erased
  Restore,  // "res" -- reserved for the deferred Restore UI; never written yet
};

struct PinRecord {
  uint32_t seq = 0;
  uint32_t utc = 0;  // 0 = unknown, never a fabricated time
  uint32_t uptimeMs = 0;
  PinOp op = PinOp::Add;
  uint32_t id = 0;
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  bool hasPos = false;  // false for Delete, whose coordinate fields are empty
  char key[kPinKeyBytes] = {};
};

// Longest legal line, terminator excluded: the fixed separators plus every field
// at its type's widest ("v1" + 10 + 10 + 10 + 3 + 11 + 10 + 11 + 11 + 0 + 8 and
// ten '|'), rounded up. Sized for the type, not for today's data.
inline constexpr size_t kPinLineMax = 120;

// Copies `key` into `out.key`, nul-terminated. False when the key is not
// storable (isValidPinKey), in which case out.key is left untouched.
bool setPinRecordKey(PinRecord& out, std::string_view key);

const char* pinOpText(PinOp op);

// Writes the record and its CRC into `buf`, no trailing newline (PinLog adds
// it). Returns the length written, or 0 when the buffer is too small or the
// record's key is not storable.
size_t encodePinRecord(const PinRecord& rec, char* buf, size_t bufLen);

// Where a paged read of the log hands its records. Implemented by whoever asked
// (MapConsoleState formats them into console replies); PinLog drives it while it
// streams the card, so a page costs one PinRecord on the stack and not an array
// of them.
class IPinLogVisitor {
 public:
  virtual ~IPinLogVisitor() = default;
  virtual void onPinLogRecord(const PinRecord& rec) = 0;
};

// Parses one line, terminator already stripped. False for: an unknown version,
// a wrong field count, a bad number, an unstorable key, an unknown op, or a CRC
// that does not match. A false here means "skip this line and keep reading" --
// never "the log is broken".
bool decodePinRecord(std::string_view line, PinRecord& out);
