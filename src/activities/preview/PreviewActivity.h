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
// | Levels | six patches off one black base, patch i nudged i times           |
//
// Marker moved cleanly over an intact grey backdrop on hardware (2026-08-05). A
// windowed BW update a button press later is fine; one issued inside the same
// render as the grey frame is not, and that is why nothing on these pages
// refreshes after the frame is done -- see docs/eink-grayscale.md, "An IMMEDIATE
// refresh after a grey frame breaks the panel".
//
// Marker, Drift and Region are the three open questions in that doc. What each
// page proves, and how to read it, is written on the page itself. Two records
// per page: CMD:SCREENSHOT_GRAY (tools/greyshot.py in the parent repo) for the
// exact levels the firmware asked for, and a photograph for what the panel did
// with them -- drift in particular is physical and cannot show up in a dump.
//
// Buttons: UP/DOWN page (that is where the hint labels sit), CONFIRM the page's
// action, LEFT/RIGHT re-render the page from scratch (unlabelled -- four hint
// slots, all taken), BACK home.
class PreviewActivity final : public Activity {
 public:
  explicit PreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Preview", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Scale, Marker, Drift, Region, Dither, Levels, Count };
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
  // Asks whether four levels is the panel's ceiling or just one batch's ceiling.
  // The Drift page measured that a repeated nudge keeps lightening the same
  // pixel, so a patch nudged k times should sit k steps off black. Six patches
  // from one base, patch i nudged i times, judged side by side -- the only way
  // to see whether the steps are even, where they saturate, and whether the
  // later ones go blotchy.
  void drawLevelsPage(const GrayPainter& painter) const;

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

  int markerStep_ = 0;  // Marker page: position along the route
  // Where the marker actually IS on the panel, which is not the same as the
  // previous logical step: presses can outrun the refresh, and then the step
  // before this one was never drawn. Tracking the drawn position is what makes
  // the window cover every marker still on the glass -- otherwise fast presses
  // leave ghosts behind (seen on hardware 2026-08-05).
  int markerDrawnStep_ = 0;
  int nudgeCount_ = 1;     // Drift page: how many times the right patch was nudged
  int regionStep_ = 0;     // Region page: which region the next nudge marks
  int levelsStep_ = 0;     // Levels page: how many nudges have been applied
  uint16_t windowMs_ = 0;  // last windowed update, measured
  GrayscaleFrame::Timings timings_{};
};
