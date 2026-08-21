#include "MapPointReader.h"

#include <cstring>

#include "MapCrc32.h"

namespace {

// Byte layout is exactly mapbuilder/tilegen/point_file.py's
// struct.calcsize("<4sHBBIiiiiIII") == 40 fields, then 4 zero bytes of pad,
// then header_crc32 at 44 -- 48 in total, which is why the record array starts
// 16-byte aligned. The offsets below are that layout's cumulative sums; change
// one, change every one after it.
constexpr size_t kMagicLen = 4;
constexpr uint8_t kMagic[kMagicLen] = {'T', 'I', 'P', '1'};
constexpr size_t kOffVersion = 4;
constexpr size_t kOffKinds = 6;
constexpr size_t kOffReserved = 7;
constexpr size_t kOffPointCount = 8;
constexpr size_t kOffBboxMinX = 12;
constexpr size_t kOffBboxMinY = 16;
constexpr size_t kOffBboxMaxX = 20;
constexpr size_t kOffBboxMaxY = 24;
constexpr size_t kOffBuildEpoch = 28;
constexpr size_t kOffNamesLen = 32;
constexpr size_t kOffBodyCrc32 = 36;
constexpr size_t kOffPad = 40;
constexpr size_t kOffHeaderCrc32 = 44;

// Within one 16-byte record.
constexpr size_t kRecKind = 0;
constexpr size_t kRecCategory = 1;
constexpr size_t kRecFlags = 2;
constexpr size_t kRecNameLen = 3;
constexpr size_t kRecX = 4;
constexpr size_t kRecY = 8;
constexpr size_t kRecNameOff = 12;
constexpr size_t kRecReserved = 14;

}  // namespace

