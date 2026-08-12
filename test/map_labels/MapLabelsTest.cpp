// What the place-name placer promises, tested without a font or a tile.
//
// The canvas here measures text arithmetically (a fixed width per character),
// so every expectation below is about the placement rules and never about
// Noto Sans's metrics. The real font is exercised by the preview binary
// instead -- see test/map_preview and docs/place-labels.md.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "MapLabels.h"
#include "MapStyleDefaults.h"

namespace {

constexpr int kScreenW = 480;
constexpr int kScreenH = 800;
constexpr int kCharPx = 10;

struct DrawnText {
  int x;
  int y;
  std::string text;
  int sizePx;
  bool bold;
  MapInk ink;
};

class FakeCanvas : public IMapCanvas {
 public:
  void drawLine(int, int, int, int, int, MapInk) override {}
  void fillRoundedRect(int x, int y, int width, int height, int, MapInk ink) override {
    boxes.push_back({x, y, width, height, ink});
  }
  void fillPolygon(const int*, const int*, int, MapInk) override {}
  void fillSpan(int, int, int, MapAreaTone) override {}

  bool measureText(const char* utf8, int sizePx, bool, int& outWidth, int& outHeight) override {
    if (!hasText) return false;
    // One char per byte is fine: every name in this file is ASCII except the
    // truncation case, which counts bytes on purpose.
    outWidth = static_cast<int>(std::string(utf8 == nullptr ? "" : utf8).size()) * kCharPx;
    outHeight = sizePx;
    return outWidth > 0 && outHeight > 0;
  }

  void drawText(int x, int y, const char* utf8, int sizePx, bool bold, MapInk ink) override {
    texts.push_back({x, y, utf8 == nullptr ? "" : utf8, sizePx, bold, ink});
  }

  void drawableRect(int& outX, int& outY, int& outWidth, int& outHeight) const override {
    outX = 0;
    outY = topY;
    outWidth = kScreenW;
    outHeight = kScreenH - topY;
  }

  // Black label text only -- the halo passes are white and are not what a
  // placement assertion is about.
  std::vector<DrawnText> blackTexts() const {
    std::vector<DrawnText> out;
    for (const DrawnText& text : texts) {
      if (text.ink == MapInk::Black) out.push_back(text);
    }
    return out;
  }

  struct Box {
    int x, y, w, h;
    MapInk ink;
  };
  std::vector<Box> boxes;
  std::vector<DrawnText> texts;
  bool hasText = true;
  int topY = 0;
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
  // Out of the way of every case below, so the puck's own exclusion box is not
  // silently what rejected a placement.
  style.markerXPx = 20;
  style.markerYPx = 780;
  style.puckRadiusPx = 10;
  style.puckRingPx = 2;
  style.puckArrowPx = 10;
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

}  // namespace

TEST(MapLabels, DrawsTheNameBesideItsDot) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapLabels::offer(scratch, place(200, 400, 2, "Limbach"), 20, 780);
  MapLabels::draw(canvas, scratch, labelStyle());

  const auto drawn = canvas.blackTexts();
  ASSERT_EQ(drawn.size(), 1u);
  EXPECT_EQ(drawn[0].text, "Limbach");
  // Right of the dot is the first placement tried, and nothing is in the way.
  EXPECT_GT(drawn[0].x, 200);
  EXPECT_EQ(scratch.placed, 1);
  EXPECT_EQ(scratch.dropped, 0);
}

