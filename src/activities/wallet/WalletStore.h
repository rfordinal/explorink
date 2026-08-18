#pragma once

#include <HalStorage.h>

#include <cstdint>

#include "WalletAsset.h"
#include "WalletCrypto.h"
#include "WalletManifestParser.h"

class GfxRenderer;

// The card side of the wallet: the manifest, and one asset into the framebuffer.
//
// Read-only by construction. There is no write, rename, delete or truncate path
// anywhere in this file or in the two activities above it, and there must never
// be one: the card is the only offline copy of a rider's documents, and a device
// that can destroy it is worse than one that cannot show it
// (../../../docs/wallet-viewer.md).
namespace wallet {

inline constexpr const char* kManifestPath = "/trailink/wallet/manifest.json";
// The encrypted tree's manifest, and its one backup. A tree is either fully
// encrypted or fully cleartext, never mixed, and which one it is, is stated by
// which of these two files exists (parent repo docs/wallet-format.md, section 3).
inline constexpr const char* kManifestBakPath = "/trailink/wallet/manifest.bak";
// Bumped when the on-card layout changes shape. A manifest from the future is
// refused with a message, never half-read.
inline constexpr uint32_t kSupportedFormatVersion = 1;

enum class Error : uint8_t {
  None = 0,
  NoManifest,          // nothing at kManifestPath
  ManifestUnreadable,  // opened, but the JSON did not parse
  ManifestVersion,     // formatVersion this firmware does not know
  PanelMismatch,       // built for another panel, and the manifest says so
  NoItems,             // parsed fine, carries nothing
  NoAsset,             // the manifest names an assetId with no file behind it
  BadAsset,            // wrong magic, wrong bit depth, header short
  AssetEncrypted,      // flags bit 0 set -- a later phase's job
  AssetWrongSize,      // rawLen is not this panel's framebuffer
  ShortRead,           // the payload ended early
  NoFrameBuffer,       // the framebuffer is lent out
  NotInManifest,       // no such item / page / tile
  Locked,              // the tree is encrypted and no key is held: unlock first
  ManifestTooBig,      // an encrypted manifest above the RAM cap, or no heap for it
  ManifestAuth,        // the GCM tag did not verify: wrong key, or an altered file
  AssetDecrypt,        // the payload did not decrypt to its plaintext
  NoCodes,             // the item carries no machine-readable codes
  CodeNotACode,        // the manifest called it a code, the header disagrees
  CodeHashMismatch,    // the payload does not hash to what the manifest promised
  CodeUnmarked,        // unverified, and the marker had nowhere to go
};

// Translated one-line reason, for the empty state and the missing-asset screen.
// PanelMismatch is deliberately absent here: it needs two geometries in the
// sentence, so the screen builds that line itself.
const char* errorText(Error error);

// The panel this device actually has, read off the live renderer. Never a
// constant (WalletAsset.h, PanelGeometry).
PanelGeometry livePanel(const GfxRenderer& renderer);

// True when the card carries an encrypted tree, i.e. `manifest.enc` exists. Asked
// before anything else, because an encrypted tree needs the key first and a
// cleartext one must keep working exactly as it did (a card in the field may be
// either).
bool treeIsEncrypted();

// Walks /trailink/wallet's shard directories and counts the assets whose 32-byte
// header parses, plus how many of those are machine codes. No key needed and no
// manifest consulted -- headers are cleartext.
//
// This is the honest half of brief 32's recovery: it can say **what the card
// holds**, so a broken manifest never reads as an empty wallet. It cannot rebuild
// the manifest, because the header carries no item or page id -- see
// docs/wallet-sync.md. Recovery proper is a resync from the phone, which is the
// authority (brief 43).
void countCardAssets(uint16_t& assets, uint16_t& codes);

// One window of one 1bpp plane, row by row, straight into `dest`.
//
// The whole seek-read-decrypt arithmetic of a page-image window, in one place,
// because there are two callers of it and they must not drift: the 1bpp page
// reader (one plane, `planeBase` 0) and the grey page reader (three planes, one
// `planeBase` each -- ../../../docs/wallet-grey.md).
//
// `planeBase` is a PAYLOAD offset, not a file offset: the 32-byte cleartext header
// is added for the seek and left out of the CTR offset, because the keystream is
// indexed from the first byte of the payload. Getting that one wrong decrypts to
// noise 32 bytes out of phase, which looks like a wrong key.
//
// `key` null means the asset is cleartext. `dest` rows are `destRowBytes` apart,
// which is the panel's stride; `srcRowBytes` is the image's, which is larger.
bool readPlaneWindow(HalFile& file, const uint8_t* key, const uint8_t iv[kAssetIvLen], uint32_t planeBase,
                     uint32_t srcRowBytes, uint32_t xByte, uint32_t y, uint32_t rows, uint8_t* dest,
                     uint32_t destRowBytes, Error& error);

struct Store {
  // Fills `out` with up to `max` items in manifest order. `seen` counts every
  // item the manifest holds, so a truncated list can say so.
  //
  // The manifest's declared panel is checked against the live one **first**, and
  // a mismatch refuses the whole wallet with Error::PanelMismatch -- one asset
  // set per wallet, so if the set is for another device none of it can be drawn.
  // `declared` is filled in whenever the manifest parsed, so the screen can name
  // both panels in the message.
  static bool listItems(ItemEntry* out, uint16_t max, const GfxRenderer& renderer, uint16_t& stored, uint32_t& seen,
                        DeclaredPanel& declared, Error& error);

