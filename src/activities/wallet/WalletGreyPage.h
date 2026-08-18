#pragma once

#include <HalDisplay.h>
#include <HalStorage.h>

#include <cstdint>

#include "WalletAsset.h"
#include "WalletCrypto.h"
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

// Why a grey frame did not happen. Deliberately NOT wallet::Error: two of these
// are capability answers ("this panel cannot", "there was no heap"), and the right
// response to those is to draw the 1bpp version of the same page, not to put a
// message on the screen. greyOutcomeToError() maps the rest onto the errors the
// failure screen already knows how to say.
enum class GreyOutcome : uint8_t {
  Ok = 0,
  NotOpen,        // render() before a successful open()
  NoFrameBuffer,  // the framebuffer is lent out
  NoGreySupport,  // supportsStripGrayscale() is false: X3 inverted, or another panel
  NoScratch,      // no heap for the 8 KB plane band -- nothing was drawn
  Missing,        // no file behind the manifest's assetId
  BadAsset,       // magic, bit depth, type, or a header that disagrees with the manifest
  WrongPanel,     // the page is smaller than this panel, so no window can fill it
  Unaligned,      // a window x that is not a multiple of 8 -- a caller bug, refused
  OutOfRange,     // the window does not fit the page
  Locked,         // encrypted and no key held
  ShortRead,      // a plane row ended early
  DecryptFailed,  // CTR refused
};

const char* greyOutcomeName(GreyOutcome outcome);

// True for the two outcomes that say "not here, not now" rather than "this card is
// wrong": the caller should quietly fall back to the 1bpp page instead of replacing
// the rider's document with an error.
inline bool greyOutcomeIsCapability(const GreyOutcome outcome) {
  return outcome == GreyOutcome::NoGreySupport || outcome == GreyOutcome::NoScratch;
}

Error greyOutcomeToError(GreyOutcome outcome);

// Per-stage cost of one grey frame, in microseconds, so the comparison against the
// 1bpp path is a measurement and not an estimate. Card time and waveform time are
// kept apart on purpose -- the plan predicted card time would be irrelevant next to
// the waveform, and this is what settles it (docs/wallet-grey.md, "What grey
// costs").
struct GreyTimings {
  uint32_t baseCardUs = 0;     // reading the base plane's window into the framebuffer
  uint32_t baseDisplayUs = 0;  // the base refresh (HALF by default)
  uint32_t planeCardUs = 0;    // reading both planes, band by band
  uint32_t planeWriteUs = 0;   // streaming those bands to controller RAM
  uint32_t nudgeUs = 0;        // displayGrayBuffer(): the grey waveform itself
  uint32_t cleanupUs = 0;      // RED resync + BW resync, not optional
  uint32_t totalUs = 0;
  uint32_t cardBytes = 0;  // how much came off the card for this frame
  bool rendered = false;   // false = the panel does not carry a grey frame
};

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

 private:
  GreyOutcome readPlaneRows(GreyPlane plane, uint32_t xByte, uint32_t y, uint32_t rows, uint8_t* dest,
                            uint32_t destRowBytes);

  HalFile file_;
  PageImageSpec spec_;
  AssetHeader header_;
  bool open_ = false;
  bool encrypted_ = false;
  uint8_t iv_[kAssetIvLen] = {0};
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
// repaint itself instead of the rider having to leave and come back.
bool consumeRepaintRequest();

void recordGrey(const GreyTimings& timings);
// The 1bpp comparison point, measured on the same page in the same session: the
// card read and the refresh that follows it.
void recordOneBpp(uint32_t cardUs, uint32_t refreshUs, uint32_t cardBytes);

const GreyTimings& lastGrey();
uint32_t lastOneBppCardUs();
uint32_t lastOneBppRefreshUs();
uint32_t lastOneBppCardBytes();

}  // namespace grey

}  // namespace wallet
