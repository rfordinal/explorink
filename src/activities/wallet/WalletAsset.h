#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "WalletSha256.h"

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
  // First 8 bytes of the sha256 of the payload. Checked for a **machine code**
  // and for nothing else: a garbage document tile is cosmetic, a garbage barcode
  // is a rider at a gate with a pass that will not scan
  // (docs/wallet-viewer.md, "Why the hash is checked here and nowhere else").
  // The manifest's full hash wins where it has one; this prefix is the fallback.
  uint8_t sha256Prefix[kSha256PrefixBytes] = {0};
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

inline bool isHexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

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

// ---------------------------------------------------------------------------
// Machine-readable codes (P2)
//
// A boarding pass or a ticket is the most valuable thing a wallet can hold, and
// the only thing on this device that has to be read by a machine rather than by
// a person. So a code asset is not "a picture that happens to be a barcode":
//
//   * the laptop generator decodes the code, regenerates it clean, renders it to
//     the device asset and decodes it back OUT OF THAT ASSET. Only then does the
//     manifest say `verified` (tools/walletgen.py, verify_code_asset);
//   * the device checks the payload's sha256 before it draws it, which it does
//     for no other asset;
//   * nothing is drawn on top of it except the symbology label, and that only
//     where the framebuffer proves there is white space for it.
//
// The asset itself is an ordinary full-screen panel-native tile, so the read path
// is `Store::loadAssetIntoFrameBuffer()`'s, unchanged.
// ---------------------------------------------------------------------------

// "c001".
inline constexpr size_t kCodeIdBufBytes = 8;
// The longest symbology the generator emits is "datamatrix" (10 characters).
inline constexpr size_t kSymbologyBufBytes = 12;

// One entry of a page's `codes` array. Field names are the manifest's, verbatim.
//
// `payload` is deliberately **not** carried. The device has no use for the
// decoded text -- it draws the bitmap, it does not encode anything -- and a
// boarding-pass payload runs to hundreds of characters, which is a buffer nobody
// needs on a screen that must cost no RAM.
struct CodeEntry {
  bool present = false;
  char id[kCodeIdBufBytes] = {0};
  char symbology[kSymbologyBufBytes] = {0};
  char assetId[kAssetIdBufBytes] = {0};
  // Full sha256 of the payload, lowercase hex, from the manifest. Empty when the
  // manifest did not state one; then the header's 8-byte prefix is the authority
  // (WalletSha256.h, checkPayloadHash).
  char sha256[kSha256HexBufBytes] = {0};
  // The generator's round-trip result. **Defaults to false**: a manifest that
  // says nothing has not verified anything, and the safe reading of silence here
  // is "not verified", never "fine".
  bool verified = false;
  uint16_t moduleSize = 0;
  uint16_t quietZone = 0;
  // The code plus its quiet zone, in logical pixels. Read and logged; the label
  // placement does not trust them -- it reads the framebuffer instead, see
  // logicalBandIsBlank().
  uint16_t codeWidthPx = 0;
  uint16_t codeHeightPx = 0;
};

// Why an asset cannot be drawn, or Ok. The single gate every reader passes
// through -- the device (WalletStore::loadAssetIntoFrameBuffer), the host
// preview (test/wallet_preview) and the tests all call this one function, so a
// disagreement about the header layout cannot hide on one side.
enum class AssetCheck : uint8_t {
  Ok = 0,
  Malformed,          // bad magic, or fewer than 32 bytes
  Encrypted,          // flags bit 0 -- a later phase's job
  BitDepth,           // not 1 bpp
  WrongPanel,         // rawLen / width / height is not this panel's, or the window does not fit
  PageImageMismatch,  // a page image whose header disagrees with the manifest
  NotACode,           // the manifest called it a code, the header says otherwise
};

// Shared head of both gates: the things that are wrong regardless of what shape
// the asset is meant to be.
//
// `allowEncrypted` is what the wallet key buys. Without a key an encrypted asset is
// refused, because drawing ciphertext puts noise on the panel and looks like a
// hardware fault; with one it is accepted here and decrypted after the read
// (WalletStore.cpp, `readWholeScreen`). The default is false so every caller that
// has no key -- the host preview, the tests -- keeps the P1 behaviour.
inline AssetCheck checkAssetCommon(const uint8_t* bytes, size_t len, AssetHeader& out, bool allowEncrypted = false) {
  if (!parseAssetHeader(bytes, len, out)) return AssetCheck::Malformed;
  if (!allowEncrypted && (out.flags & kFlagEncrypted) != 0) return AssetCheck::Encrypted;
  if (out.bitDepth != kBitDepth1) return AssetCheck::BitDepth;
  return AssetCheck::Ok;
}

