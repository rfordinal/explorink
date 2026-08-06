#include "MapRouteReader.h"

#include <cstring>

#include "MapCrc32.h"

namespace {

// Byte layout is exactly mapbuilder/route_file.py's
// struct.calcsize("<4sHBBIiiiiIII") == 40: magic(4) version(2) name_len(1)
// reserved(1) point_count(4) bbox_min_x(4) bbox_min_y(4) bbox_max_x(4)
// bbox_max_y(4) build_epoch(4) points_crc32(4) header_crc32(4). The offsets
// below are that layout's cumulative sums -- change one, change all after it.
constexpr size_t kMagicLen = 4;
constexpr uint8_t kMagic[kMagicLen] = {'T', 'I', 'R', '1'};
constexpr size_t kOffVersion = 4;
constexpr size_t kOffNameLen = 6;
constexpr size_t kOffReserved = 7;
constexpr size_t kOffPointCount = 8;
constexpr size_t kOffBboxMinX = 12;
constexpr size_t kOffBboxMinY = 16;
constexpr size_t kOffBboxMaxX = 20;
constexpr size_t kOffBboxMaxY = 24;
constexpr size_t kOffBuildEpoch = 28;
constexpr size_t kOffPointsCrc32 = 32;
constexpr size_t kOffHeaderCrc32 = 36;

constexpr size_t kPointBytes = 8;  // i32 x, i32 y

inline uint32_t roundUp4(uint32_t value) { return (value + 3u) & ~3u; }

}  // namespace

bool MapRouteReader::open(IFileSource& file, const char* path) {
  file_ = &file;
  if (!file_->open(path)) {
    file_ = nullptr;
    return false;
  }
  if (!parseHeader()) {
    file_->close();
    file_ = nullptr;
    return false;
  }
  return true;
}

void MapRouteReader::close() {
  if (file_) {
    file_->close();
    file_ = nullptr;
  }
  pointCount_ = 0;
  pointsRead_ = 0;
  bufferPos_ = 0;
  bufferFill_ = 0;
  name_[0] = '\0';
}

int MapRouteReader::readCounted(void* dst, size_t len) {
  const int n = file_->read(dst, len);
  if (n > 0) bytesRead_ += static_cast<uint32_t>(n);
  return n;
}

