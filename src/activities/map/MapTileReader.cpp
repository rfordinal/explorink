#include "MapTileReader.h"

#include <cstring>

namespace {

// Byte layout is exactly mapbuilder/tiles.py's struct.calcsize("<4sHBIIiiIIIB")
// == 36: magic(4) version(2) z(1) x(4) y(4) origin_x(4) origin_y(4)
// build_epoch(4) osm_epoch(4) header_crc32(4) layer_count(1). Field offsets
// below are that layout's cumulative sums -- change one, change all after it.
//
// Format version 2: header_crc32 covers only these 36 bytes plus the layer
// directory that follows (with header_crc32 itself zeroed while computing),
// not the whole file. Each directory entry is 13 bytes -- id, offset,
// length, and that layer's own crc32 -- up from version 1's 9 (no
// per-layer crc). A reader must check the version before trusting the
// directory's shape at all: parsing a v1 file's 9-byte entries as v2's
// 13-byte ones produces plausible-looking garbage offsets, not an error.
constexpr size_t kHeaderFixedLen = 36;
constexpr size_t kCrcFieldOffset = 31;  // struct.calcsize("<4sHBIIiiII")
constexpr size_t kDirEntryLen = 13;     // <BIII>
constexpr uint8_t kMagic[4] = {'T', 'I', 'B', '1'};

// Standard IEEE CRC32 (poly 0xEDB88320, reflected), the same algorithm
// zlib.crc32 uses -- must match mapbuilder/tiles.py's zlib.crc32 bit-for-bit
// for round-trip validation to mean anything. Self-contained: no zlib/miniz
// dependency, portable to the device build in P4 as-is.
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc;
}

}  // namespace

