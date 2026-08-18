#pragma once

#include <cstdint>

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
  // Set by anything that changes which level or page is on screen; consumed by
  // showCurrent(), which then positions the view at the focal point of whichever
  // source the new level turns out to have.
  bool needsReposition_ = true;
  // The open page-image file. Kept open across presses: a window with the file
  // already open measured 282.8 ms on the X4, and reopening per frame is
  // unmeasured cost (docs/wallet-viewer.md).
  wallet::PageReader page_;

  // Grids for all three levels of the page on screen, from the same manifest
  // pass that fetched the asset. What the arrows clamp against.
  wallet::LevelGrid grid_[wallet::kLevelCount] = {};
  wallet::Error error_ = wallet::Error::None;
};