bool MapRouteReader::parseHeader() {
  bytesRead_ = 0;
  pointsRead_ = 0;
  name_[0] = '\0';
  if (!file_->seek(0)) return false;

  uint8_t hdr[kHeaderFixedBytes];
  if (readCounted(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return false;

  if (std::memcmp(hdr, kMagic, kMagicLen) != 0) return false;

  // Version before anything else is trusted. A version-2 file appends a chunk
  // table after the point array; a reader that skipped this check would read
  // that table's bytes as coordinates rather than refuse the file.
  uint16_t version = 0;
  std::memcpy(&version, &hdr[kOffVersion], sizeof(version));
  if (version != kFormatVersion) return false;

  const uint8_t nameLen = hdr[kOffNameLen];
  if (nameLen > kMaxNameBytes) return false;
  // Not a flags field until something needs one. A writer setting it means the
  // file was produced by something this reader does not understand.
  if (hdr[kOffReserved] != 0) return false;

  std::memcpy(&pointCount_, &hdr[kOffPointCount], sizeof(pointCount_));
  std::memcpy(&bboxMinX_, &hdr[kOffBboxMinX], sizeof(bboxMinX_));
  std::memcpy(&bboxMinY_, &hdr[kOffBboxMinY], sizeof(bboxMinY_));
  std::memcpy(&bboxMaxX_, &hdr[kOffBboxMaxX], sizeof(bboxMaxX_));
  std::memcpy(&bboxMaxY_, &hdr[kOffBboxMaxY], sizeof(bboxMaxY_));
  std::memcpy(&buildEpoch_, &hdr[kOffBuildEpoch], sizeof(buildEpoch_));
  std::memcpy(&pointsCrc32_, &hdr[kOffPointsCrc32], sizeof(pointsCrc32_));

  uint32_t headerCrcStored = 0;
  std::memcpy(&headerCrcStored, &hdr[kOffHeaderCrc32], sizeof(headerCrcStored));

  uint8_t nameBytes[kMaxNameBytes];
  if (nameLen > 0) {
    if (readCounted(nameBytes, nameLen) != static_cast<int>(nameLen)) return false;
  }

  // Padding to the 4-byte boundary is part of the header, so it is part of the
  // crc: a writer that filled it with anything but zeros produces a file this
  // refuses, which is the intent.
  pointsOffset_ = roundUp4(static_cast<uint32_t>(kHeaderFixedBytes) + nameLen);
  const uint32_t padLen = pointsOffset_ - (static_cast<uint32_t>(kHeaderFixedBytes) + nameLen);
  uint8_t pad[3] = {0, 0, 0};
  if (padLen > 0) {
    if (readCounted(pad, padLen) != static_cast<int>(padLen)) return false;
  }

  // header_crc32 with its own field zeroed, over everything before the point
  // array. All of it is already in these local buffers from the reads above,
  // so this costs no extra file access.
  std::memset(&hdr[kOffHeaderCrc32], 0, sizeof(headerCrcStored));
  uint32_t crc = MapCrc32::kInit;
  crc = MapCrc32::update(crc, hdr, kHeaderFixedBytes);
  if (nameLen > 0) crc = MapCrc32::update(crc, nameBytes, nameLen);
  if (padLen > 0) crc = MapCrc32::update(crc, pad, padLen);
  crc = MapCrc32::final(crc);
  if (crc != headerCrcStored) return false;

  // Only trusted after the crc passed, which is the whole point of checking it
  // before anything else: point_count sizes every later read.
  if (pointCount_ < kMinPoints) return false;
  if (bboxMinX_ > bboxMaxX_ || bboxMinY_ > bboxMaxY_) return false;

  std::memcpy(name_, nameBytes, nameLen);
  name_[nameLen] = '\0';
  return true;
}

bool MapRouteReader::verifyPoints() {
  if (!file_) return false;
  if (!file_->seek(pointsOffset_)) return false;

  uint32_t remaining = pointCount_ * kPointBytes;
  uint32_t crc = MapCrc32::kInit;
  while (remaining > 0) {
    const size_t toRead = remaining < kStreamBufferSize ? remaining : kStreamBufferSize;
    const int n = readCounted(streamBuffer_, toRead);
    if (n <= 0) return false;
    crc = MapCrc32::update(crc, streamBuffer_, static_cast<size_t>(n));
    remaining -= static_cast<uint32_t>(n);
  }
  return MapCrc32::final(crc) == pointsCrc32_;
}

bool MapRouteReader::beginPoints() {
  if (!file_) return false;
  if (!file_->seek(pointsOffset_)) return false;
  cursorAbs_ = pointsOffset_;
  endAbs_ = pointsOffset_ + pointCount_ * kPointBytes;
  bufferPos_ = 0;
  bufferFill_ = 0;
  pointsRead_ = 0;
  return true;
}

bool MapRouteReader::refill() {
  const uint32_t avail = endAbs_ - cursorAbs_;
  if (avail == 0) return false;
  const size_t toRead = avail < kStreamBufferSize ? avail : kStreamBufferSize;
  const int n = readCounted(streamBuffer_, toRead);
  if (n <= 0) return false;
  bufferFill_ = static_cast<size_t>(n);
  bufferPos_ = 0;
  cursorAbs_ += static_cast<uint32_t>(n);
  return true;
}

bool MapRouteReader::readRaw(void* dst, size_t len) {
  uint8_t* out = static_cast<uint8_t*>(dst);
  while (len > 0) {
    if (bufferPos_ >= bufferFill_) {
      if (!refill()) return false;
    }
    const size_t avail = bufferFill_ - bufferPos_;
    const size_t take = avail < len ? avail : len;
    std::memcpy(out, &streamBuffer_[bufferPos_], take);
    bufferPos_ += take;
    out += take;
    len -= take;
  }
  return true;
}

bool MapRouteReader::nextPoint(int32_t& outX, int32_t& outY) {
  if (pointsRead_ >= pointCount_) return false;
  uint8_t buf[kPointBytes];
  if (!readRaw(buf, sizeof(buf))) return false;
  std::memcpy(&outX, &buf[0], sizeof(outX));
  std::memcpy(&outY, &buf[4], sizeof(outY));
  ++pointsRead_;
  return true;
}
