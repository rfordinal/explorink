#include "WalletStore.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace wallet {

namespace {

constexpr const char* kLogTag = "WALLET";
// The manifest is read in small bites so nothing on this path scales with its
// size. 256 bytes is the same bite HalStorage::readFileToStream() defaults to.
constexpr size_t kFeedChunk = 256;

// Runs a prepared parser over the manifest file. The parser is heap-allocated by
// the caller (it carries StreamingJsonParser's 512-byte token buffer, which has
// no business on the main task's stack).
bool feedManifest(ManifestParser& parser, Error& error) {
  if (!Storage.exists(kManifestPath)) {
    error = Error::NoManifest;
    return false;
  }
  HalFile file;
  if (!Storage.openFileForRead(kLogTag, kManifestPath, file)) {
    error = Error::NoManifest;
    return false;
  }
  char chunk[kFeedChunk];
  for (;;) {
    const int got = file.read(chunk, sizeof(chunk));
    if (got <= 0) break;
    parser.feed(chunk, static_cast<size_t>(got));
    if (parser.hasError()) break;
  }
  file.close();

  if (parser.hasError()) {
    error = Error::ManifestUnreadable;
    return false;
  }
  if (parser.formatVersion() == 0) {
    // Parsed as JSON but carries no formatVersion, so it is not a wallet
    // manifest. Refused rather than shown as an empty wallet.
    error = Error::ManifestUnreadable;
    return false;
  }
  if (parser.formatVersion() != kSupportedFormatVersion) {
    error = Error::ManifestVersion;
    return false;
  }
  error = Error::None;
  return true;
}

}  // namespace

const char* errorText(const Error error) {
  switch (error) {
    case Error::None:
      return "";
    case Error::NoManifest:
      return tr(STR_WALLET_NO_MANIFEST);
    case Error::ManifestUnreadable:
      return tr(STR_WALLET_BAD_MANIFEST);
    case Error::ManifestVersion:
      return tr(STR_WALLET_VERSION);
    case Error::NoItems:
      return tr(STR_WALLET_EMPTY);
    case Error::PanelMismatch:
      // The screen names both geometries itself; this is only the fallback.
      return tr(STR_WALLET_PANEL_OTHER);
    case Error::NoAsset:
    case Error::NotInManifest:
      return tr(STR_WALLET_ASSET_MISSING);
    case Error::BadAsset:
    case Error::ShortRead:
      return tr(STR_WALLET_ASSET_BAD);
    case Error::AssetEncrypted:
      return tr(STR_WALLET_ASSET_ENCRYPTED);
    case Error::AssetWrongSize:
      return tr(STR_WALLET_ASSET_SIZE);
    case Error::NoFrameBuffer:
      return tr(STR_WALLET_ASSET_BUSY);
  }
  return "";
}

PanelGeometry livePanel(const GfxRenderer& renderer) {
  PanelGeometry live;
  live.width = renderer.getDisplayWidth();
  live.height = renderer.getDisplayHeight();
  live.rowBytes = renderer.getDisplayWidthBytes();
  live.bufferBytes = static_cast<uint32_t>(renderer.getBufferSize());
  return live;
}

bool Store::listItems(ItemEntry* out, const uint16_t max, const GfxRenderer& renderer, uint16_t& stored,
                      uint32_t& seen, DeclaredPanel& declared, Error& error) {
  stored = 0;
  seen = 0;
  declared = DeclaredPanel{};
  error = Error::None;

  auto parser = makeUniqueNoThrow<ManifestParser>();
  if (!parser) {
    LOG_ERR(kLogTag, "OOM: manifest parser");
    error = Error::ManifestUnreadable;
    return false;
  }
  parser->beginList(out, max);
  if (!feedManifest(*parser, error)) return false;

  declared = parser->panel();
  // Before anything else: one asset set per wallet, so a set built for another
  // panel cannot be shown at all and must not be half-opened.
  if (!panelMatches(declared, livePanel(renderer))) {
    LOG_ERR(kLogTag, "manifest panel %s %ux%u/%lu, device %ux%u/%u", declared.name,
            static_cast<unsigned>(declared.width), static_cast<unsigned>(declared.height),
            static_cast<unsigned long>(declared.assetBytes), static_cast<unsigned>(renderer.getDisplayWidth()),
            static_cast<unsigned>(renderer.getDisplayHeight()), static_cast<unsigned>(renderer.getBufferSize()));
    error = Error::PanelMismatch;
    return false;
  }

  stored = parser->itemsStored();
  seen = parser->itemsSeen();
  LOG_INF(kLogTag, "manifest v%lu: %lu items (%u listed)", static_cast<unsigned long>(parser->walletVersion()),
          static_cast<unsigned long>(seen), static_cast<unsigned>(stored));
  if (stored == 0) {
    error = Error::NoItems;
    return false;
  }
  return true;
}

