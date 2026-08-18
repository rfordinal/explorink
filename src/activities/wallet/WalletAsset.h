#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// One wallet asset = one whole screen, already in panel-native byte order.
//
// The laptop-side generator rotates, scales, dithers and packs; the device
// rotates nothing. So the whole display path for a screen is: open, seek past
// this 32-byte header, read `rawLen` bytes straight into the framebuffer,
// refresh. See ../../../docs/wallet-viewer.md.
//
// Header-only on purpose: the header parse and the assetId -> path mapping are
// pure byte work with no Storage, no renderer and no globals, so the host test
// (test/wallet) links them without dragging the firmware in -- same reason
// MapTilePath.h is header-only.

namespace wallet {

// Bytes on the card before the payload starts.
inline constexpr size_t kAssetHeaderBytes = 32;

// "EWA1" -- ExplorInk Wallet Asset, format 1.
inline constexpr uint8_t kAssetMagic[4] = {'E', 'W', 'A', '1'};

// An assetId is 16 lowercase hex characters; the file lives in a directory
// named by its first two.
inline constexpr size_t kAssetIdLen = 16;
inline constexpr size_t kAssetIdBufBytes = kAssetIdLen + 1;

inline constexpr const char* kWalletDir = "/trailink/wallet";
// "/trailink/wallet/" + 2 + "/" + 16 + ".dat" + NUL
inline constexpr size_t kAssetPathBufBytes = 64;

enum class AssetType : uint8_t {
  Unknown = 0,
  Fit = 1,
  DetailTile = 2,
  OneToOneTile = 3,
  MachineCode = 4,
  // Design B: one whole-page image per level, panel-native order, and the device
  // blits an arbitrary window out of it. The pan step is a fraction of the view
  // instead of a whole screen (../../../docs/wallet-viewer.md, "Design B").
  PageImage = 5,
};

// Which of the three zoom levels a screen belongs to. Deliberately separate
// from AssetType: the level is what the viewer is showing, the type is what the
// file claims to be, and a mismatch is a malformed asset.
enum class Level : uint8_t { Fit = 0, Detail = 1, OneToOne = 2 };
inline constexpr uint8_t kLevelCount = 3;

struct AssetHeader {
  AssetType assetType = AssetType::Unknown;
  uint8_t bitDepth = 0;
  uint8_t tileCol = 0;
  uint8_t tileRow = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t rawLen = 0;
  uint32_t version = 0;
  uint8_t flags = 0;
  uint8_t presentation = 0;
  // First 8 bytes of the sha256 of the payload. Parsed and carried, never
  // checked in P1 -- verification lands in the phase that adds encryption,
  // where it is read together with the AEAD tag (docs/wallet-viewer.md,
  // "What is deliberately absent").
  uint8_t sha256Prefix[8] = {0};
};

// flags bit 0: payload is encrypted. Always 0 in P1; a set bit is refused
// rather than drawn, because the bytes would be noise on the panel.
inline constexpr uint8_t kFlagEncrypted = 0x01;

// Only 1 bpp exists today. bitDepth 2 (4-level grey) is reserved in the format
// and not implemented: grey costs a second waveform pass (docs/eink-grayscale.md).
inline constexpr uint8_t kBitDepth1 = 1;

inline uint16_t readLe16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Structural parse only: magic and length. Whether the payload fits *this*
// panel is a separate question the reader asks against the live framebuffer
// size, because the panel is not the same on every device in the line
// (BoardConfig.h:670,685-690 is the X4's 800x480; X3 is its own).
inline bool parseAssetHeader(const uint8_t* bytes, size_t len, AssetHeader& out) {
  if (bytes == nullptr || len < kAssetHeaderBytes) return false;
  if (std::memcmp(bytes, kAssetMagic, sizeof(kAssetMagic)) != 0) return false;

  out.assetType = static_cast<AssetType>(bytes[4]);
  out.bitDepth = bytes[5];
  out.tileCol = bytes[6];
  out.tileRow = bytes[7];
  out.width = readLe16(bytes + 8);
  out.height = readLe16(bytes + 10);
  out.rawLen = readLe32(bytes + 12);
  out.version = readLe32(bytes + 16);
  out.flags = bytes[20];
  out.presentation = bytes[21];
  // bytes[22..23] reserved.
  std::memcpy(out.sha256Prefix, bytes + 24, sizeof(out.sha256Prefix));
  return true;
}

inline bool isHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 16 hex characters and nothing else. Enforced before the id ever reaches a
// path, so a manifest cannot name a file outside the wallet directory: no '/',
// no '.', no '..' survives this check.
inline bool isValidAssetId(const char* assetId) {
  if (assetId == nullptr) return false;
  // Scan to the terminator, never past it: a 15-character id must not cause a
  // read of the 17th byte of a 16-byte buffer.
  size_t i = 0;
  for (; assetId[i] != '\0'; ++i) {
    if (i >= kAssetIdLen) return false;
    if (!isHexDigit(assetId[i])) return false;
  }
  return i == kAssetIdLen;
}

// Builds "<root>/<first 2 of id>/<id>.dat". False for an id that is not 16 hex
// characters, or a buffer that would not hold the path.
//
// The root is a parameter so the host preview tool (test/wallet_preview) walks a
// generated tree in a temp directory through the *same* shard-and-name mapping
// the device uses on the card. One mapping, two roots.
inline bool buildAssetPathIn(const char* root, const char* assetId, char* out, size_t outLen) {
  if (out == nullptr || root == nullptr || !isValidAssetId(assetId)) return false;
  const size_t rootLen = std::strlen(root);
  const size_t needed = rootLen + 1 /* '/' */ + 2 + 1 /* '/' */ + kAssetIdLen + 4 /* ".dat" */ + 1;
  if (outLen < needed) return false;

  size_t at = 0;
  std::memcpy(out + at, root, rootLen);
  at += rootLen;
  out[at++] = '/';
  out[at++] = assetId[0];
  out[at++] = assetId[1];
  out[at++] = '/';
  std::memcpy(out + at, assetId, kAssetIdLen);
  at += kAssetIdLen;
  std::memcpy(out + at, ".dat", 4);
  at += 4;
  out[at] = '\0';
  return true;
}

// On the card: "/trailink/wallet/<first 2 of id>/<id>.dat".
inline bool buildAssetPath(const char* assetId, char* out, size_t outLen) {
  return buildAssetPathIn(kWalletDir, assetId, out, outLen);
}

// The panel an asset was built for, and the panel it is being drawn on.
//
// Never a constant. 48,000 bytes is the X4 (800x480, 100 B/row); the X3 is
// 792x528 at 99 B/row = 52,272 bytes
// (freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h:47-54). The
// device fills this in from the live renderer; the host preview fills it in from
// its --panel flag. Same struct, same checks, both sides.
struct PanelGeometry {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t rowBytes = 0;
  uint32_t bufferBytes = 0;
};

// The two panels that exist today, for the preview tool and the tests. The
// device never uses these -- it reads the live renderer.
inline constexpr PanelGeometry kPanelX4 = {800, 480, 100, 48000};
inline constexpr PanelGeometry kPanelX3 = {792, 528, 99, 52272};

// What the manifest's "panel" object declares. One asset set per wallet, and the
// wallet says which panel it was built for.
//
//   "panel": {"name":"x4","width":800,"height":480,"rowBytes":100,"assetBytes":48000}
//
// A manifest with no "panel" object predates that field. Treated as "the live
// panel", with the per-asset header check as the real gate -- see
// ../../../docs/wallet-viewer.md, "The manifest names the panel".
inline constexpr size_t kPanelNameBufBytes = 12;

struct DeclaredPanel {
  bool present = false;
  char name[kPanelNameBufBytes] = {0};
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t rowBytes = 0;
  uint32_t assetBytes = 0;
};

// A field the manifest left out (0) is not compared: it declared nothing about
// it. A field it did declare must match exactly.
inline bool panelMatches(const DeclaredPanel& declared, const PanelGeometry& live) {
  if (!declared.present) return true;
  if (declared.width != 0 && declared.width != live.width) return false;
  if (declared.height != 0 && declared.height != live.height) return false;
  if (declared.rowBytes != 0 && declared.rowBytes != live.rowBytes) return false;
  if (declared.assetBytes != 0 && declared.assetBytes != live.bufferBytes) return false;
  return true;
}

// One level's whole-page image, as the manifest describes it.
//
// The image is bigger than the panel and is not panel-shaped: the 1:1 page image
// is byte-identical on an X4 and an X3, because there is no grid in it and so
// nothing about it is cut to a screen. That is why a page image is validated
// against its own geometry (`rowBytes * nativeHeight == rawLen`) and against the
// panel window fitting inside it -- never against the panel's own width and
// height, which is what a tile asset is checked against.
struct PageImageSpec {
  bool present = false;
  char assetId[kAssetIdBufBytes] = {0};
  uint16_t nativeWidth = 0;
  uint16_t nativeHeight = 0;
  uint16_t rowBytes = 0;
  uint32_t rawLen = 0;
  // How far one press moves the window, in native pixels. The generator
  // guarantees windowStepX is a multiple of 8 -- see clampWindowOrigin().
  uint16_t windowStepX = 0;
  uint16_t windowStepY = 0;
  // Where the level opens. Not 0,0: on a 1:1 page that is the top-left margin.
  uint16_t focalX = 0;
  uint16_t focalY = 0;
};

// How far the window origin may travel on one axis: clamped, never wrapped.
// `span` is the image, `window` is the panel. A page smaller than the panel on an
// axis pins the origin at 0 rather than going negative.
inline uint32_t clampWindowOrigin(int32_t value, uint32_t span, uint32_t window) {
  if (span <= window) return 0;
  const int32_t limit = static_cast<int32_t>(span - window);
  if (value < 0) return 0;
  return value > limit ? static_cast<uint32_t>(limit) : static_cast<uint32_t>(value);
}

// The x limit in *bytes*, which is the constraint that actually matters: a row
// read starts at x/8 and runs for the panel's rowBytes, so it must not leave the
// image's row. Multiplying back by 8 makes the limit inherently 8-aligned, so
// clamping can never produce an unaligned origin even for an image whose
// nativeWidth is not a multiple of 8.
inline uint32_t maxWindowX(const PageImageSpec& page, uint16_t panelRowBytes) {
  if (page.rowBytes <= panelRowBytes) return 0;
  return static_cast<uint32_t>(page.rowBytes - panelRowBytes) * 8u;
}

// Why an asset cannot be drawn, or Ok. The single gate every reader passes
// through -- the device (WalletStore::loadAssetIntoFrameBuffer), the host
// preview (test/wallet_preview) and the tests all call this one function, so a
// disagreement about the header layout cannot hide on one side.
enum class AssetCheck : uint8_t {
  Ok = 0,
  Malformed,   // bad magic, or fewer than 32 bytes
  Encrypted,   // flags bit 0 -- a later phase's job
  BitDepth,    // not 1 bpp
  WrongPanel,  // rawLen / width / height is not this panel's, or the window does not fit
  PageImageMismatch,  // a page image whose header disagrees with the manifest
};

// Shared head of both gates: the things that are wrong regardless of what shape
// the asset is meant to be.
inline AssetCheck checkAssetCommon(const uint8_t* bytes, size_t len, AssetHeader& out) {
  if (!parseAssetHeader(bytes, len, out)) return AssetCheck::Malformed;
  if ((out.flags & kFlagEncrypted) != 0) return AssetCheck::Encrypted;
  if (out.bitDepth != kBitDepth1) return AssetCheck::BitDepth;
  return AssetCheck::Ok;
}

// A page image: checked against its own stated geometry and against the panel
// window fitting inside it. Deliberately NOT against the panel's width and
// height -- see PageImageSpec.
inline AssetCheck checkPageImage(const uint8_t* bytes, size_t len, const PageImageSpec& page,
                                 const PanelGeometry& live, AssetHeader& out) {
  const AssetCheck common = checkAssetCommon(bytes, len, out);
  if (common != AssetCheck::Ok) return common;
  if (out.assetType != AssetType::PageImage) return AssetCheck::PageImageMismatch;
  // The file must agree with what the manifest promised about it.
  if (out.width != page.nativeWidth || out.height != page.nativeHeight) return AssetCheck::PageImageMismatch;
  if (page.rowBytes == 0 || page.nativeHeight == 0) return AssetCheck::PageImageMismatch;
  if (static_cast<uint32_t>(page.rowBytes) * page.nativeHeight != page.rawLen) return AssetCheck::PageImageMismatch;
  if (out.rawLen != page.rawLen) return AssetCheck::PageImageMismatch;
  // And the window has to fit. A page image narrower or shorter than this panel
  // cannot fill it, and that is a wrong-device problem, not a corrupt file.
  if (page.rowBytes < live.rowBytes || page.nativeHeight < live.height) return AssetCheck::WrongPanel;
  return AssetCheck::Ok;
}

// A tile asset: exactly one panel frame, checked against the panel.
inline AssetCheck checkAssetForPanel(const uint8_t* bytes, size_t len, const PanelGeometry& live, AssetHeader& out) {
  const AssetCheck common = checkAssetCommon(bytes, len, out);
  if (common != AssetCheck::Ok) return common;
  if (live.bufferBytes == 0 || out.rawLen != live.bufferBytes) return AssetCheck::WrongPanel;
  if (out.width != live.width || out.height != live.height) return AssetCheck::WrongPanel;
  return AssetCheck::Ok;
}

// The level a manifest key names. "fit", "detail", "one_to_one" -- anything
// else is a level this firmware does not know and skips.
inline bool levelFromKey(const char* key, Level& out) {
  if (key == nullptr) return false;
  if (std::strcmp(key, "fit") == 0) {
    out = Level::Fit;
    return true;
  }
  if (std::strcmp(key, "detail") == 0) {
    out = Level::Detail;
    return true;
  }
  if (std::strcmp(key, "one_to_one") == 0) {
    out = Level::OneToOne;
    return true;
  }
  return false;
}

inline const char* levelName(Level level) {
  switch (level) {
    case Level::Fit:
      return "fit";
    case Level::Detail:
      return "detail";
    case Level::OneToOne:
      return "one_to_one";
  }
  return "?";
}

// CONFIRM cycles FIT -> DETAIL -> 1:1 -> FIT.
inline Level nextLevel(Level level) {
  switch (level) {
    case Level::Fit:
      return Level::Detail;
    case Level::Detail:
      return Level::OneToOne;
    case Level::OneToOne:
      return Level::Fit;
  }
  return Level::Fit;
}

}  // namespace wallet
