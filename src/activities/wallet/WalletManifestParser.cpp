#include "WalletManifestParser.h"

#include <cstdlib>
#include <cstring>

namespace wallet {

namespace {

JsonCallbacks makeCallbacks(void* ctx, void (*onKey)(void*, const char*, size_t),
                            void (*onString)(void*, const char*, size_t),
                            void (*onNumber)(void*, const char*, size_t), void (*onBool)(void*, bool),
                            void (*onNull)(void*), void (*onObjectStart)(void*), void (*onObjectEnd)(void*),
                            void (*onArrayStart)(void*), void (*onArrayEnd)(void*)) {
  JsonCallbacks cb{};
  cb.ctx = ctx;
  cb.onKey = onKey;
  cb.onString = onString;
  cb.onNumber = onNumber;
  cb.onBool = onBool;
  cb.onNull = onNull;
  cb.onObjectStart = onObjectStart;
  cb.onObjectEnd = onObjectEnd;
  cb.onArrayStart = onArrayStart;
  cb.onArrayEnd = onArrayEnd;
  return cb;
}

uint32_t toU32(const char* text) {
  if (text == nullptr) return 0;
  const long value = std::strtol(text, nullptr, 10);
  return value < 0 ? 0u : static_cast<uint32_t>(value);
}

}  // namespace

void ManifestParser::sOnKey(void* ctx, const char* k, size_t len) { static_cast<ManifestParser*>(ctx)->onKey(k, len); }
void ManifestParser::sOnString(void* ctx, const char* v, size_t len) {
  static_cast<ManifestParser*>(ctx)->onString(v, len);
}
void ManifestParser::sOnNumber(void* ctx, const char* v, size_t len) {
  static_cast<ManifestParser*>(ctx)->onNumber(v, len);
}
void ManifestParser::sOnBool(void*, bool) {}
void ManifestParser::sOnNull(void*) {}
void ManifestParser::sOnObjectStart(void* ctx) { static_cast<ManifestParser*>(ctx)->onContainerStart(true); }
void ManifestParser::sOnObjectEnd(void* ctx) { static_cast<ManifestParser*>(ctx)->onContainerEnd(); }
void ManifestParser::sOnArrayStart(void* ctx) { static_cast<ManifestParser*>(ctx)->onContainerStart(false); }
void ManifestParser::sOnArrayEnd(void* ctx) { static_cast<ManifestParser*>(ctx)->onContainerEnd(); }

ManifestParser::ManifestParser()
    : parser(makeCallbacks(this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd)) {}

void ManifestParser::reset() {
  parser.reset();
  depth = 0;
  extraDepth = 0;
  overflowed = false;
  key[0] = '\0';
  formatVersion_ = 0;
  walletVersion_ = 0;
  panel_ = DeclaredPanel{};
  stored = 0;
  seen = 0;
  result = PageLookup{};
  itemIndex = -1;
  pageIndex = -1;
  pagesInItem = 0;
  itemTitle[0] = '\0';
  levelKnown = false;
  assetId[0] = '\0';
  assetCol = 0;
  assetRow = 0;
}

void ManifestParser::beginList(ItemEntry* dst, uint16_t max) {
  reset();
  mode = Mode::List;
  out = dst;
  outMax = dst != nullptr ? max : 0;
}

void ManifestParser::beginLookup(int item, int page, Level level, uint8_t col, uint8_t row) {
  reset();
  mode = Mode::Lookup;
  out = nullptr;
  outMax = 0;
  wantItem = item;
  wantPage = page;
  wantLevel = level;
  wantCol = col;
  wantRow = row;
}

void ManifestParser::feed(const char* data, size_t len) { parser.feed(data, len); }

void ManifestParser::push(Ctx ctx) {
  if (depth < kMaxDepth) {
    stack[depth++] = ctx;
    return;
  }
  // Deeper than the parser itself allows. It will flag its own error; keep the
  // pop count honest so nothing unwinds into the wrong context.
  ++extraDepth;
  overflowed = true;
}

bool ManifestParser::keyIs(const char* name) const { return std::strcmp(key, name) == 0; }

void ManifestParser::copyText(char* dst, size_t dstLen, const char* src, size_t srcLen) {
  if (dstLen == 0) return;
  size_t take = srcLen < dstLen - 1 ? srcLen : dstLen - 1;
  // Back off a truncation that landed inside a UTF-8 sequence.
  while (take > 0 && (static_cast<unsigned char>(src[take]) & 0xC0) == 0x80) --take;
  std::memcpy(dst, src, take);
  dst[take] = '\0';
}

void ManifestParser::onKey(const char* k, size_t len) { copyText(key, sizeof(key), k, len); }

void ManifestParser::onContainerStart(bool isObject) {
  const Ctx cur = top();
  Ctx next = Ctx::Other;

  if (isObject) {
    if (depth == 0) {
      next = Ctx::Root;
    } else if (cur == Ctx::ItemsArr) {
      next = Ctx::Item;
      ++itemIndex;
      pageIndex = -1;
      pagesInItem = 0;
      itemTitle[0] = '\0';
    } else if (cur == Ctx::PagesArr) {
      next = Ctx::Page;
      ++pageIndex;
    } else if (cur == Ctx::Root && keyIs("panel")) {
      next = Ctx::Panel;
      panel_.present = true;
    } else if (cur == Ctx::Page && keyIs("levels")) {
      next = Ctx::Levels;
    } else if (cur == Ctx::Levels) {
      Level level = Level::Fit;
      if (levelFromKey(key, level)) {
        next = Ctx::LevelObj;
        levelInPlay = level;
        levelKnown = true;
      } else {
        // A level this firmware does not know. Skipped, not an error: the
        // manifest may carry levels a later phase adds.
        levelKnown = false;
      }
    } else if (cur == Ctx::AssetsArr) {
      next = Ctx::Asset;
      assetId[0] = '\0';
      assetCol = 0;
      assetRow = 0;
    }
  } else {
    if (cur == Ctx::Root && keyIs("items")) {
      next = Ctx::ItemsArr;
      itemIndex = -1;
    } else if (cur == Ctx::Item && keyIs("pages")) {
      next = Ctx::PagesArr;
      pageIndex = -1;
    } else if (cur == Ctx::LevelObj && keyIs("assets")) {
      next = Ctx::AssetsArr;
    }
  }

  push(next);
  key[0] = '\0';
}

void ManifestParser::onContainerEnd() {
  if (extraDepth > 0) {
    --extraDepth;
    return;
  }
  if (depth == 0) return;

  const Ctx closing = stack[--depth];
  switch (closing) {
    case Ctx::PagesArr:
      pagesInItem = pageIndex >= 0 ? static_cast<uint16_t>(pageIndex + 1) : 0;
      break;
    case Ctx::Item:
      commitItem();
      break;
    case Ctx::Asset:
      commitAsset();
      break;
    default:
      break;
  }
  key[0] = '\0';
}

void ManifestParser::commitItem() {
  ++seen;
  if (mode == Mode::List) {
    if (out != nullptr && stored < outMax) {
      std::memcpy(out[stored].title, itemTitle, sizeof(itemTitle));
      out[stored].pageCount = pagesInItem;
      ++stored;
    }
    return;
  }
  if (itemIndex == wantItem) {
    result.itemFound = true;
    result.pageCount = pagesInItem;
    std::memcpy(result.title, itemTitle, sizeof(itemTitle));
    if (wantPage >= 0 && wantPage < static_cast<int>(pagesInItem)) result.pageFound = true;
  }
}

void ManifestParser::commitAsset() {
  if (mode != Mode::Lookup || !levelKnown) return;
  if (itemIndex != wantItem || pageIndex != wantPage) return;
  if (levelInPlay != wantLevel) return;
  if (assetCol != wantCol || assetRow != wantRow) return;
  if (!isValidAssetId(assetId)) return;
  std::memcpy(result.assetId, assetId, sizeof(result.assetId));
  result.assetFound = true;
}

void ManifestParser::onString(const char* value, size_t len) {
  switch (top()) {
    case Ctx::Item:
      if (keyIs("title")) copyText(itemTitle, sizeof(itemTitle), value, len);
      break;
    case Ctx::Panel:
      if (keyIs("name")) copyText(panel_.name, sizeof(panel_.name), value, len);
      break;
    case Ctx::Asset:
      if (keyIs("assetId")) copyText(assetId, sizeof(assetId), value, len);
      break;
    default:
      break;
  }
}

void ManifestParser::onNumber(const char* value, size_t len) {
  (void)len;
  switch (top()) {
    case Ctx::Root:
      if (keyIs("formatVersion")) formatVersion_ = toU32(value);
      if (keyIs("walletVersion")) walletVersion_ = toU32(value);
      break;
    case Ctx::Panel: {
      const uint32_t n = toU32(value);
      if (keyIs("width")) panel_.width = static_cast<uint16_t>(n);
      if (keyIs("height")) panel_.height = static_cast<uint16_t>(n);
      if (keyIs("rowBytes")) panel_.rowBytes = static_cast<uint16_t>(n);
      if (keyIs("assetBytes")) panel_.assetBytes = n;
      break;
    }
    case Ctx::LevelObj:
      // Grids are read for the requested page only; every other page's grid is
      // noise for this pass.
      if (mode == Mode::Lookup && levelKnown && itemIndex == wantItem && pageIndex == wantPage) {
        const uint8_t slot = static_cast<uint8_t>(levelInPlay);
        if (slot < kLevelCount) {
          const uint32_t n = toU32(value);
          const uint8_t clamped = n > 255 ? 255 : static_cast<uint8_t>(n);
          if (keyIs("cols")) result.grid[slot].cols = clamped;
          if (keyIs("rows")) result.grid[slot].rows = clamped;
          if (keyIs("defaultTileX")) result.grid[slot].defaultCol = clamped;
          if (keyIs("defaultTileY")) result.grid[slot].defaultRow = clamped;
        }
      }
      break;
    case Ctx::Asset:
      if (keyIs("col")) assetCol = static_cast<uint16_t>(toU32(value));
      if (keyIs("row")) assetRow = static_cast<uint16_t>(toU32(value));
      break;
    default:
      break;
  }
}

}  // namespace wallet
