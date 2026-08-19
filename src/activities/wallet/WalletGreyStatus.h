#pragma once

#include <cstdint>

// What a host can learn about a grey wallet frame, and nothing else: no panel, no
// card, no Arduino. Separate from WalletGreyPage.h so the semantics of every
// number `CMD:WALLETGREY status` prints are host-testable (test/wallet,
// `WalletGreyLedger*`).
//
// This file exists because of a defect. The status line used to carry one bool,
// `grey_rendered`, taken from the last render attempt's timings struct. It read as
// "a grey frame has been drawn", it was not that, and on 2026-08-18 it said 0 while
// the device's own log showed a 2,604 ms grey frame at the same moment. Two
// separate reasons, both structural:
//
//   * it was per-attempt. `render()` zeroes its timings on entry, so the next
//     attempt -- including a capability refusal that draws nothing -- erased the
//     record of the good frame before it;
//   * it was read too early. `CMD:WALLETGREY on` replies inside the serial handler
//     and the repaint it asks for happens in the next `WalletViewActivity::loop()`,
//     ~2.6 s later. The reply is emitted before the frame exists.
//
// So the ledger below keeps the two facts apart -- **has grey ever rendered**
// (monotonic, never cleared) and **does the panel carry grey right now** -- and
// publishes the pending repaint so a host can tell that the numbers it just read
// predate the frame it just asked for.
namespace wallet {

// Why a grey frame did not happen. Deliberately NOT wallet::Error: two of these
// are capability answers ("this panel cannot", "there was no heap"), and the right
// response to those is to draw the 1bpp version of the same page, not to put a
// message on the screen. greyOutcomeToError() (WalletGreyPage.h) maps the rest onto
// the errors the failure screen already knows how to say.
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

inline const char* greyOutcomeName(const GreyOutcome outcome) {
  switch (outcome) {
    case GreyOutcome::Ok:
      return "ok";
    case GreyOutcome::NotOpen:
      return "not_open";
    case GreyOutcome::NoFrameBuffer:
      return "no_framebuffer";
    case GreyOutcome::NoGreySupport:
      return "no_grey_support";
    case GreyOutcome::NoScratch:
      return "no_scratch";
    case GreyOutcome::Missing:
      return "missing";
    case GreyOutcome::BadAsset:
      return "bad_asset";
    case GreyOutcome::WrongPanel:
      return "wrong_panel";
    case GreyOutcome::Unaligned:
      return "unaligned";
    case GreyOutcome::OutOfRange:
      return "out_of_range";
    case GreyOutcome::Locked:
      return "locked";
    case GreyOutcome::ShortRead:
      return "short_read";
    case GreyOutcome::DecryptFailed:
      return "decrypt_failed";
  }
  return "?";
}

// True for the two outcomes that say "not here, not now" rather than "this card is
// wrong": the caller should quietly fall back to the 1bpp page instead of replacing
// the rider's document with an error.
inline constexpr bool greyOutcomeIsCapability(const GreyOutcome outcome) {
  return outcome == GreyOutcome::NoGreySupport || outcome == GreyOutcome::NoScratch;
}

// Per-stage cost of one grey frame, in microseconds, so the comparison against the
// 1bpp path is a measurement and not an estimate. Card time and waveform time are
// kept apart on purpose -- the plan predicted card time would be irrelevant next to
// the waveform, and this is what settles it (docs/wallet-grey.md, "What grey
// costs").
//
// No "did it render" flag lives here on purpose: that fact belongs to the ledger,
// which keeps the numbers of the last frame that actually finished instead of
// letting the next failed attempt zero them.
struct GreyTimings {
  uint32_t baseCardUs = 0;     // reading the base plane's window into the framebuffer
  uint32_t baseDisplayUs = 0;  // the base refresh (HALF by default)
  uint32_t planeCardUs = 0;    // reading both planes, band by band
  uint32_t planeWriteUs = 0;   // streaming those bands to controller RAM
  uint32_t nudgeUs = 0;        // displayGrayBuffer(): the grey waveform itself
  uint32_t cleanupUs = 0;      // RED resync + BW resync, not optional
  uint32_t totalUs = 0;
  uint32_t cardBytes = 0;  // how much came off the card for this frame
};

// Everything `CMD:WALLETGREY status` reports, and the only place the meaning of
// each field is decided.
//
// The distinction that matters, and the one the old bool could not make:
//
//   frames()       monotonic. "Has grey ever rendered on this device since boot?"
//                  Nothing clears it. This is the field a host asks.
//   onPanel()      "Is the picture on the glass right now a grey one?" Cleared by
//                  the next BW frame and by any failed attempt.
//
// What onPanel() cannot know: a reboot. E-ink keeps the picture and this flag does
// not survive, so a fresh boot in front of a grey page reports 0. Only a photograph
// or CMD:SCREENSHOT_GRAY can say what is on the glass after a restart.
class GreyLedger {
 public:
  // One grey render attempt and how it ended. Called on every return from
  // GreyPageReader::render(), success or failure.
  void noteAttempt(const GreyOutcome outcome, const GreyTimings& timings) {
    ++attempts_;
    lastOutcome_ = outcome;
    if (outcome == GreyOutcome::Ok) {
      ++frames_;
      onPanel_ = true;
      // Only a frame that finished gets to own the numbers. A failed attempt
      // leaves the last real measurement standing -- zeroing it was half of the
      // defect this class exists to fix.
      lastGrey_ = timings;
    } else {
      // Every non-Ok outcome is followed by the caller drawing something else: the
      // 1bpp page for a capability answer, the failure screen for a bad card. So
      // whatever grey the panel held is gone either way.
      onPanel_ = false;
    }
  }

  // A 1bpp page, a tile grid or a failure screen went to the panel. Grey is off the
  // glass; the count of grey frames stands.
  void noteBwFrame() { onPanel_ = false; }

  // A mode change wants the document on the panel repainted. Pending until the
  // frame it asks for has been drawn, which is a whole loop() and up to ~2.6 s
  // later -- so a status read in between is reading the frame BEFORE the flip.
  void requestRepaint() { repaintPending_ = true; }
  bool consumeRepaint() {
    const bool wanted = repaintPending_;
    repaintPending_ = false;
    return wanted;
  }
  bool repaintPending() const { return repaintPending_; }

  uint32_t frames() const { return frames_; }
  uint32_t attempts() const { return attempts_; }
  bool onPanel() const { return onPanel_; }

  // The last attempt's outcome, or "none" before anything has been attempted --
  // "ok" would be a lie there, and Ok is the enum's zero value.
  const char* lastOutcomeName() const { return attempts_ == 0 ? "none" : greyOutcomeName(lastOutcome_); }

  // The stages of the last grey frame that actually finished. All zeros while
  // frames() == 0, and a host must read frames() to know which it is looking at.
  const GreyTimings& lastGrey() const { return lastGrey_; }

 private:
  uint32_t frames_ = 0;
  uint32_t attempts_ = 0;
  bool onPanel_ = false;
  bool repaintPending_ = false;
  GreyOutcome lastOutcome_ = GreyOutcome::Ok;
  GreyTimings lastGrey_;
};

}  // namespace wallet
