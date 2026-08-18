#pragma once

#include <cstdint>

#include "WalletGreyPage.h"
#include "WalletStore.h"
#include "activities/Activity.h"

// The viewer: one asset, one whole screen, no chrome.
//
// ## The display path
//
// A wallet asset is already the framebuffer. The generator on the laptop did
// every rotation, scale, dither and pack at build time -- its rotation is the
// exact inverse of GfxRenderer's Portrait transform, so the page stands upright
// on a device held the way the rest of the UI expects. So a screen is:
//
//     open -> skip the 32-byte header -> read the payload into the framebuffer
//     -> displayBuffer()
//
// No scratch buffer exists on that path (WalletStore::loadAssetIntoFrameBuffer),
// so a screen costs zero extra RAM. That is not a nicety: with BLE up the
// largest contiguous heap block is about 43 KB (../../../docs/map-memory.md:57)
// and a screen is 48 KB. Staging one would fail before it could be drawn.
//
// ## Two sources per level, and the device picks
//
// A level offers either a **whole-page image** (design B: one image per level, an
// arbitrary window blitted out of it, the pan step a fraction of the view) or a
// **tile grid** (the original: one asset per screen, one press one screen).
// `pageImage` in the manifest wins when it is there; the tile path stays for
// cards generated before design B, and for a generator that still emits both.
// Neither is deleted.
//
// ## Buttons
//
//   CONFIRM      cycle level: FIT -> DETAIL -> 1:1 -> FIT
//   CONFIRM held  1bpp <-> grey, in a lab build only (see below)
//   LEFT/RIGHT   pan by windowStepX, or step tile column, clamped
//   UP/DOWN      pan by windowStepY, or step tile row, clamped
//                ... except where there is nothing to step on that axis (always
//                the case at FIT), where they turn the page instead
//   BACK         back to the browse list
//
// UP/DOWN are the two side buttons and LEFT/RIGHT two of the four front buttons
// (MappedInputManager.cpp:51-108). Six buttons, seven roles -- so one pair does
// two jobs, and the pair chosen is the one that has nothing to do at FIT.
// Reasoning in ../../../docs/wallet-viewer.md, "The button map".
//
// ## Grey is the same page, drawn the other way (P2b)
//
// When the level carries a `greyPlanes` asset and the grey switch is on, the frame
// is a four-level grey render instead of a 1bpp one: same window, same origin, same
// document -- `../../../docs/wallet-grey.md`. Off by default, and a level with no
// grey asset never notices the switch.
//
// A held CONFIRM flips it, so the two versions can be compared back to back on the
// glass, which is the only way that comparison can be made. It is a **lab
// affordance**, compiled in with the wallet's other test seams
// (`ENABLE_WALLET_TEST_CMDS`, on in the dev environment and in no release one) --
// the doc's own argument against hold-to-page applies to a rider's build, and this
// is not one. `CMD:WALLETGREY` is the same switch from a host, which is how the
// verification runs without hands.
//
// ## No overlay
//
// The asset fills the panel; nothing is drawn on top of it. A level or page
// badge would sit over the document, and every pixel here belongs to a paper the
// rider may have to show to somebody. The failure screen is the only frame this
// activity draws itself.
//
// Read-only: no write path of any kind.
class WalletViewActivity final : public Activity {
 public:
  WalletViewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int itemIndex, const char* title,
                     uint16_t pageCount);

  void onEnter() override;
  void loop() override;

 private:
  void showCurrent(HalDisplay::RefreshMode mode);
  // The grey frame for the page on screen. `fatal` comes back true when the grey
  // asset itself is wrong, i.e. the caller must show a failure screen rather than
  // quietly drawing the 1bpp version of the same page.
  bool showGreyPage(const wallet::PageLookup& page, bool& fatal);
  // The 1bpp comparison point, measured on the same page in the same session:
  // one card read plus the refresh that follows it. Printed in the same
  // WALLETGREY line shape as the grey stages, so a host captures both.
  void logOneBppCost(uint32_t cardUs, uint32_t refreshUs, uint32_t cardBytes);
  void drawFailure(HalDisplay::RefreshMode mode);
  // `down` moves toward the bottom of the document, `across` toward its left --
  // see the axis note in the .cpp, this is not the same thing as native x and y.
  bool stepView(int down, int across);
  bool stepPage(int delta);
  void cycleLevel();
  // Where a level opens: the manifest's focal tile for it, clamped into the grid.
  void jumpToLevelDefault();
  // True when the view cannot move down the document at all, so the side buttons
  // have nothing to pan and turn the page instead.
  bool verticalAxisIdle() const;
  // Travel limits, per axis, for the page image on screen.
  uint32_t downLimit() const;
  uint32_t acrossLimit() const;
  const wallet::LevelGrid& currentGrid() const { return grid_[static_cast<uint8_t>(level_)]; }
  // The page image on screen, whichever of the two it is. The grey asset and the
  // 1bpp asset of one level describe the same page, so the window origin means the
  // same thing in both -- but the limits are read off whichever is actually open.
  const wallet::PageImageSpec& activeSpec() const;

  const int itemIndex_;
  char title_[wallet::kTitleBufBytes] = {0};
  uint16_t pageCount_ = 0;

  int pageIndex_ = 0;
  wallet::Level level_ = wallet::Level::Fit;
  // Tile-grid coordinates. Unused while the level has a page image.
  uint8_t col_ = 0;
  uint8_t row_ = 0;
  // Page-image window origin, in the page image's own native pixels, exactly as
  // handed to PageReader::readWindow(). Native x runs DOWN the document and is
  // the 8-aligned axis; native y runs ACROSS it, inverted. See the axis note in
  // the .cpp -- getting these two the wrong way round rotates the pan by ninety
  // degrees, which is a bug the host preview caught before it ever ran.
  uint32_t winNativeX_ = 0;
  uint32_t winNativeY_ = 0;
  // The level on screen is a page image, so the arrows pan instead of stepping.
  bool usingPageImage_ = false;
  // ... and it is the grey one.
  bool usingGrey_ = false;
  // The frame on the panel is a grey frame. The next 1bpp frame must then be HALF,
  // never FAST: grey residue ghosts the following frame and a plain fast diff
  // cannot clear it (docs/eink-grayscale.md; the reader forces the same cadence at
  // EpubReaderActivity.cpp:1560-1567).
  bool lastFrameWasGrey_ = false;
  // millis() when CONFIRM went down, so a held CONFIRM can mean something else
  // than a pressed one.
  uint32_t confirmDownMs_ = 0;
  // Set by anything that changes which level or page is on screen; consumed by
  // showCurrent(), which then positions the view at the focal point of whichever
  // source the new level turns out to have.
  bool needsReposition_ = true;
  // The open page-image file. Kept open across presses: a window with the file
  // already open measured 282.8 ms on the X4, and reopening per frame is
  // unmeasured cost (docs/wallet-viewer.md).
  wallet::PageReader page_;
  // The grey asset of the same level, kept open beside it so an A/B toggle costs no
  // open (docs/wallet-grey.md).
  wallet::GreyPageReader greyPage_;

  // Grids for all three levels of the page on screen, from the same manifest
  // pass that fetched the asset. What the arrows clamp against.
  wallet::LevelGrid grid_[wallet::kLevelCount] = {};
  wallet::Error error_ = wallet::Error::None;
};
