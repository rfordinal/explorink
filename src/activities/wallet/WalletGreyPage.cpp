#include "WalletGreyPage.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <GrayscaleFrame.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "WalletCryptoDevice.h"

namespace wallet {

namespace {

constexpr const char* kLogTag = "WALLETGREY";

// The same band height the firmware's own grey path uses, taken from it rather
// than repeated: 80 physical rows x 100 bytes = 8,000 bytes of scratch on the X4.
// The RLE sidecar's band size is the same number for the same reason (parent repo
// docs/wallet-format.md, section 7).
constexpr int kBandRows = GrayscaleFrame::STRIP_ROWS;

}  // namespace

Error greyOutcomeToError(const GreyOutcome outcome) {
  switch (outcome) {
    case GreyOutcome::Ok:
      return Error::None;
    case GreyOutcome::Missing:
      return Error::NoAsset;
    case GreyOutcome::WrongPanel:
      return Error::AssetWrongSize;
    case GreyOutcome::Locked:
      return Error::Locked;
    case GreyOutcome::ShortRead:
      return Error::ShortRead;
    case GreyOutcome::DecryptFailed:
      return Error::AssetDecrypt;
    case GreyOutcome::NoFrameBuffer:
      return Error::NoFrameBuffer;
    case GreyOutcome::NoGreySupport:
    case GreyOutcome::NoScratch:
      // Never shown: the caller falls back to the 1bpp page, which is the same
      // document. Mapped to None so a screen that ignored that contract shows the
      // list's own state rather than a sentence about a panel feature.
      return Error::None;
    case GreyOutcome::NotOpen:
    case GreyOutcome::Unaligned:
    case GreyOutcome::OutOfRange:
    case GreyOutcome::BadAsset:
      return Error::BadAsset;
  }
  return Error::BadAsset;
}

GreyOutcome GreyPageReader::open(const PageImageSpec& page, const GfxRenderer& renderer) {
  if (!page.present || !isValidAssetId(page.assetId)) return GreyOutcome::BadAsset;
  // Already the right file: an A/B toggle or a pan must not pay for an open.
  if (open_ && std::strcmp(spec_.assetId, page.assetId) == 0) {
    spec_ = page;
    return GreyOutcome::Ok;
  }
  close();

  char path[kAssetPathBufBytes];
  if (!buildAssetPath(page.assetId, path, sizeof(path))) return GreyOutcome::BadAsset;
  if (!Storage.openFileForRead(kLogTag, path, file_)) {
    LOG_ERR(kLogTag, "no grey plane asset %s", path);
    return GreyOutcome::Missing;
  }

  uint8_t raw[kAssetHeaderBytes];
  if (file_.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
    LOG_ERR(kLogTag, "short grey header %s", path);
    file_.close();
    return GreyOutcome::BadAsset;
  }

  const PanelGeometry live = livePanel(renderer);
  // A key in hand is what lets an encrypted asset through the gate. Exactly the
  // same rule as every other asset: grey is not a second crypto path, it is more
  // rows of the same one (docs/wallet-crypto.md).
  const bool haveKey = Session::instance().key() != nullptr;
  const AssetCheck check = checkGreyPlanes(raw, sizeof(raw), page, live, header_, haveKey);
  if (check != AssetCheck::Ok) {
    LOG_ERR(kLogTag, "grey asset %s refused: type %u depth %u %ux%u rawLen %lu vs manifest %ux%u/%u B row, rawLen %lu",
            path, static_cast<unsigned>(header_.assetType), static_cast<unsigned>(header_.bitDepth),
            static_cast<unsigned>(header_.width), static_cast<unsigned>(header_.height),
            static_cast<unsigned long>(header_.rawLen), static_cast<unsigned>(page.nativeWidth),
            static_cast<unsigned>(page.nativeHeight), static_cast<unsigned>(page.rowBytes),
            static_cast<unsigned long>(page.rawLen));
    file_.close();
    switch (check) {
      case AssetCheck::Encrypted:
        return GreyOutcome::Locked;
      case AssetCheck::WrongPanel:
        return GreyOutcome::WrongPanel;
      default:
        return GreyOutcome::BadAsset;
    }
  }

  encrypted_ = (header_.flags & kFlagEncrypted) != 0;
  if (encrypted_ && !buildAssetIv(page.assetId, header_.version, iv_)) {
    LOG_ERR(kLogTag, "grey asset %s has no usable IV", page.assetId);
    file_.close();
    return GreyOutcome::BadAsset;
  }
  spec_ = page;
  open_ = true;
  LOG_INF(kLogTag, "grey planes open: %s %ux%u, %u B/row, %lu B/plane, step %u,%u%s", page.assetId,
          static_cast<unsigned>(page.nativeWidth), static_cast<unsigned>(page.nativeHeight),
          static_cast<unsigned>(page.rowBytes), static_cast<unsigned long>(greyPlaneBytes(page)),
          static_cast<unsigned>(page.windowStepX), static_cast<unsigned>(page.windowStepY),
          encrypted_ ? ", encrypted" : "");
  return GreyOutcome::Ok;
}

void GreyPageReader::close() {
  // Before the file goes: a producer that cannot read is worse than no producer.
  releaseReplaySource();
  if (open_ || file_.isOpen()) file_.close();
  open_ = false;
  spec_ = PageImageSpec{};
  header_ = AssetHeader{};
  encrypted_ = false;
  secureWipe(iv_, sizeof(iv_));
}

void GreyPageReader::releaseReplaySource() {
  GrayscaleFrame::clearPlaneSource(this);
  replayArmed_ = false;
}

bool GreyPageReader::planeBandThunk(void* const ctx, const bool lsbPlane, uint8_t* const rows, const int yStart,
                                    const int numRows) {
  return static_cast<GreyPageReader*>(ctx)->fillPlaneBand(lsbPlane, rows, yStart, numRows);
}

bool GreyPageReader::fillPlaneBand(const bool lsbPlane, uint8_t* const rows, const int yStart, const int numRows) {
  if (!open_ || !replayArmed_ || rows == nullptr || numRows <= 0 || yStart < 0) return false;
  const uint32_t y = replayY_ + static_cast<uint32_t>(yStart);
  if (y + static_cast<uint32_t>(numRows) > spec_.nativeHeight) return false;
  // The same call the panel path makes, with the same window and the same stride.
  // That is the whole argument for a producer over a buffer: there is no second
  // implementation to drift.
  const GreyOutcome outcome = readPlaneRows(lsbPlane ? GreyPlane::Lsb : GreyPlane::Msb, replayXByte_, y,
                                            static_cast<uint32_t>(numRows), rows, replayRowBytes_);
  if (outcome != GreyOutcome::Ok) {
    LOG_ERR(kLogTag, "grey capture band y=%lu refused: %s", static_cast<unsigned long>(y), greyOutcomeName(outcome));
    return false;
  }
  return true;
}

GreyOutcome GreyPageReader::readPlaneRows(const GreyPlane plane, const uint32_t xByte, const uint32_t y,
                                          const uint32_t rows, uint8_t* const dest, const uint32_t destRowBytes) {
  const uint8_t* const key = encrypted_ ? Session::instance().key() : nullptr;
  if (encrypted_ && key == nullptr) return GreyOutcome::Locked;
  Error error = Error::None;
  if (!readPlaneWindow(file_, key, iv_, greyPlaneOffset(spec_, plane), spec_.rowBytes, xByte, y, rows, dest,
                       destRowBytes, error)) {
    return error == Error::AssetDecrypt ? GreyOutcome::DecryptFailed : GreyOutcome::ShortRead;
  }
  return GreyOutcome::Ok;
}

GreyOutcome GreyPageReader::render(const uint32_t x, const uint32_t y, GfxRenderer& renderer,
                                   const HalDisplay::RefreshMode baseMode, GreyTimings& timings) {
  timings = GreyTimings{};
  // A new frame starts: whatever window the capture path was armed for is history
  // from here on, whether this attempt succeeds or not.
  releaseReplaySource();
  if (!open_) return GreyOutcome::NotOpen;
  if (!renderer.hasFrameBuffer()) return GreyOutcome::NoFrameBuffer;
  // Asked before anything is drawn, because the answer decides whether the caller
  // draws the 1bpp page instead. Drawing the base frame on a panel that cannot
  // nudge would put a page on the glass where every grey reads BLACK -- grey
  // shares the base frame's ink (docs/eink-grayscale.md).
  if (!renderer.supportsStripGrayscale()) return GreyOutcome::NoGreySupport;
  if ((x % 8) != 0) {
    LOG_ERR(kLogTag, "window x=%lu is not 8-aligned", static_cast<unsigned long>(x));
    return GreyOutcome::Unaligned;
  }

  const uint32_t rowBytes = renderer.getDisplayWidthBytes();
  const uint32_t rows = renderer.getDisplayHeight();
  const uint32_t xByte = x / 8;
  if (xByte + rowBytes > spec_.rowBytes || y + rows > spec_.nativeHeight) {
    LOG_ERR(kLogTag, "grey window %lu,%lu does not fit %ux%u", static_cast<unsigned long>(x),
            static_cast<unsigned long>(y), static_cast<unsigned>(spec_.nativeWidth),
            static_cast<unsigned>(spec_.nativeHeight));
    return GreyOutcome::OutOfRange;
  }

  // 8 KB, and it must exist before the base frame goes up: a base frame with no
  // planes behind it is a page whose greys are black, which is worse than the 1bpp
  // page the caller would otherwise draw. So the allocation is the first thing that
  // can fail and the last thing before the panel is touched.
  const size_t scratchBytes = static_cast<size_t>(rowBytes) * kBandRows;
  auto scratch = makeUniqueNoThrow<uint8_t[]>(scratchBytes);
  if (!scratch) {
    LOG_ERR(kLogTag, "OOM: %u bytes for the grey plane band", static_cast<unsigned>(scratchBytes));
    return GreyOutcome::NoScratch;
  }

  const uint32_t tStart = micros();

  // 1. The base plane, straight into the framebuffer. Same window, same row maths
  // as the 1bpp page -- it *is* a 1bpp page, the one where black and both greys
  // are ink.
  const GreyOutcome base = readPlaneRows(GreyPlane::Base, xByte, y, rows, renderer.getFrameBuffer(), rowBytes);
  timings.baseCardUs = micros() - tStart;
  timings.cardBytes += rowBytes * rows;
  if (base != GreyOutcome::Ok) {
    // Nothing has been written to the controller yet, and the framebuffer holds a
    // half-read frame the caller is about to replace with a failure screen.
    return base;
  }

  const uint32_t tBaseRead = micros();
  renderer.displayGrayscaleBase(baseMode);
  const uint32_t tBaseShown = micros();
  timings.baseDisplayUs = tBaseShown - tBaseRead;

  // No-op on the X4, a real OEM settle on the X3 (PanelDriver.h:117-119).
  renderer.preconditionGrayscale();
  // The strip writes below talk to the controller directly, so nothing may be in
  // flight. GrayscaleFrame::writePlanes does the same, for the same reason.
  renderer.waitRefreshComplete();

  // 2. Both planes, band by band, card straight to controller RAM. This is the
  // whole reason the generator bakes them: there is no picture to draw here, only
  // 8,000 bytes to move twelve times.
  GreyOutcome planes = GreyOutcome::Ok;
  for (uint8_t p = 0; p < 2 && planes == GreyOutcome::Ok; ++p) {
    const GreyPlane plane = p == 0 ? GreyPlane::Lsb : GreyPlane::Msb;
    for (uint32_t band = 0; band < rows; band += kBandRows) {
      const uint32_t bandRows = (rows - band < static_cast<uint32_t>(kBandRows)) ? rows - band : kBandRows;
      const uint32_t tRead = micros();
      planes = readPlaneRows(plane, xByte, y + band, bandRows, scratch.get(), rowBytes);
      const uint32_t tWrite = micros();
      timings.planeCardUs += tWrite - tRead;
      timings.cardBytes += rowBytes * bandRows;
      if (planes != GreyOutcome::Ok) break;
      renderer.writeGrayscalePlaneStrip(plane == GreyPlane::Lsb, scratch.get(), static_cast<int>(band),
                                        static_cast<int>(bandRows));
      timings.planeWriteUs += micros() - tWrite;
    }
  }

  if (planes != GreyOutcome::Ok) {
    // Plane bits are in controller RAM and the base frame is on the glass. The
    // cleanup is not optional even here -- especially here: BW RAM holds plane bits
    // now, and the next refresh would diff them against the frame and drive nearly
    // every pixel (measured 2026-08-05, docs/eink-grayscale.md, "The window is the
    // diff"). No nudge is fired, so the panel keeps the base frame, where grey
    // reads black; the caller replaces it with a failure screen at HALF.
    const uint32_t tCleanup = micros();
    renderer.cleanupGrayscaleWithFrameBuffer();
    renderer.resyncControllerBwRam();
    timings.cleanupUs = micros() - tCleanup;
    timings.totalUs = micros() - tStart;
    return planes;
  }

  // 3. The nudge. Short waveform: three active frame groups against the HALF
  // base's dozens (Ssd1677Luts.h:24-26).
  const uint32_t tPlanesDone = micros();
  renderer.displayGrayBuffer();
  const uint32_t tGrey = micros();
  timings.nudgeUs = tGrey - tPlanesDone;

  // 4. The cleanup, which is not optional and is not the caller's business.
  renderer.cleanupGrayscaleWithFrameBuffer();
  renderer.resyncControllerBwRam();
  timings.cleanupUs = micros() - tGrey;

  timings.totalUs = micros() - tStart;

  // The panel now carries these exact plane bytes, and nothing kept them: the band
  // scratch is about to be freed. Arm the capture path to read them again on demand
  // so CMD:SCREENSHOT_GRAY can answer for a wallet page at all (docs/wallet-grey.md,
  // "What a host can see of a grey frame"). Eight bytes of registration, not a
  // 96,000 byte shadow.
  replayXByte_ = xByte;
  replayY_ = y;
  replayRowBytes_ = rowBytes;
  replayArmed_ = true;
  GrayscaleFrame::setPlaneSource(GrayPlaneSource{this, &GreyPageReader::planeBandThunk});
  return GreyOutcome::Ok;
}

// --- The A/B switch --------------------------------------------------------

namespace grey {

namespace {

bool enabled_ = false;
HalDisplay::RefreshMode baseMode_ = HalDisplay::HALF_REFRESH;
GreyLedger ledger_;
uint32_t lastOneBppCardUs_ = 0;
uint32_t lastOneBppRefreshUs_ = 0;
uint32_t lastOneBppCardBytes_ = 0;

}  // namespace

bool enabled() { return enabled_; }

void setEnabled(const bool on) {
  if (enabled_ != on) ledger_.requestRepaint();
  enabled_ = on;
}

bool toggle() {
  setEnabled(!enabled_);
  return enabled_;
}

HalDisplay::RefreshMode baseMode() { return baseMode_; }

void setBaseMode(const HalDisplay::RefreshMode mode) {
  if (baseMode_ != mode) ledger_.requestRepaint();
  baseMode_ = mode;
}

const char* baseModeName() { return baseMode_ == HalDisplay::FAST_REFRESH ? "fast" : "half"; }

bool consumeRepaintRequest() { return ledger_.consumeRepaint(); }

void noteGreyAttempt(const GreyOutcome outcome, const GreyTimings& timings) { ledger_.noteAttempt(outcome, timings); }

void noteBwFrame() { ledger_.noteBwFrame(); }

const GreyLedger& status() { return ledger_; }

void recordOneBpp(const uint32_t cardUs, const uint32_t refreshUs, const uint32_t cardBytes) {
  lastOneBppCardUs_ = cardUs;
  lastOneBppRefreshUs_ = refreshUs;
  lastOneBppCardBytes_ = cardBytes;
}

uint32_t lastOneBppCardUs() { return lastOneBppCardUs_; }
uint32_t lastOneBppRefreshUs() { return lastOneBppRefreshUs_; }
uint32_t lastOneBppCardBytes() { return lastOneBppCardBytes_; }

}  // namespace grey

}  // namespace wallet