TEST(MapLabels, CityBeatsVillageWhenOnlyOneLabelFits) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  style.placeMaxLabels = 1;
  // Offered village first, so a first-come placer would draw the wrong one.
  MapLabels::offer(scratch, place(200, 300, 2, "Limbach"), 20, 780);
  MapLabels::offer(scratch, place(200, 500, 0, "Bratislava"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  const auto drawn = canvas.blackTexts();
  ASSERT_EQ(drawn.size(), 1u);
  EXPECT_EQ(drawn[0].text, "Bratislava");
  // Rank <= 1 is the major tier: bigger, bold.
  EXPECT_EQ(drawn[0].sizePx, style.placeLabelPx);
  EXPECT_TRUE(drawn[0].bold);
}

TEST(MapLabels, MinorTierIsSmallerAndNotBold) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  const MapStyle style = labelStyle();
  MapLabels::offer(scratch, place(200, 400, 3, "Cajla"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  const auto drawn = canvas.blackTexts();
  ASSERT_EQ(drawn.size(), 1u);
  EXPECT_EQ(drawn[0].sizePx, style.placeLabelMinorPx);
  EXPECT_FALSE(drawn[0].bold);
}

TEST(MapLabels, DoesNotLayANameAcrossTheRoute) {
  MapStyle style = labelStyle();
  // A vertical route through the middle of the screen, as wide as the style's.
  MapLabelScratch scratch;
  scratch.route.markSegment(240, 0, 240, kScreenH, style.routeWidthPx / 2);

  FakeCanvas canvas;
  // Dot far enough left that "Pezinok" (70 px) reaches the route if drawn to the
  // right, and has room on the left.
  MapLabels::offer(scratch, place(200, 400, 1, "Pezinok"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  const auto drawn = canvas.blackTexts();
  ASSERT_EQ(drawn.size(), 1u);
  EXPECT_LT(drawn[0].x, 200) << "the label should have flipped to the free side of the dot";
}

TEST(MapLabels, DropsTheNameWhenTheRouteLeavesNoRoom) {
  MapStyle style = labelStyle();
  style.placeLabelRouteOverlapPct = 0;

  MapLabelScratch scratch;
  // Route smeared over the whole neighbourhood of the dot: no side is free.
  for (int y = 300; y <= 500; y += 8) scratch.route.markSegment(100, y, 400, y, 8);

  FakeCanvas canvas;
  MapLabels::offer(scratch, place(250, 400, 1, "Pezinok"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  EXPECT_TRUE(canvas.blackTexts().empty());
  EXPECT_EQ(scratch.placed, 0);
  EXPECT_EQ(scratch.dropped, 1);
}

TEST(MapLabels, TwoNamesAtTheSameSpotDoNotOverlap) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  const MapStyle style = labelStyle();
  MapLabels::offer(scratch, place(240, 400, 1, "Modra"), 20, 780);
  MapLabels::offer(scratch, place(242, 402, 1, "Vinosady"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  const auto drawn = canvas.blackTexts();
  ASSERT_EQ(drawn.size(), 2u);
  const int firstW = static_cast<int>(drawn[0].text.size()) * kCharPx;
  const int secondW = static_cast<int>(drawn[1].text.size()) * kCharPx;
  const bool disjointX = drawn[0].x + firstW <= drawn[1].x || drawn[1].x + secondW <= drawn[0].x;
  const bool disjointY =
      drawn[0].y + drawn[0].sizePx <= drawn[1].y || drawn[1].y + drawn[1].sizePx <= drawn[0].y;
  EXPECT_TRUE(disjointX || disjointY);
}

TEST(MapLabels, HonoursTheLabelCap) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  style.placeMaxLabels = 2;
  for (int i = 0; i < 6; ++i) MapLabels::offer(scratch, place(100, 100 + i * 90, 2, "Vinosady"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  EXPECT_EQ(canvas.blackTexts().size(), 2u);
  EXPECT_EQ(scratch.placed, 2);
}

TEST(MapLabels, TheRungCapCanBeStricterThanTheStyle) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  style.placeMaxLabels = 6;
  for (int i = 0; i < 6; ++i) MapLabels::offer(scratch, place(100, 150 + i * 90, 2, "Vinosady"), 20, 780);
  // Rung 0 shows 480 x 800 m: one settlement, so it allows fewer names than the
  // style's own affordability cap (MapViewport::ZoomStep::maxLabels).
  MapLabels::draw(canvas, scratch, style, 2);

  EXPECT_EQ(canvas.blackTexts().size(), 2u);
  EXPECT_EQ(scratch.placed, 2);
}

TEST(MapLabels, TheStyleCapStillWinsWhenItIsTheStricterOne) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  style.placeMaxLabels = 1;
  for (int i = 0; i < 4; ++i) MapLabels::offer(scratch, place(100, 150 + i * 90, 2, "Vinosady"), 20, 780);
  MapLabels::draw(canvas, scratch, style, 14);

  EXPECT_EQ(canvas.blackTexts().size(), 1u);
}

TEST(MapLabels, ARungCapOfZeroDrawsNothing) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapLabels::offer(scratch, place(200, 400, 1, "Pezinok"), 20, 780);
  MapLabels::draw(canvas, scratch, labelStyle(), 0);

  EXPECT_TRUE(canvas.texts.empty());
}

TEST(MapLabels, TruncatesALongNameWithAnEllipsis) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  style.placeLabelMaxWidthPx = 80;  // 8 characters at this canvas's 10 px each
  MapLabels::offer(scratch, place(150, 400, 2, "Slovensky Grob"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  const auto drawn = canvas.blackTexts();
  ASSERT_EQ(drawn.size(), 1u);
  EXPECT_NE(drawn[0].text, "Slovensky Grob");
  EXPECT_NE(drawn[0].text.find("\xE2\x80\xA6"), std::string::npos) << "expected a U+2026 ellipsis";
  EXPECT_LE(static_cast<int>(drawn[0].text.size()) * kCharPx, style.placeLabelMaxWidthPx);
}

TEST(MapLabels, NeverCutsAMultiByteCharacterInHalf) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  // "Viničné" is 9 bytes; the cap allows 4 of this canvas's 10 px characters, so
  // the cut lands inside the two-byte c-caron unless the loop steps off it.
  style.placeLabelMaxWidthPx = 40;
  MapLabels::offer(scratch, place(150, 400, 2, "Vini\xC4\x8Dn\xC3\xA9"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  const auto drawn = canvas.blackTexts();
  ASSERT_EQ(drawn.size(), 1u);
  // Every continuation byte must still follow a lead byte -- i.e. the string is
  // valid UTF-8.
  const std::string& text = drawn[0].text;
  for (size_t i = 0; i < text.size(); ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if ((byte & 0xC0) != 0xC0) continue;
    const int extra = (byte & 0xE0) == 0xC0 ? 1 : (byte & 0xF0) == 0xE0 ? 2 : 3;
    ASSERT_LE(i + extra, text.size() - 1) << "lead byte with a truncated tail";
    for (int k = 1; k <= extra; ++k) {
      EXPECT_EQ(static_cast<unsigned char>(text[i + k]) & 0xC0, 0x80);
    }
    i += extra;
  }
}

TEST(MapLabels, AnOffScreenPlaceIsNotADroppedLabel) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  // The tile range is wider than the viewport, so most places in a frame are
  // outside it. Naming those is the off-screen chevrons' job.
  MapLabels::offer(scratch, place(-400, 400, 1, "Pezinok"), 20, 780);
  MapLabels::draw(canvas, scratch, labelStyle());

  EXPECT_TRUE(canvas.blackTexts().empty());
  EXPECT_EQ(scratch.placed, 0);
  EXPECT_EQ(scratch.dropped, 0);
}

TEST(MapLabels, KeepsOutOfTheHeaderBand) {
  FakeCanvas canvas;
  canvas.topY = 100;  // the device's header band (docs/map-header-status.md)
  MapLabelScratch scratch;
  MapLabels::offer(scratch, place(200, 104, 1, "Pezinok"), 20, 780);
  MapLabels::draw(canvas, scratch, labelStyle());

  for (const DrawnText& text : canvas.texts) EXPECT_GE(text.y, canvas.topY);
}

TEST(MapLabels, NoLabelsUnderThePuck) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  style.markerXPx = 240;
  style.markerYPx = 400;
  style.puckRadiusPx = 26;
  style.puckRingPx = 3;
  MapLabels::offer(scratch, place(240, 400, 1, "Pezinok"), style.markerXPx, style.markerYPx);
  MapLabels::draw(canvas, scratch, style);

  for (const DrawnText& text : canvas.blackTexts()) {
    const int width = static_cast<int>(text.text.size()) * kCharPx;
    const bool clearOfPuck = text.x > style.markerXPx + 29 || text.x + width < style.markerXPx - 29 ||
                             text.y > style.markerYPx + 29 || text.y + text.sizePx < style.markerYPx - 29;
    EXPECT_TRUE(clearOfPuck);
  }
}

TEST(MapLabels, ACanvasWithoutTextDrawsNoLabels) {
  FakeCanvas canvas;
  canvas.hasText = false;
  MapLabelScratch scratch;
  MapLabels::offer(scratch, place(200, 400, 1, "Pezinok"), 20, 780);
  MapLabels::draw(canvas, scratch, labelStyle());

  EXPECT_TRUE(canvas.texts.empty());
  EXPECT_TRUE(canvas.boxes.empty());
}

TEST(MapLabels, TheBoxStyleDrawsAWhiteBoxAndNoHalo) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  style.placeLabelBg = true;
  style.placeLabelBgPadPx = 3;
  style.placeLabelBgBorderPx = 1;
  MapLabels::offer(scratch, place(200, 400, 1, "Pezinok"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  ASSERT_EQ(canvas.boxes.size(), 2u);  // black border, white inside
  EXPECT_EQ(canvas.boxes[0].ink, MapInk::Black);
  EXPECT_EQ(canvas.boxes[1].ink, MapInk::White);
  for (const DrawnText& text : canvas.texts) EXPECT_EQ(text.ink, MapInk::Black);
}

TEST(MapLabels, HaloIsDrawnWhiteBeforeTheBlackText) {
  FakeCanvas canvas;
  MapLabelScratch scratch;
  MapStyle style = labelStyle();
  style.placeLabelHaloPx = 2;
  MapLabels::offer(scratch, place(200, 400, 1, "Pezinok"), 20, 780);
  MapLabels::draw(canvas, scratch, style);

  ASSERT_FALSE(canvas.texts.empty());
  EXPECT_EQ(canvas.texts.back().ink, MapInk::Black);
  // Eight offsets per ring radius (MapLabels.cpp's kHaloRing).
  EXPECT_EQ(canvas.texts.size(), 8u * style.placeLabelHaloPx + 1u);
  for (size_t i = 0; i + 1 < canvas.texts.size(); ++i) EXPECT_EQ(canvas.texts[i].ink, MapInk::White);
}

TEST(MapOccupancy, MarksAndReadsBackARectangle) {
  MapOccupancyGrid grid;
  EXPECT_FALSE(grid.anySet(0, 0, 480, 800));
  grid.markRect(100, 200, 16, 16);
  EXPECT_TRUE(grid.anySet(100, 200, 1, 1));
  EXPECT_FALSE(grid.anySet(300, 200, 16, 16));

  int set = 0, total = 0;
  grid.coverage(96, 200, 32, 16, set, total);
  EXPECT_GT(total, 0);
  EXPECT_GT(set, 0);
  EXPECT_LE(set, total);
}

TEST(MapOccupancy, ClipsASegmentThatStartsFarOffScreen) {
  MapOccupancyGrid grid;
  // A 200 km route at 1 m/px puts its far end 2e8 px away (IMapRouteSource.h).
  // The point of the test is that this returns at all, and still marks the part
  // that crosses the screen.
  grid.markSegment(-200000000, 400, 200000000, 400, 6);
  EXPECT_TRUE(grid.anySet(240, 400, 1, 1));
  EXPECT_FALSE(grid.anySet(240, 100, 1, 1));
}

TEST(MapOccupancy, ASegmentEntirelyOffScreenMarksNothing) {
  MapOccupancyGrid grid;
  grid.markSegment(-5000, -5000, -4000, -4000, 6);
  EXPECT_FALSE(grid.anySet(0, 0, 480, 800));
}
