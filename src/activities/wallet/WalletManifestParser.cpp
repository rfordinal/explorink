#include "WalletManifestParser.h"

#include <cstdlib>
#include <cstring>

namespace wallet {

namespace {

JsonCallbacks makeCallbacks(void* ctx, void (*onKey)(void*, const char*, size_t),
                            void (*onString)(void*, const char*, size_t), void (*onNumber)(void*, const char*, size_t),
                            void (*onBool)(void*, bool), void (*onNull)(void*), void (*onObjectStart)(void*),
                            void (*onObjectEnd)(void*), void (*onArrayStart)(void*), void (*onArrayEnd)(void*)) {
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
void ManifestParser::sOnBool(void* ctx, bool v) { static_cast<ManifestParser*>(ctx)->onBool(v); }
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
  wantedImage = nullptr;
  codeResult = CodeLookup{};
  inWantedCode = false;
  codesInItem = 0;
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

void ManifestParser::beginCodeLookup(int item, int code) {
  reset();
  mode = Mode::Codes;
  out = nullptr;
  outMax = 0;
  wantItem = item;
  wantCode = code;
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
      codesInItem = 0;
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
    } else if (cur == Ctx::LevelObj && (keyIs("pageImage") || keyIs("greyPlanes") || keyIs("greyPageImage"))) {
      // Three objects, identical fields, one context. Which slot they fill is the
      // only difference, and it is decided here while the key is still in hand --
      // push() clears it.
      next = Ctx::PageImage;
      if (mode == Mode::Lookup && levelKnown && itemIndex == wantItem && pageIndex == wantPage &&
          levelInPlay == wantLevel) {
        PageImageSpec* slot = keyIs("pageImage")    ? &result.pageImage
                              : keyIs("greyPlanes") ? &result.greyPlanes
                                                    : &result.greyImage;
        *slot = PageImageSpec{};
        slot->present = true;
        wantedImage = slot;
      }
    } else if (cur == Ctx::AssetsArr) {
      next = Ctx::Asset;
      assetId[0] = '\0';
      assetCol = 0;
      assetRow = 0;
    } else if (cur == Ctx::CodesArr) {
      next = Ctx::Code;
      // codesInItem is the cursor as well as the total: the object opening here is
      // the codesInItem-th code of this item, and the count only advances when the
      // object closes.
      if (mode == Mode::Codes && itemIndex == wantItem && static_cast<int>(codesInItem) == wantCode) {
        codeResult.code = CodeEntry{};
        codeResult.code.present = true;
        inWantedCode = true;
      }
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
    } else if (cur == Ctx::Page && keyIs("codes")) {
      next = Ctx::CodesArr;
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
    case Ctx::PageImage:
      // A page image whose assetId is not 16 hex characters is no page image:
      // the id is what becomes a path, and buildAssetPath refuses it anyway.
      if (wantedImage != nullptr && !isValidAssetId(wantedImage->assetId)) *wantedImage = PageImageSpec{};
      wantedImage = nullptr;
      break;
    case Ctx::Code:
      commitCode();
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
      out[stored].codeCount = codesInItem;
      ++stored;
    }
    return;
  }
  if (mode == Mode::Codes) {
    if (itemIndex == wantItem) {
      codeResult.itemFound = true;
      codeResult.codeCount = codesInItem;
      std::memcpy(codeResult.title, itemTitle, sizeof(itemTitle));
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

// Closes one code object. Counted in every mode -- the browse list wants the
// per-item total as much as the code screen does -- and counted whether or not
// this was the code asked for, which is what makes it a total rather than "codes
// seen up to the index".
void ManifestParser::commitCode() {
  if (inWantedCode) {
    // The id becomes a path. Same 16-hex rule as everywhere else, and a code that
    // fails it is no code at all rather than one that fails to open later.
    if (!isValidAssetId(codeResult.code.assetId)) codeResult.code = CodeEntry{};
    inWantedCode = false;
  }
  if (codesInItem < 0xFFFFu) ++codesInItem;
}

void ManifestParser::onBool(const bool value) {
  // An item's own "grey": the document says whether it wants the grey waveform.
  // Absent means false, which is the right default -- a card written before grey
  // existed asks for none (docs/wallet-grey.md).
  if (top() == Ctx::Item && keyIs("grey")) {
    if (mode == Mode::Lookup && itemIndex == wantItem) result.wantsGrey = value;
    return;
  }
  if (top() != Ctx::Code || !inWantedCode) return;
  // `verified` is the generator's decode round-trip result. Absent means false --
  // see CodeEntry.
  if (keyIs("verified")) codeResult.code.verified = value;
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
    case Ctx::PageImage:
      if (wantedImage != nullptr && keyIs("assetId")) {
        copyText(wantedImage->assetId, sizeof(wantedImage->assetId), value, len);
      }
      break;
    case Ctx::Code:
      // `payload` is read by nobody on purpose (WalletAsset.h, CodeEntry). It is
      // also the one field here that can be longer than the parser's 512-byte
      // token buffer, and an overlong string is dropped without a callback
      // (StreamingJsonParser.cpp:226-249) -- so a 700-character boarding-pass
      // payload costs this parser nothing and breaks nothing.
      if (!inWantedCode) break;
      if (keyIs("id")) copyText(codeResult.code.id, sizeof(codeResult.code.id), value, len);
      if (keyIs("symbology")) copyText(codeResult.code.symbology, sizeof(codeResult.code.symbology), value, len);
      if (keyIs("assetId")) copyText(codeResult.code.assetId, sizeof(codeResult.code.assetId), value, len);
      if (keyIs("sha256")) copyText(codeResult.code.sha256, sizeof(codeResult.code.sha256), value, len);
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
    case Ctx::Code: {
      if (!inWantedCode) break;
      const uint32_t n = toU32(value);
      const uint16_t small = n > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(n);
      if (keyIs("moduleSize")) codeResult.code.moduleSize = small;
      if (keyIs("quietZone")) codeResult.code.quietZone = small;
      if (keyIs("codeWidthPx")) codeResult.code.codeWidthPx = small;
      if (keyIs("codeHeightPx")) codeResult.code.codeHeightPx = small;
      break;
    }
    case Ctx::PageImage: {
      if (wantedImage == nullptr) break;
      const uint32_t n = toU32(value);
      const uint16_t small = n > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(n);
      if (keyIs("nativeWidth")) wantedImage->nativeWidth = small;
      if (keyIs("nativeHeight")) wantedImage->nativeHeight = small;
      if (keyIs("rowBytes")) wantedImage->rowBytes = small;
      if (keyIs("rawLen")) wantedImage->rawLen = n;
      if (keyIs("windowStepX")) wantedImage->windowStepX = small;
      if (keyIs("windowStepY")) wantedImage->windowStepY = small;
      if (keyIs("focalX")) wantedImage->focalX = small;
      if (keyIs("focalY")) wantedImage->focalY = small;
      break;
    }
    default:
      break;
  }
}

}  // namespace wallet
