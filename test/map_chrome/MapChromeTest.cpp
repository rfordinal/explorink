// The chrome register (MapChrome.h) and the two placers that ask it.
//
// The register itself is arithmetic, so it is tested directly. The placers are
// tested through a canvas whose areaReserved() is the register -- the same
// seam GfxRendererCanvas uses on the device -- because what matters is not that
// the register answers correctly but that a label and a POI mark actually
// change what they draw when it does.
//
// Nothing here knows what a compass or a button box is. Where the rectangles
// come from is MapActivity::buildChromeRegister()'s job and needs a real theme
// and a real panel; what they mean is this file's job.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "MapChrome.h"
#include "MapLabels.h"
#include "MapPointMarks.h"
#include "MapStyleDefaults.h"

namespace {

constexpr int kScreenW = 480;
constexpr int kScreenH = 800;
constexpr int kCharPx = 10;

// Enough of a canvas to see what was asked for and where. Text is measured
// arithmetically, same as MapLabelsTest's fake, so an expectation here is never
// about a font's metrics.
class FakeCanvas : public IMapCanvas {
 public:
  struct Box {
    int x, y, w, h;
    MapInk ink;
  };
  struct Text {
    int x, y;
    std::string text;
    MapInk ink;
  };

  void drawLine(int x1, int y1, int x2, int y2, int, MapInk ink) override { lines.push_back({x1, y1, x2, y2, ink}); }
  void fillRoundedRect(int x, int y, int width, int height, int, MapInk ink) override {
    boxes.push_back({x, y, width, height, ink});
  }
  void fillPolygon(const int*, const int*, int, MapInk) override {}
  void fillSpan(int x1, int x2, int y, MapAreaTone) override { spans.push_back({x1, x2, y}); }

  bool measureText(const char* utf8, int sizePx, bool, int& outWidth, int& outHeight) override {
    outWidth = static_cast<int>(std::string(utf8 == nullptr ? "" : utf8).size()) * kCharPx;
    outHeight = sizePx;
    return outWidth > 0 && outHeight > 0;
  }

  void drawText(int x, int y, const char* utf8, int, bool, MapInk ink) override {
    texts.push_back({x, y, utf8 == nullptr ? "" : utf8, ink});
  }

  bool drawTextRotated(int, int, const char*, int, bool, MapInk, int, int, int, int, int) override { return true; }

  void drawableRect(int& outX, int& outY, int& outWidth, int& outHeight) const override {
    outX = 0;
    outY = 0;
    outWidth = kScreenW;
    outHeight = kScreenH;
  }

  bool areaReserved(int x, int y, int width, int height) const override { return chrome.hits(x, y, width, height); }

  std::vector<Text> blackTexts() const {
    std::vector<Text> out;
    for (const Text& text : texts) {
      if (text.ink == MapInk::Black) out.push_back(text);
    }
    return out;
  }

  struct Line {
    int x1, y1, x2, y2;
    MapInk ink;
  };
  struct Span {
    int x1, x2, y;
  };