bool MapPointReader::open(IFileSource& file, const char* path) {
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

void MapPointReader::close() {
  if (file_) {
    file_->close();
    file_ = nullptr;
  }
  pointCount_ = 0;
  namesLen_ = 0;
  kindsPresent_ = 0;
  recordsRead_ = 0;
  bufferPos_ = 0;
  bufferFill_ = 0;
}

int MapPointReader::readCounted(void* dst, size_t len) {
  const int n = file_->read(dst, len);
  if (n > 0) bytesRead_ += static_cast<uint32_t>(n);
  return n;
}

bool MapPointReader::parseHeader() {
  bytesRead_ = 0;
  recordsRead_ = 0;
  if (!file_->seek(0)) return false;

  uint8_t hdr[kHeaderBytes];
  if (readCounted(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return false;

  if (std::memcmp(hdr, kMagic, kMagicLen) != 0) return false;

  // Version before anything else is trusted. A version-2 file appends a chunk
  // after the name pool; a reader that skipped this check would read those
  // bytes as records.
  uint16_t version = 0;
  std::memcpy(&version, &hdr[kOffVersion], sizeof(version));
  if (version != kFormatVersion) return false;

  // Not a flags field until something needs one. A writer setting it means the
  // file came from something this reader does not understand.
  if (hdr[kOffReserved] != 0) return false;
  for (size_t i = 0; i < 4; ++i) {
    if (hdr[kOffPad + i] != 0) return false;
  }

  kindsPresent_ = hdr[kOffKinds];
  std::memcpy(&pointCount_, &hdr[kOffPointCount], sizeof(pointCount_));
  std::memcpy(&bboxMinX_, &hdr[kOffBboxMinX], sizeof(bboxMinX_));
  std::memcpy(&bboxMinY_, &hdr[kOffBboxMinY], sizeof(bboxMinY_));
  std::memcpy(&bboxMaxX_, &hdr[kOffBboxMaxX], sizeof(bboxMaxX_));
  std::memcpy(&bboxMaxY_, &hdr[kOffBboxMaxY], sizeof(bboxMaxY_));
  std::memcpy(&buildEpoch_, &hdr[kOffBuildEpoch], sizeof(buildEpoch_));
  std::memcpy(&namesLen_, &hdr[kOffNamesLen], sizeof(namesLen_));
  std::memcpy(&bodyCrc32_, &hdr[kOffBodyCrc32], sizeof(bodyCrc32_));

  uint32_t headerCrcStored = 0;
  std::memcpy(&headerCrcStored, &hdr[kOffHeaderCrc32], sizeof(headerCrcStored));

  // header_crc32 with its own field zeroed, over the whole 48-byte header. The
  // bytes are already in this local buffer, so it costs no extra file access.
  std::memset(&hdr[kOffHeaderCrc32], 0, sizeof(headerCrcStored));
  uint32_t crc = MapCrc32::kInit;
  crc = MapCrc32::update(crc, hdr, kHeaderBytes);
  crc = MapCrc32::final(crc);
  if (crc != headerCrcStored) return false;

  // Only trusted once the crc has passed, which is the whole reason it is
  // checked first: point_count sizes every later read.
  if (pointCount_ == 0) return false;
  if (bboxMinX_ > bboxMaxX_ || bboxMinY_ > bboxMaxY_) return false;
  return true;
}

bool MapPointReader::verifyBody() {
  if (!file_) return false;
  if (!file_->seek(static_cast<uint32_t>(kHeaderBytes))) return false;

  uint32_t remaining = pointCount_ * static_cast<uint32_t>(kRecordBytes) + namesLen_;
  uint32_t crc = MapCrc32::kInit;
  while (remaining > 0) {
    const size_t toRead = remaining < kStreamBufferSize ? remaining : kStreamBufferSize;
    const int n = readCounted(streamBuffer_, toRead);
    if (n <= 0) return false;
    crc = MapCrc32::update(crc, streamBuffer_, static_cast<size_t>(n));
    remaining -= static_cast<uint32_t>(n);
  }
  return MapCrc32::final(crc) == bodyCrc32_;
}

bool MapPointReader::beginRecords() {
  if (!file_) return false;
  if (!file_->seek(static_cast<uint32_t>(kHeaderBytes))) return false;
  cursorAbs_ = static_cast<uint32_t>(kHeaderBytes);
  endAbs_ = cursorAbs_ + pointCount_ * static_cast<uint32_t>(kRecordBytes);
  bufferPos_ = 0;
  bufferFill_ = 0;
  recordsRead_ = 0;
  return true;
}

bool MapPointReader::refill() {
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

bool MapPointReader::readRaw(void* dst, size_t len) {
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

bool MapPointReader::nextRecord(Record& out) {
  if (recordsRead_ >= pointCount_) return false;
  uint8_t buf[kRecordBytes];
  if (!readRaw(buf, sizeof(buf))) return false;

  uint16_t reserved = 0;
  std::memcpy(&reserved, &buf[kRecReserved], sizeof(reserved));
  if (reserved != 0) return false;

  out.kind = static_cast<MapPointKind>(buf[kRecKind]);
  out.category = buf[kRecCategory];
  out.flags = buf[kRecFlags];
  out.nameLen = buf[kRecNameLen];
  std::memcpy(&out.x, &buf[kRecX], sizeof(out.x));
  std::memcpy(&out.y, &buf[kRecY], sizeof(out.y));
  std::memcpy(&out.nameOffset, &buf[kRecNameOff], sizeof(out.nameOffset));

  // Bounds the crc cannot catch when the crc field itself was the damage. A
  // record outside the declared bbox is a corrupt file, not a wider shard.
  if (out.nameLen > kMaxNameBytes) return false;
  if (static_cast<uint32_t>(out.nameOffset) + out.nameLen > namesLen_) return false;
  if (out.x < bboxMinX_ || out.x > bboxMaxX_ || out.y < bboxMinY_ || out.y > bboxMaxY_) return false;

  ++recordsRead_;
  return true;
}

bool MapPointReader::readName(const Record& record, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) return false;
  out[0] = '\0';
  if (!file_) return false;
  if (record.nameLen == 0) return true;
  if (static_cast<uint32_t>(record.nameOffset) + record.nameLen > namesLen_) return false;

  const uint32_t poolStart = static_cast<uint32_t>(kHeaderBytes) + pointCount_ * static_cast<uint32_t>(kRecordBytes);
  if (!file_->seek(poolStart + record.nameOffset)) return false;

  // Truncate to the caller's buffer rather than refusing: a name that does not
  // fit a list row is still worth printing as far as it goes, and the writer
  // already caps a name at kMaxNameBytes. Reads only what will be kept.
  const size_t take = record.nameLen < outLen - 1 ? record.nameLen : outLen - 1;
  if (readCounted(out, take) != static_cast<int>(take)) {
    out[0] = '\0';
    return false;
  }
  out[take] = '\0';

  // Put the record cursor back exactly where the walk left it, computed from
  // recordsRead_ rather than from the buffer: this call seeked away, and the
  // bytes still sitting in the stream buffer are no longer the ones that follow
  // the file position. So the buffer is dropped and the next refill() starts at
  // the next unread record. That is what makes readName() safe to call in the
  // middle of a walk, which the Nearby list does for every row it prints.
  const uint32_t nextRecordAbs =
      static_cast<uint32_t>(kHeaderBytes) + recordsRead_ * static_cast<uint32_t>(kRecordBytes);
  bufferPos_ = 0;
  bufferFill_ = 0;
  cursorAbs_ = nextRecordAbs;
  if (!file_->seek(nextRecordAbs)) return false;
  return true;
}
