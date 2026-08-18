#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"
#include "WalletAsset.h"

// Reads /trailink/wallet/manifest.json without ever holding it in RAM.
//
// The manifest is fed through StreamingJsonParser (lib/JsonParser) in small
// chunks, exactly like ReleaseJsonParser reads a GitHub release. Nothing is
// buffered but the answer the caller asked for, so a manifest with a hundred
// pages costs the same RAM as one with one page.
//
// Two questions, one parser:
//
//   * `beginList()`  -- what items are on the card (title, page count). The
//     browse screen's list.
//   * `beginLookup()` -- for one item, one page: the grid of every level and
//     the assetId of one tile. The viewer's whole per-screen need.
//
// The viewer re-runs a lookup on every tile step rather than caching a table of
// assetIds. A manifest read is a few KB off the card; a FAST refresh is 500 ms
// (../../../docs/refresh-modes.md). Re-reading costs nothing measurable and it
// caps the viewer's resident RAM at a few hundred bytes with no ceiling on how
// many tiles a level may have.
//
// Pure: no Storage, no renderer, no globals. The Storage glue lives in
// WalletStore, and test/wallet drives this class off in-memory strings.
namespace wallet {

// A title long enough for "Vehicle registration certificate" and no longer.
inline constexpr size_t kTitleBufBytes = 48;

struct ItemEntry {
  char title[kTitleBufBytes] = {0};
  uint16_t pageCount = 0;
};

struct LevelGrid {
  uint8_t cols = 0;
  uint8_t rows = 0;
  // The generator's focal tile for this level ("defaultTileX"/"defaultTileY"):
  // the centre biased top-left, so a 4x4 grid points at 1,1 where a page's text
  // starts rather than at the top-left margin. A hint, not a constraint -- the
  // viewer opens a level here and the arrows go wherever they like afterwards.
  uint8_t defaultCol = 0;
  uint8_t defaultRow = 0;
};

struct PageLookup {
  bool itemFound = false;
  bool pageFound = false;
  bool assetFound = false;
  char assetId[kAssetIdBufBytes] = {0};
  // The requested level's whole-page image, when it has one. Filled for the
  // requested level only -- the viewer re-runs a lookup on a level change, so
  // carrying the other two would be dead weight (design B).
  PageImageSpec pageImage;
  LevelGrid grid[kLevelCount] = {};
  uint16_t pageCount = 0;
  char title[kTitleBufBytes] = {0};
};

class ManifestParser {
 public:
  ManifestParser();

  ManifestParser(const ManifestParser&) = delete;
  ManifestParser& operator=(const ManifestParser&) = delete;

  // List mode: fill up to `max` entries. `itemsSeen()` still counts every item
  // in the manifest, so a truncated list can say so out loud.
  void beginList(ItemEntry* out, uint16_t max);
  // Lookup mode: one item, one page, one tile. Grids for all three levels come
  // along in the same pass because they are six bytes and the viewer needs them
  // to clamp its arrows.
  void beginLookup(int itemIndex, int pageIndex, Level level, uint8_t col, uint8_t row);

  void feed(const char* data, size_t len);

  bool hasError() const { return parser.hasError() || overflowed; }
  uint32_t formatVersion() const { return formatVersion_; }
  uint32_t walletVersion() const { return walletVersion_; }
  // What the manifest says it was built for. `present` false means the manifest
  // predates the field (WalletAsset.h, DeclaredPanel).
  const DeclaredPanel& panel() const { return panel_; }

  uint16_t itemsStored() const { return stored; }
  uint32_t itemsSeen() const { return seen; }
  const PageLookup& lookup() const { return result; }

 private:
  enum class Mode : uint8_t { List, Lookup };

  // Where in the document we are. One entry per open object/array, so the
  // meaning of a key never depends on guessing a depth number.
  enum class Ctx : uint8_t {
    Other,
    Root,
    Panel,
    ItemsArr,
    Item,
    PagesArr,
    Page,
    Levels,
    LevelObj,
    AssetsArr,
    Asset,
    PageImage
  };

  static constexpr uint8_t kMaxDepth = StreamingJsonParser::MAX_NESTING;
  static constexpr size_t kKeyBufBytes = 24;

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void reset();
  void onKey(const char* key, size_t len);
  void onString(const char* value, size_t len);
  void onNumber(const char* value, size_t len);
  void onContainerStart(bool isObject);
  void onContainerEnd();

  Ctx top() const { return depth > 0 ? stack[depth - 1] : Ctx::Other; }
  void push(Ctx ctx);
  bool keyIs(const char* name) const;
  void commitItem();
  void commitAsset();
  // Copies a UTF-8 string into a fixed buffer, cutting on a codepoint boundary
  // rather than mid-sequence -- half a glyph draws as a replacement box.
  static void copyText(char* dst, size_t dstLen, const char* src, size_t srcLen);

  StreamingJsonParser parser;
  Mode mode = Mode::List;

  Ctx stack[kMaxDepth] = {};
  uint8_t depth = 0;
  // Containers we could not push because the document nests deeper than the
  // parser allows. Counted so the pops stay balanced instead of unwinding the
  // real stack.
  uint16_t extraDepth = 0;
  bool overflowed = false;

  char key[kKeyBufBytes] = {0};

  uint32_t formatVersion_ = 0;
  uint32_t walletVersion_ = 0;
  DeclaredPanel panel_;

  // List mode
  ItemEntry* out = nullptr;
  uint16_t outMax = 0;
  uint16_t stored = 0;
  uint32_t seen = 0;

  // Lookup mode
  int wantItem = -1;
  int wantPage = -1;
  Level wantLevel = Level::Fit;
  uint8_t wantCol = 0;
  uint8_t wantRow = 0;
  PageLookup result;

  // Cursor through the document
  int itemIndex = -1;
  int pageIndex = -1;
  uint16_t pagesInItem = 0;
  char itemTitle[kTitleBufBytes] = {0};
  Level levelInPlay = Level::Fit;
  bool levelKnown = false;

  // The asset object currently being read
  char assetId[kAssetIdBufBytes] = {0};
  uint16_t assetCol = 0;
  uint16_t assetRow = 0;
  // True only while inside the pageImage object of the level being looked up.
  bool inWantedPageImage = false;
};

}  // namespace wallet
