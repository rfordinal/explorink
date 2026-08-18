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

// Builds "/trailink/wallet/<first 2 of id>/<id>.dat". False for an id that is
// not 16 hex characters, or a buffer that would not hold the path.
inline bool buildAssetPath(const char* assetId, char* out, size_t outLen) {
  if (out == nullptr || !isValidAssetId(assetId)) return false;
  const size_t dirLen = std::strlen(kWalletDir);
  const size_t needed = dirLen + 1 /* '/' */ + 2 + 1 /* '/' */ + kAssetIdLen + 4 /* ".dat" */ + 1;
  if (outLen < needed) return false;

  size_t at = 0;
  std::memcpy(out + at, kWalletDir, dirLen);
  at += dirLen;
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
