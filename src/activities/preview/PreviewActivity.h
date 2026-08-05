#pragma once

#include <GrayscaleFrame.h>

#include <cstdint>

#include "activities/Activity.h"

// Hardware test bench for the four-level grey path -- the "grey cannot be
// captured by CMD:SCREENSHOT, so verification is visual" activity that
// docs/eink-grayscale.md asks for. Five pages, each one measurement:
//
// | Scale  | all four levels, as fills, text and lines of every width       |
// | Marker | a marker moved by a windowed BW update over a grey backdrop     |
// | Drift  | one patch nudged once vs one nudged again on every press        |
// | Region | a nudge that marks one region only, leaving the rest untouched   |
// | Dither | real grey next to the existing 2x2 checkerboard, same sizes     |
//
// Marker, Drift and Region are the three open questions in that doc. What each
// page proves, and how to read it, is written on the page itself: the panel is
// the output device, and a photograph of it is the record.
//
// Buttons: LEFT/RIGHT page, CONFIRM the page's action, UP/DOWN re-render the
// page from scratch, BACK home.
class PreviewActivity final : public Activity {
 public:
  explicit PreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Preview", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Scale, Marker, Drift, Region, Dither, Count };
  // What the next render() does. A page action is not always a full frame --
  // that is the whole point of the pages.
  enum class Update : uint8_t {
    Full,          // BW base + both planes + nudge + cleanup
    MarkerWindow,  // redraw the frame, refresh only the marker's window
    Nudge,         // planes only, no new base frame
    Counter        // windowed BW update of the counter line
  };
  // Which pass of which drawing the callback is serving. Nudge passes draw only
  // the region being nudged, so the rest of the panel keeps what it has.
  enum class DrawKind : uint8_t { Frame, NudgeRegion };

  static void drawTrampoline(void* ctx, const GrayPainter& painter);
  void draw(const GrayPainter& painter) const;

  void drawChrome(const GrayPainter& painter) const;
  void drawScalePage(const GrayPainter& painter) const;
  void drawMarkerPage(const GrayPainter& painter) const;
  void drawDriftPage(const GrayPainter& painter) const;
  void drawRegionPage(const GrayPainter& painter) const;
  void drawDitherPage(const GrayPainter& painter) const;

  void drawMarker(const GrayPainter& painter, int step) const;
  void markerRect(int step, int& x, int& y, int& w, int& h) const;
  void counterRect(int& x, int& y, int& w, int& h) const;
  void drawCounterLine();

  GrayDrawCallback callback() { return GrayDrawCallback{this, &drawTrampoline}; }

  int contentTop() const;
  int contentBottom() const;

  Page page_ = Page::Scale;
  Update update_ = Update::Full;
  DrawKind drawKind_ = DrawKind::Frame;

  int markerStep_ = 0;      // Marker page: position along the route
  int markerPrevStep_ = 0;  // where it was, so the window covers both
  int nudgeCount_ = 1;      // Drift page: how many times the right patch was nudged
  int regionStep_ = 0;      // Region page: which region the next nudge marks
  uint16_t windowMs_ = 0;   // last windowed update, measured
  GrayscaleFrame::Timings timings_{};
};
