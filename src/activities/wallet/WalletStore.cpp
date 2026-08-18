#include "WalletStore.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "WalletCryptoDevice.h"

namespace wallet {

namespace {

constexpr const char* kLogTag = "WALLET";
// The manifest is read in small bites so nothing on this path scales with its
// size. 256 bytes is the same bite HalStorage::readFileToStream() defaults to.
constexpr size_t kFeedChunk = 256;

// Reads an encrypted manifest, verifies its tag and feeds the plaintext to the
// parser.
//
// **The whole file has to be in RAM before one byte of it is parsed.** GCM only
// authenticates once it has seen everything, and parsing unauthenticated JSON is
// the mistake this design exists to avoid -- so there is no streaming path here and
// there must not be one. The size is capped rather than trusted, and the buffer is
// one allocation used twice: the ciphertext goes in, the plaintext comes out in
// place (mbedtls allows output == input, gcm.c's guard refuses only a partial
// overlap).
bool feedEncryptedManifest(ManifestParser& parser, Error& error) {
  HalFile file;
  if (!Storage.openFileForRead(kLogTag, kManifestEncPath, file)) {
    error = Error::NoManifest;
    return false;
  }
  const size_t fileSize = static_cast<size_t>(file.size());
  if (fileSize > kMaxEncryptedManifestBytes) {
    LOG_ERR(kLogTag, "encrypted manifest is %u bytes, cap is %u", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(kMaxEncryptedManifestBytes));
    file.close();
    error = Error::ManifestTooBig;
    return false;
  }
  if (fileSize < kManifestEnvelopeLenV1 + kGcmTagLen) {
    file.close();
    error = Error::ManifestAuth;
    return false;
  }

  const uint8_t* key = Session::instance().key();
  if (key == nullptr) {
    file.close();
    error = Error::Locked;
    return false;
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(fileSize);
  if (!buffer) {
    // The cap is what turns this from an OOM into a message, but the allocation can
    // still fail below the cap when BLE holds the big blocks.
    LOG_ERR(kLogTag, "OOM: %u bytes for the encrypted manifest", static_cast<unsigned>(fileSize));
    file.close();
    error = Error::ManifestTooBig;
    return false;
  }
  const int got = file.read(buffer.get(), fileSize);
  file.close();
  if (got != static_cast<int>(fileSize)) {
    LOG_ERR(kLogTag, "short read %d of %u from the encrypted manifest", got, static_cast<unsigned>(fileSize));
    error = Error::ManifestAuth;
    return false;
  }

  ManifestEnvelope envelope;
  if (!parseManifestEnvelope(buffer.get(), fileSize, envelope)) {
    LOG_ERR(kLogTag, "not an EWM1 manifest, or its lengths disagree");
    error = Error::ManifestAuth;
    return false;
  }
  uint8_t* cipher = buffer.get() + envelope.ciphertextOffset;
  if (!gcmDecryptInPlace(key, envelope.nonce, kGcmNonceLen, buffer.get() + envelope.tagOffset, kGcmTagLen, cipher,
                         envelope.ciphertextLen)) {
    // Wrong key or an altered file, and GCM cannot tell those apart. Nothing has
    // been parsed and nothing will be.
    error = Error::ManifestAuth;
    return false;
  }

  // Only now, with the tag verified, does the JSON exist as far as this code is
  // concerned. Fed in the same 256-byte bites as the cleartext path so the parser
  // sees no difference at all.
  for (size_t at = 0; at < envelope.ciphertextLen && !parser.hasError(); at += kFeedChunk) {
    const size_t take = (at + kFeedChunk <= envelope.ciphertextLen) ? kFeedChunk : envelope.ciphertextLen - at;
    parser.feed(reinterpret_cast<const char*>(cipher + at), take);
  }
  // The plaintext held every title in the wallet; it does not get to sit in a freed
  // heap block afterwards.
  secureWipe(buffer.get(), fileSize);
  return true;
}

// Runs a prepared parser over the manifest file. The parser is heap-allocated by
// the caller (it carries StreamingJsonParser's 512-byte token buffer, which has
// no business on the main task's stack).
bool feedManifest(ManifestParser& parser, Error& error) {
  if (treeIsEncrypted()) {
    if (!feedEncryptedManifest(parser, error)) return false;
    // Fall through to the same version and error checks the cleartext path runs:
    // an encrypted manifest is still a manifest.
    if (parser.hasError()) {
      error = Error::ManifestUnreadable;
      return false;
    }
    if (parser.formatVersion() == 0) {
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
    case Error::Locked:
      return tr(STR_WALLET_LOCKED);
    case Error::ManifestTooBig:
      return tr(STR_WALLET_TOO_BIG);
    case Error::ManifestAuth:
      return tr(STR_WALLET_BAD_KEY);
    case Error::AssetDecrypt:
      return tr(STR_WALLET_ASSET_DECRYPT);
    case Error::NoCodes:
      return tr(STR_WALLET_CODE_NONE);
    case Error::CodeNotACode:
      return tr(STR_WALLET_CODE_BAD);
    case Error::CodeHashMismatch:
      return tr(STR_WALLET_CODE_BAD_HASH);
    case Error::CodeUnmarked:
      return tr(STR_WALLET_CODE_UNMARKED);
  }
  return "";
}

bool treeIsEncrypted() { return Storage.exists(kManifestEncPath); }

bool readCardWalletVersion(uint32_t& out) {
  out = 0;
  if (!Storage.exists(kManifestEncPath)) return false;
  HalFile file;
  if (!Storage.openFileForRead(kLogTag, kManifestEncPath, file)) return false;
  // Only the envelope, and only its cleartext part: 26 bytes off the front, no
  // key involved. This is what lets a locked device answer "which version do I
  // hold" so the phone can compute pending work without a PIN (brief 23-26, 54).
  uint8_t head[kManifestEnvelopeLen] = {0};
  const int got = file.read(head, sizeof(head));
  if (got != static_cast<int>(sizeof(head))) return false;
  if (std::memcmp(head, kManifestMagic, sizeof(kManifestMagic)) != 0) return false;
  if (head[4] != kManifestEncVersion) return false;  // v1 carries no version
  out = readLe32(head + 22);
  return true;
}

PanelGeometry livePanel(const GfxRenderer& renderer) {
  PanelGeometry live;
  live.width = renderer.getDisplayWidth();
  live.height = renderer.getDisplayHeight();
  live.rowBytes = renderer.getDisplayWidthBytes();
  live.bufferBytes = static_cast<uint32_t>(renderer.getBufferSize());
  return live;
}

bool Store::listItems(ItemEntry* out, const uint16_t max, const GfxRenderer& renderer, uint16_t& stored, uint32_t& seen,
                      DeclaredPanel& declared, Error& error) {
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
  // Either source will do. A design-B level carries a page image and may carry no
  // tiles at all; a card generated before design B carries only tiles.
  if (!out.assetFound && !out.pageImage.present) {
    LOG_ERR(kLogTag, "no %s source for item %d page %d (tile %u,%u)", levelName(level), itemIndex, pageIndex,
            static_cast<unsigned>(col), static_cast<unsigned>(row));
    error = Error::NotInManifest;
    return false;
  }
  return true;
}

bool Store::lookupCode(const int itemIndex, const int codeIndex, CodeLookup& out, Error& error) {
  out = CodeLookup{};
  error = Error::None;

  auto parser = makeUniqueNoThrow<ManifestParser>();
  if (!parser) {
    LOG_ERR(kLogTag, "OOM: manifest parser");
    error = Error::ManifestUnreadable;
    return false;
  }
  parser->beginCodeLookup(itemIndex, codeIndex);
  if (!feedManifest(*parser, error)) return false;

  out = parser->codes();
  if (!out.itemFound) {
    error = Error::NotInManifest;
    return false;
  }
  if (out.codeCount == 0) {
    // Not a fault: most documents have no code. The browse screen reads the count
    // and leaves LEFT/RIGHT idle.
    error = Error::NoCodes;
    return false;
  }
  if (!out.code.present) {
    LOG_ERR(kLogTag, "item %d has %u codes, none at index %d", itemIndex, static_cast<unsigned>(out.codeCount),
            codeIndex);
    error = Error::NotInManifest;
    return false;
  }
  LOG_INF(kLogTag, "item %d code %d/%u: %s %s %s", itemIndex, codeIndex, static_cast<unsigned>(out.codeCount),
          out.code.id, out.code.symbology, out.code.verified ? "verified" : "UNVERIFIED");
  return true;
}

bool PageReader::open(const PageImageSpec& page, const GfxRenderer& renderer, Error& error) {
  error = Error::None;
  if (!page.present || !isValidAssetId(page.assetId)) {
    error = Error::NotInManifest;
    return false;
  }
  // Already the right file: a pan must not pay for an open.
  if (open_ && std::strcmp(spec_.assetId, page.assetId) == 0) {
    spec_ = page;
    return true;
  }
  close();

  char path[kAssetPathBufBytes];
  if (!buildAssetPath(page.assetId, path, sizeof(path))) {
    error = Error::BadAsset;
    return false;
  }
  if (!Storage.openFileForRead(kLogTag, path, file_)) {
    LOG_ERR(kLogTag, "no page image %s", path);
    error = Error::NoAsset;
    return false;
  }

  uint8_t raw[kAssetHeaderBytes];
  if (file_.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
    LOG_ERR(kLogTag, "short page-image header %s", path);
    file_.close();
    error = Error::BadAsset;
    return false;
  }

  const PanelGeometry live = livePanel(renderer);
  const bool haveKey = Session::instance().key() != nullptr;
  switch (checkPageImage(raw, sizeof(raw), page, live, header_, haveKey)) {
    case AssetCheck::Ok:
      break;
    case AssetCheck::Encrypted:
      file_.close();
      error = Error::AssetEncrypted;
      return false;
    case AssetCheck::WrongPanel:
      // The page image is narrower or shorter than this panel, so no window can
      // fill the screen. A wrong-device problem, not a corrupt file.
      LOG_ERR(kLogTag, "page image %ux%u/%u B row is smaller than the %ux%u/%u panel",
              static_cast<unsigned>(page.nativeWidth), static_cast<unsigned>(page.nativeHeight),
              static_cast<unsigned>(page.rowBytes), static_cast<unsigned>(live.width),
              static_cast<unsigned>(live.height), static_cast<unsigned>(live.rowBytes));
      file_.close();
      error = Error::AssetWrongSize;
      return false;
    case AssetCheck::Malformed:
    case AssetCheck::BitDepth:
    case AssetCheck::NotACode:
    case AssetCheck::PageImageMismatch:
      LOG_ERR(kLogTag, "page image %s does not match the manifest", path);
      file_.close();
      error = Error::BadAsset;
      return false;
  }

  encrypted_ = (header_.flags & kFlagEncrypted) != 0;
  if (encrypted_ && !buildAssetIv(page.assetId, header_.version, iv_)) {
    LOG_ERR(kLogTag, "page image %s has no usable IV", page.assetId);
    file_.close();
    error = Error::BadAsset;
    return false;
  }
  spec_ = page;
  open_ = true;
  LOG_INF(kLogTag, "page image open: %s %ux%u, %u B/row, step %u,%u", page.assetId,
          static_cast<unsigned>(page.nativeWidth), static_cast<unsigned>(page.nativeHeight),
          static_cast<unsigned>(page.rowBytes), static_cast<unsigned>(page.windowStepX),
          static_cast<unsigned>(page.windowStepY));
  return true;
}

void PageReader::close() {
  if (open_ || file_.isOpen()) file_.close();
  open_ = false;
  spec_ = PageImageSpec{};
  header_ = AssetHeader{};
  encrypted_ = false;
  secureWipe(iv_, sizeof(iv_));
}

bool PageReader::readWindow(const uint32_t x, const uint32_t y, GfxRenderer& renderer, Error& error) {
  error = Error::None;
  if (!open_) {
    error = Error::NotInManifest;
    return false;
  }
  if (!renderer.hasFrameBuffer()) {
    error = Error::NoFrameBuffer;
    return false;
  }
  if ((x % 8) != 0) {
    // The caller clamps against maxWindowX(), which is a byte count times 8 and
    // so cannot be unaligned. Reaching here means a caller invented an origin.
    LOG_ERR(kLogTag, "window x=%lu is not 8-aligned", static_cast<unsigned long>(x));
    error = Error::BadAsset;
    return false;
  }

  const uint32_t rowBytes = renderer.getDisplayWidthBytes();
  const uint32_t rows = renderer.getDisplayHeight();
  const uint32_t xByte = x / 8;
  if (xByte + rowBytes > spec_.rowBytes || y + rows > spec_.nativeHeight) {
    LOG_ERR(kLogTag, "window %lu,%lu does not fit %ux%u", static_cast<unsigned long>(x), static_cast<unsigned long>(y),
            static_cast<unsigned>(spec_.nativeWidth), static_cast<unsigned>(spec_.nativeHeight));
    error = Error::AssetWrongSize;
    return false;
  }

  const uint8_t* const key = encrypted_ ? Session::instance().key() : nullptr;
  if (encrypted_ && key == nullptr) {
    error = Error::Locked;
    return false;
  }

  uint8_t* const fb = renderer.getFrameBuffer();
  for (uint32_t r = 0; r < rows; ++r) {
    // Two offsets, and they are not the same number: the file offset includes the
    // 32-byte cleartext header, the CTR offset does not. The keystream is indexed
    // from the first byte of the PAYLOAD.
    const uint32_t payloadOffset = (y + r) * spec_.rowBytes + xByte;
    const uint32_t offset = kAssetHeaderBytes + payloadOffset;
    if (!file_.seekSet(offset) || file_.read(fb + r * rowBytes, rowBytes) != static_cast<int>(rowBytes)) {
      LOG_ERR(kLogTag, "page-image row %lu short at offset %lu", static_cast<unsigned long>(r),
              static_cast<unsigned long>(offset));
      error = Error::ShortRead;
      return false;
    }
    if (encrypted_ && !ctrDecryptInPlace(key, iv_, payloadOffset, fb + r * rowBytes, rowBytes)) {
      error = Error::AssetDecrypt;
      return false;
    }
  }
  // This is the whole reason the format uses CTR. Every one of those 480 rows starts
  // at an arbitrary, generally unaligned payload offset, and CTR reaches it by
  // starting the counter at offset / 16 and dropping offset % 16 bytes of keystream.
  // Anything chained would have forced a decrypt from byte zero per row -- 480 times
  // up to 585 KB (docs/wallet-crypto.md, "Why CTR").
  //
  // No plaintext hash check here: a window is a fraction of the payload, so there is
  // nothing to check it against. A wrong key cannot get this far anyway -- the
  // manifest that named this asset would not have authenticated.
  // header_.sha256Prefix is still unchecked here, same as the tile path -- and a
  // window is a fraction of the payload, so a whole-file hash would cost more than
  // the read it is guarding.
  return true;
}

namespace {

// Which gate a whole-screen read passes through. Same read either way; a code
// must additionally *say* it is a code (WalletAsset.h, checkCodeAsset).
enum class Gate : uint8_t { Tile, Code };

bool readWholeScreen(const char* assetId, const Gate gate, GfxRenderer& renderer, AssetHeader& header, Error& error) {
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
  // A key in hand is what lets an encrypted asset through the gate at all; without
  // one it is refused rather than drawn as noise.
  const bool haveKey = Session::instance().key() != nullptr;
  const AssetCheck check = gate == Gate::Code ? checkCodeAsset(raw, sizeof(raw), live, header, haveKey)
                                              : checkAssetForPanel(raw, sizeof(raw), live, header, haveKey);
  switch (check) {
    case AssetCheck::Ok:
      break;
    case AssetCheck::NotACode:
      // The manifest's codes array pointed at a document tile. Drawing page three
      // of an insurance policy where a boarding pass belongs is worse than
      // drawing nothing.
      LOG_ERR(kLogTag, "%s is assetType %u, not a machine code", path, static_cast<unsigned>(header.assetType));
      file.close();
      error = Error::CodeNotACode;
      return false;
    case AssetCheck::PageImageMismatch:
      // Only checkPageImage() produces this, and this is not that gate.
      LOG_ERR(kLogTag, "unexpected page-image verdict for %s", path);
      file.close();
      error = Error::BadAsset;
      return false;
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

  if ((header.flags & kFlagEncrypted) != 0) {
    // Decrypted where it lies. The read already put the ciphertext in the
    // framebuffer and a second 48 KB buffer would not fit
    // (../../../docs/map-memory.md:57), so CTR runs over those bytes in place --
    // one screen, offset 0, one pass.
    uint8_t iv[kAssetIvLen];
    const uint8_t* key = Session::instance().key();
    if (key == nullptr || !buildAssetIv(assetId, header.version, iv)) {
      error = Error::Locked;
      return false;
    }
    if (!ctrDecryptInPlace(key, iv, 0, renderer.getFrameBuffer(), want)) {
      error = Error::AssetDecrypt;
      return false;
    }
    // The header's sha256 prefix covers the PLAINTEXT, so it is checked here and
    // nowhere earlier -- and on an encrypted asset it earns its keep twice: it
    // catches a corrupt payload as before, and it catches a wrong key, which would
    // otherwise put 48 KB of noise on the panel looking like a hardware fault. It
    // is not a MAC: an attacker who can rewrite the card rewrites this prefix with
    // it. Only the manifest is authenticated.
    const HashResult hash = checkPayloadHash(renderer.getFrameBuffer(), want, nullptr, header.sha256Prefix);
    if (!hash.ok) {
      LOG_ERR(kLogTag, "%s decrypted to something that is not its plaintext (wrong key, or corrupt)", path);
      error = Error::AssetDecrypt;
      return false;
    }
  }
  return true;
}

}  // namespace

bool Store::loadAssetIntoFrameBuffer(const char* assetId, GfxRenderer& renderer, AssetHeader& header, Error& error) {
  // A document tile's sha256 is parsed and ignored, deliberately: a wrong pixel on
  // a passport scan is cosmetic, and a page image is read a window at a time, so a
  // whole-file hash would cost more than the read it guards. The code path below
  // is the exception, and the only one (docs/wallet-viewer.md).
  return readWholeScreen(assetId, Gate::Tile, renderer, header, error);
}

bool Store::loadCodeIntoFrameBuffer(const CodeEntry& code, GfxRenderer& renderer, AssetHeader& header, Error& error) {
  if (!readWholeScreen(code.assetId, Gate::Code, renderer, header, error)) return false;

  // The payload IS the framebuffer, so the bytes to hash are already in RAM and
  // the second read the obvious design would need does not exist. The caller holds
  // a RenderLock across this, so nothing else can write those bytes in between.
  const HashResult hash = checkPayloadHash(renderer.getFrameBuffer(), header.rawLen, code.sha256, header.sha256Prefix);
  if (!hash.ok) {
    LOG_ERR(kLogTag, "code %s %s: sha256 mismatch against the %s", code.id, code.assetId,
            hash.authority == HashAuthority::Full ? "manifest" : "asset header");
    error = Error::CodeHashMismatch;
    return false;
  }
  LOG_INF(kLogTag, "code %s hash ok (%s)", code.id, hash.authority == HashAuthority::Full ? "manifest" : "prefix");
  return true;
}

}  // namespace wallet