bool Store::lookupPage(const int itemIndex, const int pageIndex, const Level level, const uint8_t col,
                       const uint8_t row, PageLookup& out, Error& error) {
  out = PageLookup{};
  error = Error::None;

  auto parser = makeUniqueNoThrow<ManifestParser>();
  if (!parser) {
    LOG_ERR(kLogTag, "OOM: manifest parser");
    error = Error::ManifestUnreadable;
    return false;
  }
  parser->beginLookup(itemIndex, pageIndex, level, col, row);
  if (!feedManifest(*parser, error)) return false;

  out = parser->lookup();
  if (!out.itemFound || !out.pageFound) {
    error = Error::NotInManifest;
    return false;
  }
  if (!out.assetFound) {
    // The page exists and the level does not carry this tile. The viewer clamps
    // its arrows off the grid, so this is a manifest that disagrees with itself.
    LOG_ERR(kLogTag, "no %s tile %u,%u for item %d page %d", levelName(level), static_cast<unsigned>(col),
            static_cast<unsigned>(row), itemIndex, pageIndex);
    error = Error::NotInManifest;
    return false;
  }
  return true;
}

bool Store::loadAssetIntoFrameBuffer(const char* assetId, GfxRenderer& renderer, AssetHeader& header, Error& error) {
  header = AssetHeader{};
  error = Error::None;

  if (!renderer.hasFrameBuffer()) {
    // Someone is holding the framebuffer's bytes for a build phase
    // (GfxRenderer::FrameBufferLoan). Nothing in the wallet lends it, so this is
    // a guard, not a state the viewer can reach on its own.
    error = Error::NoFrameBuffer;
    return false;
  }

  char path[kAssetPathBufBytes];
  if (!buildAssetPath(assetId, path, sizeof(path))) {
    error = Error::BadAsset;
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead(kLogTag, path, file)) {
    LOG_ERR(kLogTag, "no asset file %s", path);
    error = Error::NoAsset;
    return false;
  }

  uint8_t raw[kAssetHeaderBytes];
  const int gotHeader = file.read(raw, sizeof(raw));
  if (gotHeader != static_cast<int>(sizeof(raw))) {
    LOG_ERR(kLogTag, "short header %s", path);
    file.close();
    error = Error::BadAsset;
    return false;
  }

  // One gate, shared with the host preview and the tests, so a disagreement
  // about the header layout or the panel geometry cannot hide on one side
  // (WalletAsset.h, checkAssetForPanel).
  const PanelGeometry live = livePanel(renderer);
  switch (checkAssetForPanel(raw, sizeof(raw), live, header)) {
    case AssetCheck::Ok:
      break;
    case AssetCheck::Malformed:
      LOG_ERR(kLogTag, "bad asset header %s", path);
      file.close();
      error = Error::BadAsset;
      return false;
    case AssetCheck::Encrypted:
      // P1 ships no crypto. Drawing the ciphertext would put noise on the panel
      // and look like a hardware fault, so it is refused with a message instead.
      file.close();
      error = Error::AssetEncrypted;
      return false;
    case AssetCheck::BitDepth:
      // bitDepth 2 is reserved in the format and not implemented here.
      LOG_ERR(kLogTag, "bitDepth %u unsupported", static_cast<unsigned>(header.bitDepth));
      file.close();
      error = Error::BadAsset;
      return false;
    case AssetCheck::WrongPanel:
      LOG_ERR(kLogTag, "asset %ux%u/%lu bytes, panel %ux%u/%lu", static_cast<unsigned>(header.width),
              static_cast<unsigned>(header.height), static_cast<unsigned long>(header.rawLen),
              static_cast<unsigned>(live.width), static_cast<unsigned>(live.height),
              static_cast<unsigned long>(live.bufferBytes));
      file.close();
      error = Error::AssetWrongSize;
      return false;
  }

  const size_t want = live.bufferBytes;

  // The one read that matters. Straight into the framebuffer: the payload is
  // already panel-native (row-major, panelWidthBytes per row, MSB first, bit 1 =
  // white), so there is nothing to transform and nowhere to stage it.
  // GfxRenderer.cpp:517-524 is the bit convention this relies on.
  const int got = file.read(renderer.getFrameBuffer(), want);
  file.close();
  if (got != static_cast<int>(want)) {
    LOG_ERR(kLogTag, "short read %d of %u from %s", got, static_cast<unsigned>(want), path);
    error = Error::ShortRead;
    return false;
  }

  // header.sha256Prefix is parsed and ignored. Verification lands with
  // encryption, where the payload is authenticated anyway -- hashing 48 KB here
  // would cost a read the framebuffer has already consumed.
  return true;
}

}  // namespace wallet
