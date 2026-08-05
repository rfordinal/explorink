#pragma once

#include <GrayscaleFrame.h>

#include <cstdint>

#include "activities/Activity.h"

// Hardware test bench for the four-level grey path, the test activity
// docs/eink-grayscale.md asks for. Five pages, each one measurement:
//
// | Scale  | all four levels, as fills, text and lines of every width       |
// | Marker | a marker moved by a windowed BW update over a grey backdrop     |
// | Drift  | one patch nudged once vs one nudged again on every press        |
// | Region | a nudge that marks one region only, leaving the rest untouched   |
// | Dither | real grey next to the existing 2x2 checkerboard, same sizes     |
//
// Marker's answer came out negative and the page is kept for it: a windowed BW
// update over grey does NOT preserve the grey. Measured 2026-08-05 -- any refresh
// after a grey frame drives every pixel to its RAM value, and a grey pixel's RAM
// value is ink, so the whole backdrop goes black. Moving something over a grey
// base costs a full grey re-render, ~2.1 s.
//
// Marker, Drift and Region are the three open questions in that doc. What each
// page proves, and how to read it, is written on the page itself. Two records
// per page: CMD:SCREENSHOT_GRAY (tools/greyshot.py in the parent repo) for the
// exact levels the firmware asked for, and a photograph for what the panel did
// with them -- drift in particular is physical and cannot show up in a dump.
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
    Nudge          // planes only, no new base frame
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

  // Draws one line of page text, trimmed to the screen width. A hint that runs
  // off the right edge is not just ugly: every clipped pixel logs an "Outside
  // range" error, and during CMD:SCREENSHOT_GRAY those errors land inside the
  // binary payload (found on hardware 2026-08-05, before SerialLogMute existed).
  void note(const GrayPainter& painter, int x, int y, const char* text, GrayShade shade) const;

  void drawMarker(const GrayPainter& painter, int step) const;
  void markerRect(int step, int& x, int& y, int& w, int& h) const;
  void counterRect(int& x, int& y, int& w, int& h) const;

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
