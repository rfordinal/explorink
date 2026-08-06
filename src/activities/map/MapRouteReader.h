#pragma once

#include <cstddef>
#include <cstdint>

#include "IFileSource.h"

// Reads .tir route files -- ../../../docs/route-file-spec.md in the parent
// xteink repo. Header parse, crc32 validation, then streaming iteration over
// the point array.
//
// The trip layer is one file, not tiles (docs/map-data-spec.md, "The trip
// layer is one file, not tiles"): a route is not browsed spatially, and the
// device needs all of it at once to fit it on screen.
//
// ## RAM is O(1) in route length
//
// Same rule MapTileReader follows, for the same reason. Points stream through
// a fixed kStreamBufferSize buffer; nothing accumulates. A 3,000-point route
// is 24 KB on the card and costs this class kStreamBufferSize of RAM, whatever
// its length. Never read the point array into a vector -- the renderer draws
// segment by segment and needs no more than two points at a time.
//
// ## Points are absolute Mercator metres, y growing north
//
// Not the tile-local i16 offsets a .tib way record carries, and **not
// tile-local y, which grows south** (MapProjection::projectTileLocal). A route
// has no tile origin to be local to, and a day's ride is longer than the 21 km
// of ground an i16 Mercator offset covers at this latitude. Project a point
// with MapProjection::projectMerc() and nothing else.
//
// ## Two checksums, at two different times
//
// - `header_crc32` covers the fixed header and the name. Checked in open(),
//   not optional: point_count and the point array's offset come out of those
//   bytes, so a corrupt header sends the reader off the end of the file and no
//   later check can undo that.
// - `points_crc32` covers the point array, and is checked by verifyPoints() --
//   a separate call, so opening a route to read its name and length for a
//   picker row costs 104 bytes, not 24 KB. The owner calls it once when the
//   route is loaded; beginPoints() does not re-check on every viewport reset,
//   because the file on the card does not change between them.
//
// Every multi-byte field is decoded with memcpy out of a raw byte buffer,
// never a cast-and-dereference -- ESP32-C3 (RISC-V) faults on unaligned
// multi-byte loads. Little endian on the wire; ESP32-C3 and x86 are both
// little endian, so no byte swap is ever needed here -- do not add one.
class MapRouteReader {
 public:
  // 128 points per read. Small on purpose: this buffer is alive for as long as
  // a route is loaded, next to MapTileSource's own 4 KB, and a route is read
  // sequentially exactly once per viewport reset. 24 reads for a 3,000-point
  // route against six for a 4 KB buffer -- a handful of milliseconds on a
  // reset that already pays 1,800 ms for the refresh.
  static constexpr size_t kStreamBufferSize = 1024;

  // Bytes before the name in the header. The point array starts at
  // roundUp4(kHeaderFixedBytes + nameLen).
  static constexpr size_t kHeaderFixedBytes = 40;
  static constexpr size_t kMaxNameBytes = 64;
  static constexpr uint32_t kMinPoints = 2;

  // The one .tir format version this build reads. Exact equality, never a
  // range: a later version appends a chunk table after the point array, and a
  // reader that ignored the version would read its bytes as points.
  static constexpr uint16_t kFormatVersion = 1;

  // Parses the header and validates header_crc32. Returns false and leaves the
  // file closed on bad magic, a wrong version, a short read, a name over the
  // cap, a non-zero reserved byte, or a crc mismatch. `file` must outlive this
  // reader; it stays open so beginPoints() can seek back without reopening.
  bool open(IFileSource& file, const char* path);
  void close();

  bool isOpen() const { return file_ != nullptr; }

  uint32_t pointCount() const { return pointCount_; }
  uint32_t buildEpoch() const { return buildEpoch_; }
  // Null-terminated, empty when the file carries no name. Truncated to
  // kMaxNameBytes, which the format also caps, so this never truncates a name
  // a valid writer produced.
  const char* name() const { return name_; }

  // The route's axis-aligned Mercator extent, straight out of the header. The
  // overview fit needs a centre before it streams a single point
  // (MapRouteFit.h), and having it here is what makes that one pass instead of
  // two.
  int32_t bboxMinX() const { return bboxMinX_; }
  int32_t bboxMinY() const { return bboxMinY_; }
  int32_t bboxMaxX() const { return bboxMaxX_; }
  int32_t bboxMaxY() const { return bboxMaxY_; }

  // One streaming pass over the point array, checking points_crc32. Call once
  // when a route is loaded. False means the file is corrupt and must not be
  // drawn: half a route on screen is a route that ends somewhere it does not,
  // which is worse than no route at all -- the same honesty rule that hatches
  // a missing tile instead of drawing white.
  bool verifyPoints();

  // Rewinds to the first point. Rewindable, and cheap: one seek. Called once
  // per viewport reset and once per fit pass.
  bool beginPoints();
  // Fills the next point and returns true; false at the end of the array or on
  // a short read. Coordinates are absolute Mercator metres, y growing north.
  bool nextPoint(int32_t& outX, int32_t& outY);
  // Points handed out since the last beginPoints().
  uint32_t pointsRead() const { return pointsRead_; }

  // Real bytes pulled from the file since open(), header included. The route
  // is read once per viewport reset, so this is what says whether that read is
  // worth worrying about next to the tiles' own.
  uint32_t bytesRead() const { return bytesRead_; }

 private:
  bool parseHeader();
  bool readRaw(void* dst, size_t len);
  bool refill();
  int readCounted(void* dst, size_t len);

  IFileSource* file_ = nullptr;

  uint32_t pointCount_ = 0;
  uint32_t pointsOffset_ = 0;
  int32_t bboxMinX_ = 0;
  int32_t bboxMinY_ = 0;
  int32_t bboxMaxX_ = 0;
  int32_t bboxMaxY_ = 0;
  uint32_t buildEpoch_ = 0;
  uint32_t pointsCrc32_ = 0;
  char name_[kMaxNameBytes + 1] = {};

  uint8_t streamBuffer_[kStreamBufferSize];
  size_t bufferPos_ = 0;
  size_t bufferFill_ = 0;
  uint32_t cursorAbs_ = 0;
  uint32_t endAbs_ = 0;
  uint32_t pointsRead_ = 0;
  uint32_t bytesRead_ = 0;
};
