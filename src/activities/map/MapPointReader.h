#pragma once

#include <cstddef>
#include <cstdint>

#include "IFileSource.h"
#include "MapPointTypes.h"

// Reads one .tip point shard -- ../../../docs/point-file-spec.md in the parent
// xteink repo. Header parse, crc32 validation, then streaming iteration over
// the fixed-size record array, with names fetched from the pool on demand.
//
// The point layer is the safety points (later landmarks) on their own z10 grid,
// separate from the base tile so it can be refreshed on its own
// (docs/map-data-spec.md, "The safety layer is its own file"). z10 because the
// Nearby menu searches a 25 km radius from the GPS fix, which touches 3x3
// shards instead of z11's 5x5.
//
// ## RAM is O(1) in point count
//
// Same rule MapTileReader and MapRouteReader follow. Records stream through a
// fixed kStreamBufferSize buffer; nothing accumulates. A shard is tens of
// points, so this buffer is generous rather than tight -- it is sized to hold a
// whole realistic shard's records in one read, which makes a walk one card
// access instead of several.
//
// ## Names are a second read, on purpose
//
// A record is 16 bytes with a `name_off` into a pool that follows the array.
// The nearest-per-category pass reads only the array -- 100 points is 1.6 kB --
// and calls readName() for the handful of rows a screen actually prints. A
// reader that inlined names would make the distance pass carry every string it
// will not show.
//
// ## Positions are absolute Mercator metres, y growing north
//
// Not the tile-local i16 offsets a .tib way record carries, and **not
// tile-local y, which grows south** (MapProjection::projectTileLocal). A z10
// tile is 39 km of Mercator and an i16 offset covers 32.7 km, so the offset
// would overflow the very tile it was local to. Project with
// MapProjection::projectMercWide() and nothing else.
//
// Every multi-byte field is decoded with memcpy out of a raw byte buffer, never
// a cast-and-dereference -- ESP32-C3 (RISC-V) faults on unaligned multi-byte
// loads. Little endian on the wire; ESP32-C3 and x86 are both little endian, so
// no byte swap is ever needed here -- do not add one.
class MapPointReader {
 public:
  // 64 records. A z10 shard carries tens of points, so this is normally the
  // whole array in one read, and it is 1 KB of RAM held only while a shard is
  // open -- shards are opened one at a time, not all nine at once.
  static constexpr size_t kStreamBufferSize = 1024;

  static constexpr size_t kHeaderBytes = 48;
  static constexpr size_t kRecordBytes = 16;
  static constexpr uint16_t kFormatVersion = 1;
  static constexpr size_t kMaxNameBytes = kPointNameMaxBytes;

  // One record, decoded. Coordinates are absolute Mercator metres; the name is
  // not here -- readName() fetches it, because most records never need one.
  struct Record {
    int32_t x = 0;
    int32_t y = 0;
    MapPointKind kind = MapPointKind::Unknown;
    uint8_t category = 0;
    uint8_t flags = 0;
    uint8_t nameLen = 0;
    uint16_t nameOffset = 0;
  };

  // Parses the header and validates header_crc32. Returns false and leaves the
  // file closed on bad magic, a wrong version, a short read, a non-zero
  // reserved byte, an inconsistent size or a crc mismatch. `file` must outlive
  // this reader; it stays open so records and names can both be read.
  bool open(IFileSource& file, const char* path);
  void close();
  bool isOpen() const { return file_ != nullptr; }

  uint32_t pointCount() const { return pointCount_; }
  uint32_t buildEpoch() const { return buildEpoch_; }
  // Bitmask of MapPointKind values present, straight out of the header: bit N
  // set means kind N is in this file. Lets a caller skip a whole shard when it
  // is drawing safety only and the shard holds landmarks only.
  uint8_t kindsPresent() const { return kindsPresent_; }

  int32_t bboxMinX() const { return bboxMinX_; }
  int32_t bboxMinY() const { return bboxMinY_; }
  int32_t bboxMaxX() const { return bboxMaxX_; }
  int32_t bboxMaxY() const { return bboxMaxY_; }

  // One streaming pass over the records and the name pool, checking
  // body_crc32. Call once per shard open, before trusting a record.
  //
  // A shard that fails this must not be drawn: a corrupt record is a hospital
  // in the wrong place, which is worse on this layer than on any other -- the
  // same honesty rule that hatches a missing tile instead of drawing white.
  bool verifyBody();

  // Rewinds to the first record. Rewindable and cheap: one seek.
  bool beginRecords();
  // Fills the next record; false at the end of the array or on a short read.
  bool nextRecord(Record& out);
  uint32_t recordsRead() const { return recordsRead_; }

  // Copies a record's name into `out` as a null-terminated string, empty when
  // the record has none. False on a seek/read failure or a name that runs past
  // the pool -- corruption a crc can miss when the crc field was the damage.
  //
  // Seeks, and then seeks back to the next unread record, so it is safe to
  // call in the middle of a walk -- what the Nearby list does per printed row.
  // It costs one extra card access, so call it for rows a screen prints and
  // never inside a distance pass.
  bool readName(const Record& record, char* out, size_t outLen);

  uint32_t bytesRead() const { return bytesRead_; }

 private:
  bool parseHeader();
  bool refill();
  bool readRaw(void* dst, size_t len);
  int readCounted(void* dst, size_t len);

  IFileSource* file_ = nullptr;

  uint32_t pointCount_ = 0;
  uint32_t namesLen_ = 0;
  uint32_t bodyCrc32_ = 0;
  uint32_t buildEpoch_ = 0;
  uint8_t kindsPresent_ = 0;
  int32_t bboxMinX_ = 0;
  int32_t bboxMinY_ = 0;
  int32_t bboxMaxX_ = 0;
  int32_t bboxMaxY_ = 0;

  uint8_t streamBuffer_[kStreamBufferSize];
  size_t bufferPos_ = 0;
  size_t bufferFill_ = 0;
  uint32_t cursorAbs_ = 0;
  uint32_t endAbs_ = 0;
  uint32_t recordsRead_ = 0;
  uint32_t bytesRead_ = 0;
};