// A page image: checked against its own stated geometry and against the panel
// window fitting inside it. Deliberately NOT against the panel's width and
// height -- see PageImageSpec.
inline AssetCheck checkPageImage(const uint8_t* bytes, size_t len, const PageImageSpec& page, const PanelGeometry& live,
                                 AssetHeader& out, bool allowEncrypted = false) {
  const AssetCheck common = checkAssetCommon(bytes, len, out, allowEncrypted);
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
inline AssetCheck checkAssetForPanel(const uint8_t* bytes, size_t len, const PanelGeometry& live, AssetHeader& out,
                                     bool allowEncrypted = false) {
  const AssetCheck common = checkAssetCommon(bytes, len, out, allowEncrypted);
  if (common != AssetCheck::Ok) return common;
  if (live.bufferBytes == 0 || out.rawLen != live.bufferBytes) return AssetCheck::WrongPanel;
  if (out.width != live.width || out.height != live.height) return AssetCheck::WrongPanel;
  return AssetCheck::Ok;
}

// A machine code: an ordinary panel-shaped tile that must also *say* it is a
// code. A manifest entry pointing at a document tile is a generator or a sync
// bug, and drawing page three of an insurance policy where a boarding pass
// should be is worse than drawing nothing.
inline AssetCheck checkCodeAsset(const uint8_t* bytes, size_t len, const PanelGeometry& live, AssetHeader& out,
                                 bool allowEncrypted = false) {
  const AssetCheck panelCheck = checkAssetForPanel(bytes, len, live, out, allowEncrypted);
  if (panelCheck != AssetCheck::Ok) return panelCheck;
  if (out.assetType != AssetType::MachineCode) return AssetCheck::NotACode;
  return AssetCheck::Ok;
}

// The one decision that lets a code bitmap reach the panel.
//
// Pure, and called from exactly one place (WalletCodeActivity::showCurrent), so
// every branch is host-testable and no second path can grow beside it. The
// failure *screen* needs a renderer and cannot be tested on the host; this
// verdict, which is what selects it, can.
enum class CodeVerdict : uint8_t {
  Draw,            // refresh the panel
  RefuseAsset,     // the file, the gate or the hash said no -- nothing is drawn
  RefuseUnmarked,  // unverified, and the "not verified" marker had nowhere to go
};

// `loadedAndHashed` -- the payload is in the framebuffer and its sha256 matched.
// `manifestVerified` -- the manifest's `verified` for this code.
// `markerPlaced` -- the unverified marker was actually drawn on white space.
//
// An unverified code is shown, but only ever *marked* (docs/wallet-viewer.md,
// "Unverified codes are shown, marked"). So if the marker could not be placed,
// the code does not go up either: an unmarked unverified code is exactly the lie
// this feature must not tell.
inline CodeVerdict codeVerdict(bool loadedAndHashed, bool manifestVerified, bool markerPlaced) {
  if (!loadedAndHashed) return CodeVerdict::RefuseAsset;
  if (!manifestVerified && !markerPlaced) return CodeVerdict::RefuseUnmarked;
  return CodeVerdict::Draw;
}

// True when a horizontal band of the LOGICAL portrait screen holds no ink at all
// in a panel-native framebuffer.
//
// This is how the symbology label finds its space. The alternative was to trust
// `codeWidthPx` / `codeHeightPx` plus "the generator centres it" -- an assumption
// about a tool in another repo, guarding the one thing on this screen that must
// not be drawn over. Reading the bytes needs no assumption: if there is ink in
// the band, there is no label.
//
// Logical portrait (x, y) maps to panel (y, panelHeight - 1 - x)
// (GfxRenderer::rotateCoordinates(), lib/GfxRenderer/GfxRenderer.cpp:216-223), so
// a logical band of rows is a panel band of *columns* across every panel row --
// which is why this cannot be a memcmp over a row range.
inline bool logicalBandIsBlank(const uint8_t* fb, const PanelGeometry& panel, int y0, int y1) {
  if (fb == nullptr || panel.rowBytes == 0) return false;
  // A logical y is a panel x, so the band is clamped against the panel's WIDTH
  // (800), not its height. Getting this the wrong way round would check the wrong
  // 480 columns and pass a band that is not blank at all.
  if (y0 < 0) y0 = 0;
  if (y1 > static_cast<int>(panel.width)) y1 = panel.width;
  if (y1 <= y0) return false;
  for (uint16_t phyY = 0; phyY < panel.height; ++phyY) {
    const uint8_t* row = fb + static_cast<size_t>(phyY) * panel.rowBytes;
    for (int phyX = y0; phyX < y1; ++phyX) {
      // bit 1 = white; a cleared bit is ink (GfxRenderer.cpp:517-524).
      if (((row[phyX / 8] >> (7 - (phyX % 8))) & 1u) == 0u) return false;
    }
  }
  return true;
}

// Where LEFT/RIGHT land on the code ring.
//
// **The walk wraps; it does not clamp.** That is the opposite of the document
// viewer's arrows, on purpose: a page has edges because paper has edges, and a
// press at the edge doing nothing is what a sheet of paper does. A document's two
// or three codes have no edges and no spatial meaning -- a rider at a gate flips
// between a boarding pass and a bag tag and should not have to remember which end
// of the list they are at. A ring of two is also the common case, where clamping
// would make one of the two buttons dead half the time.
//
// One function, two call sites, so the browse screen's entry point is a step on
// the same ring rather than a second rule:
//
//   browse RIGHT -> walkCodeIndex(-1, +1, count) == 0            (the first code)
//   browse LEFT  -> walkCodeIndex(0,  -1, count) == count - 1    (the last)
//   code screen  -> walkCodeIndex(current, +/-1, count)
//
// Returns -1 for an item with no codes, which is the caller's cue to do nothing.
// A ring of one returns `from` unchanged, so the caller sees "no move" and spends
// no refresh.
inline int walkCodeIndex(int from, int delta, uint16_t count) {
  if (count == 0) return -1;
  const int n = static_cast<int>(count);
  int next = from + delta;
  // Modulo, written out: `%` on a negative left operand is implementation-defined
  // territory nobody should have to think about at a gate.
  while (next < 0) next += n;
  while (next >= n) next -= n;
  return next;
}

// Parses `CMD:GOTO_WALLET`'s optional arguments: nothing, an item index, or an
// item and a code index.
//
//   ""      -> item -1, code -1   (the browse list)
//   "0"     -> item  0, code -1   (open document 0)
//   "0 3"   -> item  0, code  3   (open document 0's code 3)
//
// False for anything else -- a negative number, a non-number, a third argument.
// Refused rather than coerced: a host script that mistypes an index should be told,
// not silently shown document 0 (../../../docs/wallet-viewer.md, "CMD:GOTO_WALLET").
//
// Range is NOT checked here; nothing outside the manifest knows how many documents
// a wallet has. WalletActivity checks it against the real list and says so on the
// panel and in the log.
inline bool parseGotoWalletArgs(const char* args, int& itemIndex, int& codeIndex) {
  itemIndex = -1;
  codeIndex = -1;
  if (args == nullptr) return true;

  int values[2] = {0, 0};
  int found = 0;
  size_t i = 0;
  for (;;) {
    while (args[i] == ' ' || args[i] == '\t') ++i;
    if (args[i] == '\0') break;
    if (args[i] < '0' || args[i] > '9') return false;  // no signs, no letters
    if (found >= 2) return false;                      // a third argument is a typo, not a feature
    int value = 0;
    while (args[i] >= '0' && args[i] <= '9') {
      value = value * 10 + (args[i] - '0');
      if (value > 9999) return false;  // no wallet has ten thousand documents
      ++i;
    }
    values[found++] = value;
  }
  if (found >= 1) itemIndex = values[0];
  if (found >= 2) codeIndex = values[1];
  return true;
}

// "qr" -> "QR", "pdf417" -> "PDF417". A symbology is a technical identifier, not
// prose: it is never translated, only upper-cased for the label. ASCII only,
// which is all a symbology name ever is.
//
// An empty or missing symbology becomes "CODE", so the label never comes out
// blank on a manifest that left the field out.
inline void symbologyLabel(const char* symbology, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) return;
  if (symbology == nullptr || symbology[0] == '\0') {
    const char* fallback = "CODE";
    size_t i = 0;
    for (; fallback[i] != '\0' && i + 1 < outLen; ++i) out[i] = fallback[i];
    out[i] = '\0';
    return;
  }
  size_t i = 0;
  for (; symbology[i] != '\0' && i + 1 < outLen; ++i) {
    const char c = symbology[i];
    out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  out[i] = '\0';
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
