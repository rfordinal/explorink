#pragma once

#include <HalStorage.h>

#include <cstdint>

#include "WalletAsset.h"
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
// Bumped when the on-card layout changes shape. A manifest from the future is
// refused with a message, never half-read.
inline constexpr uint32_t kSupportedFormatVersion = 1;

enum class Error : uint8_t {
  None = 0,
  NoManifest,         // nothing at kManifestPath
  ManifestUnreadable, // opened, but the JSON did not parse
  ManifestVersion,    // formatVersion this firmware does not know
  PanelMismatch,      // built for another panel, and the manifest says so
  NoItems,            // parsed fine, carries nothing
  NoAsset,            // the manifest names an assetId with no file behind it
  BadAsset,           // wrong magic, wrong bit depth, header short
  AssetEncrypted,     // flags bit 0 set -- a later phase's job
  AssetWrongSize,     // rawLen is not this panel's framebuffer
  ShortRead,          // the payload ended early
  NoFrameBuffer,      // the framebuffer is lent out
  NotInManifest,      // no such item / page / tile
};

// Translated one-line reason, for the empty state and the missing-asset screen.
// PanelMismatch is deliberately absent here: it needs two geometries in the
// sentence, so the screen builds that line itself.
const char* errorText(Error error);

// The panel this device actually has, read off the live renderer. Never a
// constant (WalletAsset.h, PanelGeometry).
PanelGeometry livePanel(const GfxRenderer& renderer);

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
};

}  // namespace wallet
