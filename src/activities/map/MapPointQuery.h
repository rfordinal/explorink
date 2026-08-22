#pragma once

#include <cstddef>
#include <cstdint>

#include "IFileSource.h"
#include "MapPointReader.h"
#include "MapPointShards.h"
#include "MapPointTypes.h"

// The radius search behind the `Nearby` menu: what useful things are around the
// rider, from the **GPS fix** and not from the viewport
// (../../../docs/safety-concept.md, "Nearby").
//
// That is the whole reason this exists next to MapPointSource. The render source
// walks the shards a viewport touches and projects to screen; this one walks the
// shards a 25 km circle touches and measures ground distance. A hospital 14 km
// away has to be listed while the map shows 3x5 km, so the two questions cannot
// share a walk.
//
// ## Two passes, and only one of them reads a name
//
// - `nearestPerCategory()` fills one distance per category for the menu's rows.
//   Records only, no names: 100 points is 1.6 kB of card reads for ten numbers.
// - `listCategory()` fills the rows of one category's screen, sorted by
//   distance, and reads a name per row it keeps.
//
// ## No clever reordering
//
// `listCategory()` sorts by distance and nothing else. An unverified spring at
// 700 m stays above a confirmed tap at 2.1 km: ranking one over the other is the
// device deciding something the rider should decide, and it hides the data
// instead of showing it (safety-concept.md, "Honesty rules").
//
// ## RAM
//
// One MapPointReader (1 KB) plus the caller's own result array. Nothing else, no
// heap. About 1.2 KB, past CLAUDE.md's 256-byte stack rule, so the owner holds it
// as a member or heap-allocates it. Never a local.
class MapPointQuery {
 public:
  // Rows one category screen can show. Eight is what fits an OptionPopup list
  // without scrolling past what a rider reads at a glance, and it bounds the
  // caller's array -- there is no allocation anywhere in this class.
  static constexpr size_t kMaxHits = 8;
  // A name on a list row, truncated from the format's 63 bytes. A row 480 px
  // wide prints far less than this.
  static constexpr size_t kNameBytes = 32;

  // Never found. Not 0 and not a separate bool: a distance of zero is a real
  // answer (the rider is standing on it), so the sentinel has to be out of
  // range instead.
  static constexpr uint32_t kNoDistance = 0xFFFFFFFFu;

  struct Config {
    // SD root holding points/10/<col>/<row>.tip. Not copied.
    const char* rootDir = nullptr;
    // Where the rider is, 1e7 degrees -- the fix, never the viewport anchor.
    int32_t fixLatE7 = 0;
    int32_t fixLonE7 = 0;
    // Bit N set = consider MapPointKind N. Safety only by default.
    uint8_t kindMask = 1u << static_cast<uint8_t>(MapPointKind::Safety);
    // Metres. The UI prints this number ("None within 25 km"), because a radius
    // the rider cannot reason about is a radius that lies.
    uint32_t radiusM = static_cast<uint32_t>(MapPointShards::kSearchRadiusM);
  };

  // One row of a category screen.
  struct Hit {
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
    uint32_t metres = 0;
    uint8_t category = 0;
    uint8_t flags = 0;
    // 0..7, N NE E SE S SW W NW. Eight sectors and never degrees: the device
    // says roughly that way, which is what a straight-line bearing is worth.
    uint8_t sector = 0;
    char name[kNameBytes] = {};
  };

  explicit MapPointQuery(IFileSource& file);

  // Aims the query: the fix moves, and the kind mask and radius are decisions a
  // caller makes per press. Same shape as MapPointSource::begin().
  void begin(const Config& config) { config_ = config; }
  const Config& config() const { return config_; }

  // Nearest distance in metres per safety category id, kNoDistance where the
  // radius holds none. `out` must have kSafetyCategoryCount entries.
  //
  // False only when the query could not run at all (no root). An empty result is
  // a true with every entry at kNoDistance -- "None within 25 km" is an answer,
  // and a category never disappears from the menu (safety-concept.md).
  bool nearestPerCategory(uint32_t* out, size_t outCount);

  // The nearest `maxHits` points of one category, nearest first. Returns how
  // many were written.
  size_t listCategory(uint8_t category, Hit* out, size_t maxHits);

  // Shards opened and bytes read by the last call, for the debug readout: this
  // is the cost the z10 grid was chosen to bound (3x3 files worst case).
  uint32_t shardsOpened() const { return shardsOpened_; }
  uint32_t shardsMissing() const { return shardsMissing_; }
  uint32_t shardsCorrupt() const { return shardsCorrupt_; }
  uint32_t bytesRead() const { return bytesRead_; }

  // Which of the 8 compass sectors a target sits in, from one point to another.
  // Integer only, no libm: the sector boundaries are comparisons against
  // tan(22.5 deg), which is 0.4142, done in fixed point. Same reasoning as
  // PinGeo -- the ESP32-C3 has no hardware float and a sector does not need one.
  static uint8_t sector8(int32_t fromLatE7, int32_t fromLonE7, int32_t toLatE7, int32_t toLonE7);

  // "N", "NE", ... for a sector. ASCII, not a translated string: these are
  // compass points, and they are the same in every language the device carries.
  static const char* sectorName(uint8_t sector);

 private:
  // Walks every shard in range once. `visit` is called per record that passes
  // the kind mask and the radius, with its metres and lat/lon already computed.
  //
  // `verify` decides whether each shard's body_crc32 is checked before its
  // records are trusted, and that is a measured trade rather than a preference.
  // Checking it costs a second full read of the file: verifyBody() streams the
  // whole body and beginRecords() then re-reads the record array. Measured on
  // the device 2026-08-22, six shards for one menu press:
  // `nearby: 6 shard(s) read, ... 201994 bytes` -- against about 142 kB of
  // actual file.
  //
  // So the distance pass runs without it and the display pass runs with it:
  //
  // - `nearestPerCategory()` produces numbers for the menu's rows. A corrupt
  //   record there can only make one distance wrong, and the per-record checks
  //   nextRecord() already applies (inside the declared bbox, reserved half-word
  //   zero, the name inside the pool) refuse anything grossly broken.
  // - `listCategory()` produces what reaches the screen: names, and the
  //   coordinate `Set destination` writes into the pin log. That is where a
  //   silently wrong record would become a claim the rider acts on, so the crc
  //   is checked there and a failing shard contributes no rows at all.
  //
  // The honesty rule in ../../../docs/point-file-spec.md is unchanged for
  // anything a rider sees; what changed is that a distance in a menu is not
  // worth doubling every read for.
  template <typename Visit>
  bool walk(const Visit& visit, bool verify);

  IFileSource& file_;
  Config config_;
  MapPointReader reader_;

  uint32_t shardsOpened_ = 0;
  uint32_t shardsMissing_ = 0;
  uint32_t shardsCorrupt_ = 0;
  uint32_t bytesRead_ = 0;

  char path_[160] = {};
};
