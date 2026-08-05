#pragma once

#include <cstddef>
#include <cstdint>

#include "IFileSource.h"

// Reads .tib tiles -- docs/map-data-spec.md, "Tile file format". Header
// parse, crc32 validation, layer directory, then streaming iteration over
// one layer at a time through a fixed internal buffer.
//
// RAM is O(1) in tile size, not O(tile): the internal stream buffer is a
// fixed kStreamBufferSize regardless of how large the layer being read is,
// and a way's points are delivered into a caller-owned buffer capped at 256
// points (mapbuilder splits longer ways at build time for exactly this).
// Never read a layer into a vector "for now" -- that defeats the whole
// design and will only surface as a RAM problem on real hardware.
//
// Every multi-byte field is decoded with memcpy from a raw byte buffer,
// never via a cast-and-dereference -- ESP32-C3 (RISC-V) faults on unaligned
// multi-byte loads. Little-endian on the wire throughout; ESP32-C3 and x86
// are both little-endian, so no byte swap is ever needed here -- do not add
// one.
class MapTileReader {
 public:
  static constexpr size_t kStreamBufferSize = 4096;
  static constexpr uint16_t kMaxWayPoints = 256;
  // A tile carrying more layers than this is rejected outright by open(), so
  // this is the number that has to move first when the builder gains a layer.
  // It did on 2026-08-05 (landuse), and until this was 6 every new tile read as
  // an unavailable one -- a whole card of hatch.
  static constexpr size_t kMaxLayers = 6;

  enum class Layer : uint8_t {
    Water = 1,
    Buildings = 2,
    Roads = 3,
    Places = 4,
    Junctions = 5,
    Landuse = 6,
  };

  struct WayHeader {
    uint8_t classId = 0;
    uint8_t roughness = 0;
    uint16_t flags = 0;
    uint16_t pointCount = 0;
  };

  struct PlaceHeader {
    int16_t x = 0;
    int16_t y = 0;
    uint8_t rank = 0;
    uint8_t nameLen = 0;
  };

  struct JunctionRecord {
    int16_t x = 0;
    int16_t y = 0;
    uint32_t classMask = 0;
  };

  // Opens `path` through `file` (already constructed, not yet open), parses
  // the fixed header and layer directory, and validates `header_crc32` --
  // which covers only those bytes, not the whole file. Returns false and
  // leaves the file closed on any failure: bad magic, wrong format version,
  // short read, or a header crc mismatch. A layer's own bytes are only
  // fetched and crc-checked when beginLayer() actually opens that layer, so
  // a layer never touched (buildings, water, junctions when the caller only
  // draws roads and places) never costs a read.
  bool open(IFileSource& file, const char* path);
  void close();

  uint8_t z() const { return z_; }
  uint32_t tileX() const { return x_; }
  uint32_t tileY() const { return y_; }
  int32_t originX() const { return originX_; }
  int32_t originY() const { return originY_; }
  uint32_t buildEpoch() const { return buildEpoch_; }
  uint32_t osmEpoch() const { return osmEpoch_; }

  bool hasLayer(Layer layer) const;
  uint32_t layerLength(Layer layer) const;

  // Validates that layer's own crc32 -- a second full pass over just its
  // bytes, through the same fixed stream buffer -- then seeks back to its
  // start and resets the streaming cursor. Only one layer is open for
  // reading at a time. Returns false if the layer is absent, empty, or
  // fails its crc32 (which the caller must treat the same as "tile
  // unavailable", not "nothing here": findLayer() already ruled out
  // "absent" by the time crc is checked, so a false here past that point is
  // corrupt data, not an empty layer).
  bool beginLayer(Layer layer);

  // Reads one way record's fixed header. Follow with readWayPoints() for
  // exactly `out.pointCount` points before reading the next way header.
  bool readWayHeader(WayHeader& out);
  // `outXs`/`outYs` must have room for `count` entries (count == the
  // WayHeader.pointCount just read). A `count` above kMaxWayPoints is
  // rejected here, before a single byte is read: mapbuilder splits longer
  // ways at build time, so a larger count means a corrupt file, and the cap
  // belongs in the reader rather than in every caller's hands. On false the
  // stream cursor has not moved past the record, so the caller must abandon
  // the layer rather than continue.
  bool readWayPoints(int16_t* outXs, int16_t* outYs, uint16_t count);

  bool readPlaceHeader(PlaceHeader& out);
  // Reads exactly `out.nameLen` bytes from the stream regardless of
  // `bufCap`, so the cursor always lands on the next record -- truncates
  // into the caller's buffer if the name is longer than `bufCap` allows.
  // Writes a null terminator within `bufCap`.
  bool readPlaceName(const PlaceHeader& header, char* buf, size_t bufCap);

  bool readJunctionRecord(JunctionRecord& out);

  // Real bytes pulled from `file` for the tile currently open -- header,
  // directory, every layer-crc validation pass, every streamed record.
  // Reset to zero at the start of open(). This is what gate 4
  // (docs/PROGRESS.md) measures against the whole-file-crc32 baseline: the
  // point of the per-layer split is that a layer never opened costs zero of
  // these, not that it costs less.
  uint32_t bytesRead() const { return bytesRead_; }

  // Reads bytesRead() and zeroes it in one step. A caller that accumulates
  // this into its own running total (MapTileSource::closeCurrentTile())
  // must use this, not bytesRead(): closeCurrentTile() can run more than
  // once between one open() and the next (once when a tile's layer runs
  // out, again from advanceToNextTile()'s own top-of-function close), and
  // bytesRead() alone would hand back the same already-banked count on the
  // second call, double-counting it. Zeroing on read makes every capture
  // safe to repeat.
  uint32_t takeBytesRead() {
    const uint32_t n = bytesRead_;
    bytesRead_ = 0;
    return n;
  }

  // There is deliberately no peakBufferBytes() accessor here. It used to
  // return kStreamBufferSize, and the test asserting it equalled
  // kStreamBufferSize proved nothing -- it would have passed with a
  // whole-file vector bolted on. The O(1) claim is measured instead, as a
  // real heap high-water mark across tiles of very different sizes, in
  // test/map_tile_reader/MapTileReaderGoldenTest.cpp.

 private:
  static constexpr uint16_t kFormatVersion = 2;

  struct LayerEntry {
    uint8_t id = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t crc32 = 0;
  };

  bool parseHeader();
  bool validateLayerCrc32(const LayerEntry& entry);
  const LayerEntry* findLayer(Layer layer) const;
  bool readRaw(void* dst, size_t len);
  bool refill();
  // The one place that ever calls file_->read(). Every byte it returns is
  // real I/O, so this is also the one place bytesRead_ is incremented.
  int readCounted(void* dst, size_t len);

  IFileSource* file_ = nullptr;

  uint16_t version_ = 0;
  uint8_t z_ = 0;
  uint32_t x_ = 0;
  uint32_t y_ = 0;
  int32_t originX_ = 0;
  int32_t originY_ = 0;
  uint32_t buildEpoch_ = 0;
  uint32_t osmEpoch_ = 0;
  uint32_t headerCrc32Stored_ = 0;
  uint8_t layerCount_ = 0;
  LayerEntry layers_[kMaxLayers];

  uint8_t streamBuffer_[kStreamBufferSize];
  size_t bufferPos_ = 0;
  size_t bufferFill_ = 0;
  uint32_t layerCursorAbs_ = 0;
  uint32_t layerEndAbs_ = 0;
  uint32_t bytesRead_ = 0;
};