  // One item's codes: how many it has across all its pages, and the one at
  // `codeIndex`. Re-run per screen, same reasoning as lookupPage().
  //
  // `codeIndex` out of range is not an error for the count -- a caller asking
  // only "does this item have codes" passes 0 and reads `out.codeCount`.
  static bool lookupCode(int itemIndex, int codeIndex, CodeLookup& out, Error& error);

  // One item, one page: the grid of all three levels, the requested level's
  // whole-page image if it has one, and the assetId of the named tile. Re-run per
  // screen; see WalletManifestParser.h for why that is cheaper than caching.
  //
  // Succeeds when the level offers *either* source. A level with a page image
  // need carry no tile assets at all, and a card generated before design B
  // carries only tiles -- both must open.
  static bool lookupPage(int itemIndex, int pageIndex, Level level, uint8_t col, uint8_t row, PageLookup& out,
                         Error& error);

  // Open, skip the 32-byte header, read the payload straight into the
  // framebuffer, and hand back the header. No scratch buffer exists anywhere on
  // this path: the destination is the framebuffer itself, so a screen costs zero
  // extra RAM. Same move as the sleep frame (src/main.cpp:216-234).
  //
  // The caller refreshes; this function never touches the panel.
  static bool loadAssetIntoFrameBuffer(const char* assetId, GfxRenderer& renderer, AssetHeader& header, Error& error);

  // One machine code into the framebuffer, **hashed before it can be shown**.
  //
  // Same single read as loadAssetIntoFrameBuffer() -- and then the sha256 of the
  // bytes now sitting in the framebuffer, against the manifest's hash (or the
  // header's 8-byte prefix where the manifest states none). Nothing is drawn: the
  // caller refreshes only if this returned true, so a payload that fails the hash
  // never reaches the panel.
  //
  // Hashing the framebuffer rather than re-reading the file is what makes this
  // one read instead of two: the payload *is* the framebuffer, so the bytes to
  // hash are already in RAM. ~8 ms of CPU against ~65 ms of card
  // (docs/wallet-viewer.md, "Why the hash is checked here and nowhere else").
  static bool loadCodeIntoFrameBuffer(const CodeEntry& code, GfxRenderer& renderer, AssetHeader& header, Error& error);
};

// One open whole-page image, and an arbitrary window blitted out of it.
//
// The file stays open across presses. That is not an optimisation, it is what was
// measured: 282.8 ms for a window with the file already open
// (../../../docs/wallet-viewer.md, "Measured on the X4"). Reopening per frame is
// unmeasured cost, so the design does not do it.
//
// Row by row, straight into the framebuffer: 480 seeks and 480 reads of the
// panel's rowBytes. No scratch buffer for the image exists here either -- in
// panel-native order a horizontal pan is pure row selection and a vertical pan is
// a byte offset inside the row, so an 8-aligned window needs no bit rotation and
// nothing to transform it in.
class PageReader {
 public:
  PageReader() = default;
  ~PageReader() { close(); }
  PageReader(const PageReader&) = delete;
  PageReader& operator=(const PageReader&) = delete;

  // Opens `page.assetId` and validates it against the manifest's promise and
  // against the window fitting. A second call for the same assetId is a no-op, so
  // a pan does not reopen anything.
  bool open(const PageImageSpec& page, const GfxRenderer& renderer, Error& error);
  void close();
  bool isOpen() const { return open_; }
  const PageImageSpec& spec() const { return spec_; }
  const AssetHeader& header() const { return header_; }

  // Blits the window whose top-left is (x, y) in native pixels. x must be a
  // multiple of 8 -- the caller clamps with clampWindowOrigin()/maxWindowX(),
  // which cannot produce anything else, so an unaligned x here is a bug and is
  // refused rather than rounded.
  bool readWindow(uint32_t x, uint32_t y, GfxRenderer& renderer, Error& error);

 private:
  HalFile file_;
  PageImageSpec spec_;
  AssetHeader header_;
  bool open_ = false;
  // Built once on open() and reused for every row: the IV depends on the asset and
  // its version, not on the window. Only the CTR *offset* changes per row.
  bool encrypted_ = false;
  uint8_t iv_[kAssetIvLen] = {0};
};

}  // namespace wallet
