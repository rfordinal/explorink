#pragma once

#include <HalDisplay.h>
#include <HalStorage.h>

#include <cstdint>

#include "WalletAsset.h"
#include "WalletCrypto.h"
#include "WalletGreyStatus.h"
#include "WalletStore.h"

class GfxRenderer;

// The grey half of the wallet viewer: one baked-plane asset, one grey frame.
//
// **The whole point of this file is that it renders nothing.** The panel's four
// grey levels are a BW base frame plus two bit planes streamed to controller RAM
// (../../../docs/eink-grayscale.md), and the generator bakes all three. So a grey
// wallet page is three windows off the card and four driver calls -- not the
// 13-callback `GrayscaleFrame::render()` the map would pay, and no per-band
// drawing at all. `../../../docs/wallet-grey.md` is the layout, the sequence and
// the measured cost.
//
// The rules this file exists to obey, all of them measured on hardware and all of
// them in docs/eink-grayscale.md:
//
//   * the base frame is displayed BEFORE the planes are written, because the LSB
//     plane *is* BW RAM and writing it destroys the frame the controller holds;
//   * `cleanupGrayscaleWithFrameBuffer()` + `resyncControllerBwRam()` run before
//     this code returns, always, including on every failure path after the first
//     plane byte was written. Without them the next windowed or fast update is
//     promoted to a full HALF and every grey on the panel dies;
//   * nothing refreshes in the same breath as the grey nudge. The frame says
//     everything it has to say; the caller returns and lets the panel settle.
namespace wallet {

// GreyOutcome, greyOutcomeName(), greyOutcomeIsCapability(), GreyTimings and the
// status ledger live in WalletGreyStatus.h: no panel, no card, host-testable, and
// the one place the meaning of every number CMD:WALLETGREY prints is decided.
Error greyOutcomeToError(GreyOutcome outcome);

// One open GreyPlanes asset, and the whole grey sequence for one window.
//
// Kept open across presses for the same reason PageReader is: a window with the
// file already open is the measured cost, reopening per frame is not
// (docs/wallet-viewer.md, "Measured on the X4").
class GreyPageReader {
 public:
  GreyPageReader() = default;
  ~GreyPageReader() { close(); }
  GreyPageReader(const GreyPageReader&) = delete;
  GreyPageReader& operator=(const GreyPageReader&) = delete;

  // Opens `page.assetId` and checks it against the manifest's promise, against
  // this panel and against the window fitting. A second call for the same assetId
  // is a no-op, so a pan does not reopen anything.
  GreyOutcome open(const PageImageSpec& page, const GfxRenderer& renderer);
  void close();
  bool isOpen() const { return open_; }
  const PageImageSpec& spec() const { return spec_; }
  const AssetHeader& header() const { return header_; }

  // The window whose top-left is (x, y) in native pixels, as a grey frame on the
  // panel. `x` must be a multiple of 8 (clampWindowOrigin()/maxWindowX() cannot
  // produce anything else).
  //
  // The caller must hold a RenderLock: this writes the framebuffer and drives the
  // controller, and the render task owns both.
  //
  // `baseMode` is the refresh the BW base frame gets. HALF is what every working
  // grey path in this firmware uses and what the sleep screen says is required;
  // FAST is exposed only so the difference can be judged on the panel
  // (docs/wallet-grey.md, "Open: is a FAST base good enough").
  GreyOutcome render(uint32_t x, uint32_t y, GfxRenderer& renderer, HalDisplay::RefreshMode baseMode,
                     GreyTimings& timings);

  // Drop this reader's grey capture registration. **The caller must do this the
  // moment a BW frame replaces the grey one** -- a 1bpp page, a tile grid, a
  // failure screen -- because from then on the planes this reader would hand out
  // do not match the glass, and CMD:SCREENSHOT_GRAY would call them exact.
  //
  // render() arms the registration itself on success, and re-arming is what a pan
  // does; close() and the destructor release it. Safe to call when nothing is
  // armed.
  void releaseReplaySource();

 private:
  GreyOutcome readPlaneRows(GreyPlane plane, uint32_t xByte, uint32_t y, uint32_t rows, uint8_t* dest,
                            uint32_t destRowBytes);

  // The grey capture path. GrayscaleFrame replays a *drawn* frame by re-running its
  // draw callback; a wallet grey frame was never drawn -- the generator baked the
  // planes and render() streamed them from the card to controller RAM. So this
  // reader registers itself as a plane PRODUCER instead and re-reads the same
  // window on demand, which is bit-identical by construction and costs the same
  // 8 KB band as the panel path (docs/wallet-grey.md, "What a host can see of a
  // grey frame").
  static bool planeBandThunk(void* ctx, bool lsbPlane, uint8_t* rows, int yStart, int numRows);
  bool fillPlaneBand(bool lsbPlane, uint8_t* rows, int yStart, int numRows);

  HalFile file_;
  PageImageSpec spec_;
  AssetHeader header_;
  bool open_ = false;
  bool encrypted_ = false;
  uint8_t iv_[kAssetIvLen] = {0};
  // The window the last successful render() streamed to the panel, so the producer
  // above can read exactly those bytes again. Panel geometry cannot change under it.
  uint32_t replayXByte_ = 0;
  uint32_t replayY_ = 0;
  uint32_t replayRowBytes_ = 0;
  bool replayArmed_ = false;
};

// The A/B switch, and the last measured cost of each side of it.
//
// P2b is a decision phase: the same page, both ways, back to back on the glass,
// because whether grey reads better than a 1bpp dither is a human judgement and
// nothing on the laptop can settle it. So the mode is process-wide state rather
// than a per-activity field -- a serial command has to be able to flip it while a
// document is on the screen, and the screen has to notice.
//
// **Off by default**, in every build. A card with no grey assets, or a build nobody
// switched, behaves exactly as it did before P2b.
namespace grey {

bool enabled();
void setEnabled(bool on);
// Returns the new state.
bool toggle();

// The base frame's refresh mode. HALF unless somebody asked for FAST.
HalDisplay::RefreshMode baseMode();
void setBaseMode(HalDisplay::RefreshMode mode);
const char* baseModeName();

// True once after anything changed the mode, so the screen on the panel can
// repaint itself instead of the rider having to leave and come back. Until it is
// consumed AND the frame is drawn, status()'s numbers describe the frame BEFORE the
// flip -- which is why status() publishes the pending flag too.
bool consumeRepaintRequest();

// One grey render attempt and how it ended, on every path out of
// GreyPageReader::render(). Was recordGrey(), which stored a per-attempt bool the
// next attempt erased; see WalletGreyStatus.h for what that cost.
void noteGreyAttempt(GreyOutcome outcome, const GreyTimings& timings);
// A BW frame went to the panel: the 1bpp page, a tile grid, a failure screen. The
// panel no longer carries grey, and the count of grey frames stands.
void noteBwFrame();

// Everything CMD:WALLETGREY status reports about grey frames. Read it, do not
// duplicate its fields.
const GreyLedger& status();

// The 1bpp comparison point, measured on the same page in the same session: the
// card read and the refresh that follows it.
void recordOneBpp(uint32_t cardUs, uint32_t refreshUs, uint32_t cardBytes);

uint32_t lastOneBppCardUs();
uint32_t lastOneBppRefreshUs();
uint32_t lastOneBppCardBytes();

}  // namespace grey

}  // namespace wallet