  MapChromeRegister chrome;
  std::vector<Box> boxes;
  std::vector<Text> texts;
  std::vector<Line> lines;
  std::vector<Span> spans;
};

MapStyle labelStyle() {
  MapStyle style = kDefaultMapStyle;
  style.placeLabelPx = 20;
  style.placeLabelBold = true;
  style.placeLabelMinorPx = 16;
  style.placeLabelMinorBold = false;
  style.placeLabelOffsetPx = 6;
  style.placeLabelBg = false;
  style.placeLabelHaloPx = 2;
  style.placeMaxLabels = 6;
  style.placeLabelGapPx = 4;
  style.placeLabelRouteOverlapPct = 8;
  style.placeLabelMaxWidthPx = 0;
  style.placeDotDiameterPx = 10;
  // Far from every case below, so the puck's own exclusion box is never what
  // rejected a placement.
  style.markerXPx = 20;
  style.markerYPx = 780;
  style.puckRadiusPx = 10;
  style.puckRingPx = 2;
  style.puckArrowPx = 10;
  return style;
}

MapStyle pointStyle() {
  MapStyle style = kDefaultMapStyle;
  style.pointsSafetyEnabled = true;
  style.pointSquarePx = 22;
  style.pointBorderPx = 1;
  style.pointGlyphPx = 12;
  style.pointFlagPx = 6;
  // One mark per point: clustering would move the mark to a centroid and the
  // assertions below are about one square at one place.
  style.pointClusterRadiusPx = 0;
  return style;
}

MapPlaceRef place(const int x, const int y, const uint8_t rank, const char* name) {
  MapPlaceRef out;
  out.x = static_cast<int16_t>(x);
  out.y = static_cast<int16_t>(y);
  out.rank = rank;
  out.name = name;
  return out;
}

MapPointRef point(const int x, const int y) {
  MapPointRef out;
  out.x = x;
  out.y = y;
  out.kind = MapPointKind::Safety;
  out.category = 1;  // water, which has an icon
  out.flags = 0;
  return out;
}

// Did anything at all get inked inside this box?
bool anyInkIn(const FakeCanvas& canvas, const int x, const int y, const int w, const int h) {
  const auto inside = [&](const int px, const int py) { return px >= x && px < x + w && py >= y && py < y + h; };
  for (const auto& box : canvas.boxes) {
    if (inside(box.x, box.y)) return true;
  }
  for (const auto& span : canvas.spans) {
    if (span.y >= y && span.y < y + h && span.x2 >= x && span.x1 < x + w) return true;
  }
  for (const auto& line : canvas.lines) {
    if (inside(line.x1, line.y1) || inside(line.x2, line.y2)) return true;
  }
  for (const auto& text : canvas.texts) {
    if (inside(text.x, text.y)) return true;
  }
  return false;
}

}  // namespace

TEST(MapChromeRegisterTest, EmptyRegisterReservesNothing) {
  MapChromeRegister chrome;
  EXPECT_FALSE(chrome.hits(0, 0, kScreenW, kScreenH));
  EXPECT_EQ(chrome.count(), 0u);
  EXPECT_EQ(chrome.dropped(), 0u);
}

TEST(MapChromeRegisterTest, HitsOnlyWhatOverlaps) {
  MapChromeRegister chrome;
  chrome.add(100, 100, 50, 50, MapChromeTag::Compass);

  EXPECT_TRUE(chrome.hits(120, 120, 10, 10));     // wholly inside
  EXPECT_TRUE(chrome.hits(90, 90, 20, 20));       // corner overlap
  EXPECT_TRUE(chrome.hits(0, 0, kScreenW, 200));  // swallows it
  EXPECT_FALSE(chrome.hits(0, 0, 50, 50));        // clear of it
}

TEST(MapChromeRegisterTest, TouchingEdgesDoNotCount) {
  MapChromeRegister chrome;
  chrome.add(100, 100, 50, 50, MapChromeTag::Button);
  // Half-open on both axes: a box that ends exactly where the button starts
  // shares no pixel with it, and refusing it would cost a column for nothing.
  EXPECT_FALSE(chrome.hits(50, 100, 50, 50));
  EXPECT_FALSE(chrome.hits(150, 100, 50, 50));
  EXPECT_FALSE(chrome.hits(100, 50, 50, 50));
  EXPECT_FALSE(chrome.hits(100, 150, 50, 50));
  EXPECT_TRUE(chrome.hits(149, 100, 50, 50));
}

TEST(MapChromeRegisterTest, EmptyRectanglesAreNotStored) {
  MapChromeRegister chrome;
  // What a theme answers for a box it does not draw. Storing it would make
  // every query walk an entry that can never match.
  EXPECT_TRUE(chrome.add(10, 10, 0, 40, MapChromeTag::Button));
  EXPECT_TRUE(chrome.add(10, 10, 40, 0, MapChromeTag::Button));
  EXPECT_EQ(chrome.count(), 0u);
  EXPECT_EQ(chrome.dropped(), 0u);
}