bool MapTileReader::open(IFileSource& file, const char* path) {
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

void MapTileReader::close() {
  if (file_) {
    file_->close();
    file_ = nullptr;
  }
}

bool MapTileReader::parseHeader() {
  if (!file_->seek(0)) return false;

  uint8_t hdr[kHeaderFixedLen];
  int n = file_->read(hdr, sizeof(hdr));
  if (n != static_cast<int>(sizeof(hdr))) return false;

  if (std::memcmp(hdr, kMagic, sizeof(kMagic)) != 0) return false;

  size_t off = 4;
  std::memcpy(&version_, &hdr[off], sizeof(version_));
  off += sizeof(version_);

  // Version before anything else is trusted: a version-1 file's directory
  // entries are 9 bytes, not this version's 13, so parsing them as v2 would
  // produce plausible-looking garbage offsets rather than a clean refusal.
  if (version_ != kFormatVersion) return false;

  z_ = hdr[off];
  off += 1;
  std::memcpy(&x_, &hdr[off], sizeof(x_));
  off += sizeof(x_);
  std::memcpy(&y_, &hdr[off], sizeof(y_));
  off += sizeof(y_);
  std::memcpy(&originX_, &hdr[off], sizeof(originX_));
  off += sizeof(originX_);
  std::memcpy(&originY_, &hdr[off], sizeof(originY_));
  off += sizeof(originY_);
  std::memcpy(&buildEpoch_, &hdr[off], sizeof(buildEpoch_));
  off += sizeof(buildEpoch_);
  std::memcpy(&osmEpoch_, &hdr[off], sizeof(osmEpoch_));
  off += sizeof(osmEpoch_);
  std::memcpy(&headerCrc32Stored_, &hdr[off], sizeof(headerCrc32Stored_));
  off += sizeof(headerCrc32Stored_);
  layerCount_ = hdr[off];

  if (layerCount_ > kMaxLayers) return false;

  uint8_t dir[kMaxLayers * kDirEntryLen];
  const size_t dirLen = layerCount_ * kDirEntryLen;
  n = file_->read(dir, dirLen);
  if (n != static_cast<int>(dirLen)) return false;

  // header_crc32 covers the fixed header (with its own field zeroed) plus
  // the layer directory -- both already sitting in these two local buffers
  // from the reads just above, so this costs no extra file access. This is
  // the only crc check open() ever pays for; a layer's own bytes are
  // checked only if and when beginLayer() actually opens that layer.
  std::memset(&hdr[kCrcFieldOffset], 0, sizeof(headerCrc32Stored_));
  uint32_t crc = 0xFFFFFFFFu;
  crc = crc32Update(crc, hdr, kHeaderFixedLen);
  crc = crc32Update(crc, dir, dirLen);
  crc ^= 0xFFFFFFFFu;
  if (crc != headerCrc32Stored_) return false;

  for (uint8_t i = 0; i < layerCount_; ++i) {
    const uint8_t* entry = &dir[i * kDirEntryLen];
    LayerEntry& out = layers_[i];
    out.id = entry[0];
    std::memcpy(&out.offset, &entry[1], sizeof(out.offset));
    std::memcpy(&out.length, &entry[5], sizeof(out.length));
    std::memcpy(&out.crc32, &entry[9], sizeof(out.crc32));
  }
  return true;
}

bool MapTileReader::validateLayerCrc32(const LayerEntry& entry) {
  if (!file_->seek(entry.offset)) return false;

  uint32_t crc = 0xFFFFFFFFu;
  uint32_t remaining = entry.length;
  while (remaining > 0) {
    const size_t toRead = remaining < kStreamBufferSize ? static_cast<size_t>(remaining) : kStreamBufferSize;
    const int n = file_->read(streamBuffer_, toRead);
    if (n <= 0) return false;
    crc = crc32Update(crc, streamBuffer_, static_cast<size_t>(n));
    remaining -= static_cast<uint32_t>(n);
  }
  crc ^= 0xFFFFFFFFu;

  return crc == entry.crc32;
}

const MapTileReader::LayerEntry* MapTileReader::findLayer(Layer layer) const {
  const uint8_t id = static_cast<uint8_t>(layer);
  for (uint8_t i = 0; i < layerCount_; ++i) {
    if (layers_[i].id == id) return &layers_[i];
  }
  return nullptr;
}

bool MapTileReader::hasLayer(Layer layer) const {
  const LayerEntry* e = findLayer(layer);
  return e != nullptr && e->length > 0;
}

uint32_t MapTileReader::layerLength(Layer layer) const {
  const LayerEntry* e = findLayer(layer);
  return e ? e->length : 0;
}

bool MapTileReader::beginLayer(Layer layer) {
  const LayerEntry* e = findLayer(layer);
  if (!e || e->length == 0) return false;
  if (!validateLayerCrc32(*e)) return false;
  if (!file_->seek(e->offset)) return false;

  layerCursorAbs_ = e->offset;
  layerEndAbs_ = e->offset + e->length;
  bufferPos_ = 0;
  bufferFill_ = 0;
  return true;
}

bool MapTileReader::refill() {
  const uint32_t avail = layerEndAbs_ - layerCursorAbs_;
  if (avail == 0) return false;
  const size_t toRead = avail < kStreamBufferSize ? avail : kStreamBufferSize;
  const int n = file_->read(streamBuffer_, toRead);
  if (n <= 0) return false;
  bufferFill_ = static_cast<size_t>(n);
  bufferPos_ = 0;
  layerCursorAbs_ += static_cast<uint32_t>(n);
  return true;
}

bool MapTileReader::readRaw(void* dst, size_t len) {
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

bool MapTileReader::readWayHeader(WayHeader& out) {
  uint8_t buf[6];
  if (!readRaw(buf, sizeof(buf))) return false;
  out.classId = buf[0];
  out.roughness = buf[1];
  std::memcpy(&out.flags, &buf[2], sizeof(out.flags));
  std::memcpy(&out.pointCount, &buf[4], sizeof(out.pointCount));
  return true;
}

bool MapTileReader::readWayPoints(int16_t* outXs, int16_t* outYs, uint16_t count) {
  // Enforced here, not by the caller. A corrupt point_count on the card
  // would otherwise be a stack buffer overflow in whichever caller forgot to
  // check -- and P4 adds a second caller.
  if (count > kMaxWayPoints) return false;
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t buf[4];
    if (!readRaw(buf, sizeof(buf))) return false;
    std::memcpy(&outXs[i], &buf[0], sizeof(int16_t));
    std::memcpy(&outYs[i], &buf[2], sizeof(int16_t));
  }
  return true;
}

bool MapTileReader::readPlaceHeader(PlaceHeader& out) {
  uint8_t buf[6];
  if (!readRaw(buf, sizeof(buf))) return false;
  std::memcpy(&out.x, &buf[0], sizeof(out.x));
  std::memcpy(&out.y, &buf[2], sizeof(out.y));
  out.rank = buf[4];
  out.nameLen = buf[5];
  return true;
}

bool MapTileReader::readPlaceName(const PlaceHeader& header, char* buf, size_t bufCap) {
  const size_t toBuf = bufCap > 0 ? (header.nameLen < bufCap - 1 ? header.nameLen : bufCap - 1) : 0;
  if (toBuf > 0 && !readRaw(buf, toBuf)) return false;
  if (bufCap > 0) buf[toBuf] = '\0';

  size_t remaining = header.nameLen - toBuf;
  uint8_t discard[64];
  while (remaining > 0) {
    const size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
    if (!readRaw(discard, chunk)) return false;
    remaining -= chunk;
  }
  return true;
}

bool MapTileReader::readJunctionRecord(JunctionRecord& out) {
  uint8_t buf[8];
  if (!readRaw(buf, sizeof(buf))) return false;
  std::memcpy(&out.x, &buf[0], sizeof(out.x));
  std::memcpy(&out.y, &buf[2], sizeof(out.y));
  std::memcpy(&out.classMask, &buf[4], sizeof(out.classMask));
  return true;
}