TEST(MapChromeRegisterTest, OverflowIsCountedRatherThanSilent) {
  MapChromeRegister chrome;
  for (size_t i = 0; i < MapChromeRegister::kMaxEntries; ++i) {
    EXPECT_TRUE(chrome.add(static_cast<int>(i) * 2, 0, 1, 1, MapChromeTag::Notice));
  }
  EXPECT_FALSE(chrome.add(400, 400, 10, 10, MapChromeTag::Notice));
  EXPECT_EQ(chrome.count(), MapChromeRegister::kMaxEntries);
  EXPECT_EQ(chrome.dropped(), 1u);
  // The dropped one is genuinely not reserved -- which is why the caller logs
  // the count instead of trusting the register to have grown.
  EXPECT_FALSE(chrome.hits(400, 400, 10, 10));
}

TEST(MapChromeRegisterTest, ClearForgetsBothTheEntriesAndTheDrops) {
  MapChromeRegister chrome;
  for (size_t i = 0; i < MapChromeRegister::kMaxEntries + 2; ++i) {
    chrome.add(static_cast<int>(i) * 2, 0, 1, 1, MapChromeTag::Notice);
  }
  ASSERT_GT(chrome.dropped(), 0u);
  chrome.clear();
  EXPECT_EQ(chrome.count(), 0u);
  EXPECT_EQ(chrome.dropped(), 0u);
  EXPECT_FALSE(chrome.hits(0, 0, kScreenW, kScreenH));
}

TEST(MapChromeLabels, ANameMovesToItsOtherSideRatherThanBeingLost) {
  FakeCanvas canvas;
  // A band to the right of the dot, where the first placement would go.
  canvas.chrome.add(200, 300, 200, 100, MapChromeTag::DebugWindow);

  MapLabelScratch scratch;
  MapLabels::offer(scratch, place(200, 350, 2, "Limbach"), 20, 780);
  MapLabels::draw(canvas, scratch, labelStyle());

  const auto drawn = canvas.blackTexts();
  ASSERT_EQ(drawn.size(), 1u);
  EXPECT_EQ(drawn[0].text, "Limbach");
  // Left of the dot is the second position tried, and it is clear.
  EXPECT_LT(drawn[0].x, 200);
  EXPECT_EQ(scratch.placed, 1);
  EXPECT_EQ(scratch.dropped, 0);
}

TEST(MapChromeLabels, ANameWithEveryPositionReservedIsDropped) {
  FakeCanvas canvas;
  // The whole neighbourhood of the dot, so none of the eight placements clears
  // it. A dropped name is the right outcome: half a name is a different name.
  canvas.chrome.add(0, 200, kScreenW, 300, MapChromeTag::Header);

  MapLabelScratch scratch;
  MapLabels::offer(scratch, place(200, 350, 2, "Limbach"), 20, 780);
  MapLabels::draw(canvas, scratch, labelStyle());

  EXPECT_TRUE(canvas.blackTexts().empty());
  EXPECT_EQ(scratch.placed, 0);
  EXPECT_EQ(scratch.dropped, 1);
}

TEST(MapChromePointMarks, AMarkUnderFurnitureIsNotDrawn) {
  const MapStyle style = pointStyle();

  FakeCanvas clear;
  MapPointMarks::draw(clear, point(240, 400), style);
  ASSERT_TRUE(anyInkIn(clear, 240 - style.pointSquarePx, 400 - style.pointSquarePx, style.pointSquarePx * 2,
                       style.pointSquarePx * 2))
      << "the control case must draw, or the reserved case proves nothing";

  FakeCanvas reserved;
  reserved.chrome.add(230, 390, 40, 40, MapChromeTag::ScaleBar);
  MapPointMarks::draw(reserved, point(240, 400), style);
  EXPECT_TRUE(reserved.boxes.empty());
  EXPECT_TRUE(reserved.spans.empty());
  EXPECT_TRUE(reserved.lines.empty());
}

TEST(MapChromePointMarks, AMarkBesideFurnitureStillDraws) {
  const MapStyle style = pointStyle();
  FakeCanvas canvas;
  // Reserved band well clear of the mark's own square.
  canvas.chrome.add(0, 0, kScreenW, 100, MapChromeTag::Header);
  MapPointMarks::draw(canvas, point(240, 400), style);
  EXPECT_TRUE(anyInkIn(canvas, 240 - style.pointSquarePx, 400 - style.pointSquarePx, style.pointSquarePx * 2,
                       style.pointSquarePx * 2));
}
