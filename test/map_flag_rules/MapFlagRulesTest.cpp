// The way record's `flags` and `roughness` bytes, and the style grammar that
// finally reads them (MapStyle.h, MapRoadFlagRule; docs/map-style.md,
// "Matching a way's flag bits").
//
// Two things are being pinned here, and the first matters more than the second.
//
// **The default draws exactly what it drew before.** Every .tib tile has carried
// these two bytes since the first one was written, and the renderer read neither
// until 2026-08-27. The mechanism that changes that is a knob the maintainer
// turns on when there is a panel to judge it on -- so the shipped
// data/mapstyle.json must produce a byte-identical picture, and a flagged way
// must be indistinguishable from an unflagged one under it. That is what
// TheShippedDefault asserts, and it is the same claim the golden render in
// test/map_tile_reader makes at whole-frame scale.
//
// **And the mechanism works when a style asks for it.** A rule matched on a flag
// bit or a roughness floor replaces the class's stroke; the rest of the map is
// untouched. Rendered rather than unit-tested field by field, because "the
// renderer reads the rule" is the claim, and reading a struct member proves
// nothing about which pixels land.
#include <gtest/gtest.h>

#include <vector>

#include "MapClassEnum.h"
#include "MapRenderer.h"
#include "MapStyleDefaults.h"
// Included for its static_asserts, not for a symbol used below. MapWaymark.h is
// generated in the tilegen repo and committed here, and until 2026-08-27 no
// translation unit included it, so a drift in the bit layout or in the three
// parallel 64-entry tables compiled cleanly and shipped. This is the flags word
// the waymark id lives in (bits 8-13), so this is the file that should carry it.
// Do not drop the include as unused.
#include "MapWaymark.h"
#include "PpmCanvas.h"

namespace {

constexpr int kWidth = 120;
constexpr int kHeight = 120;

// One horizontal road across the middle of the canvas. Horizontal on purpose:
// MapStroke stacks copies along the dominant axis, so a horizontal line's black
// count is width * length exactly and a diagonal's is not (MapStroke.h).
struct OneWay : public IMapSource {
  MapWayRef way;
  std::vector<int16_t> xs{10, 110};
  std::vector<int16_t> ys{60, 60};
  int handedOut = 0;

  OneWay(uint8_t classId, uint16_t flags, uint8_t roughness) {
    way.classId = classId;
    way.flags = flags;
    way.roughness = roughness;
    way.pointCount = 2;
    way.xs = xs.data();
    way.ys = ys.data();
  }

  bool beginWays() override {
    handedOut = 0;
    return true;
  }
  bool nextWay(MapWayRef& out) override {
    if (handedOut++ > 0) return false;
    out = way;
    return true;
  }
  // Every other layer is empty. The renderer must not open one it has nothing
  // to draw from, and a false here is the "this tile has no such layer" answer
  // MapTileSource gives (IMapSource.h).
  bool beginBuildings() override { return false; }
  bool nextBuilding(MapWayRef&) override { return false; }
  bool beginWater() override { return false; }
  bool nextWater(MapWayRef&) override { return false; }
  bool beginLanduse() override { return false; }
  bool nextLanduse(MapWayRef&) override { return false; }
  bool beginContours() override { return false; }
  bool nextContour(MapWayRef&) override { return false; }
  bool beginPlaces() override { return false; }
  bool nextPlace(MapPlaceRef&) override { return false; }
};

// The renderer with nothing but the road layer: no route, no timing, no places,
// no labels, no points, and no marker (drawMarker is a separate call).
std::vector<uint8_t> renderWay(const MapStyle& style, uint8_t classId, uint16_t flags, uint8_t roughness) {
  PpmCanvas canvas(kWidth, kHeight);
  OneWay source(classId, flags, roughness);
  MapViewState state;
  state.markerX = -1000;  // off canvas; render() draws no marker anyway
  state.markerY = -1000;
  MapRenderer::render(canvas, source, state, style);
  return canvas.pixels();
}

int blackCount(const std::vector<uint8_t>& pixels) {
  int count = 0;
  for (uint8_t value : pixels) {
    if (value) ++count;
  }
  return count;
}

// Black pixels in the column through the middle of the way -- its thickness.
int thicknessAtMidspan(const std::vector<uint8_t>& pixels) {
  int count = 0;
  for (int y = 0; y < kHeight; ++y) {
    if (pixels[static_cast<size_t>(y) * kWidth + 60]) ++count;
  }
  return count;
}

// A minimal style: every class a 1 px solid hairline, no casing, nothing else
// drawn. Frozen here rather than read from data/mapstyle.json for the same
// reason the golden fixture freezes its own -- a style edit must not fail this
// test, and a 1 px line is plain Bresenham so no thick-line decomposition can
// move a pixel.
MapStyle hairlineStyle() {
  MapStyle style{};
  for (uint8_t i = 0; i < kClassEnumSlots; ++i) {
    style.roadWidthPx[i] = 1;
    style.roadPattern[i] = MapLinePattern::Solid;
    style.roadFillTone[i] = MapAreaTone::None;
  }
  return style;
}

// `path`, not `track`. Both are the hiker's classes, but data/mapstyle.json's
// base style hides `track` outright (roadWidthPx[11] == 0), so a way of that
// class draws nothing at all under kDefaultMapStyle and the default-unchanged
// test would have been asserting equality between two empty canvases.
constexpr uint8_t kPath = static_cast<uint8_t>(MapClassId::Path);

}  // namespace

// ---------------------------------------------------------------------------
// The default must not have changed anything.
// ---------------------------------------------------------------------------

TEST(TheShippedDefault, CarriesNoFlagRuleAtAll) {
  for (uint8_t slot = 0; slot < kMapRoadFlagRuleSlots; ++slot) {
    const MapRoadFlagRule& rule = kDefaultMapStyle.roadFlagRules[slot];
    EXPECT_EQ(rule.flagMask, 0u) << "slot " << int(slot);
    EXPECT_EQ(rule.roughnessMin, 0u) << "slot " << int(slot);
    EXPECT_EQ(rule.widthPx, 0u) << "slot " << int(slot);
    EXPECT_FALSE(rule.hidden) << "slot " << int(slot);
  }
}

// The whole point of the default: an access-restricted way still draws exactly
// like an open one. This test is the thing that should go red on the day the
// maintainer turns the knob on -- deliberately, with a panel in hand.
TEST(TheShippedDefault, DrawsAFlaggedWayIdenticallyToAnOpenOne) {
  const uint16_t everyDataBit =
      kFlagLink | kFlagBridge | kFlagTunnel | kFlagOneway | kFlagUnpaved | kFlagNoMotor | kFlagNoBicycle | kFlagNoFoot;
  const std::vector<uint8_t> open = renderWay(kDefaultMapStyle, kPath, 0, 0);
  const std::vector<uint8_t> flagged = renderWay(kDefaultMapStyle, kPath, everyDataBit, 7);
  EXPECT_GT(blackCount(open), 0) << "the fixture way must actually draw something";
  EXPECT_EQ(open, flagged);
}

TEST(TheShippedDefault, HairlineStyleAlsoIgnoresEveryFlag) {
  const MapStyle style = hairlineStyle();
  EXPECT_EQ(renderWay(style, kPath, 0, 0), renderWay(style, kPath, kFlagNoFoot, 0));
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagNoFoot, 0)), 1);
}

// ---------------------------------------------------------------------------
// And the mechanism, once a style asks for it.
// ---------------------------------------------------------------------------

TEST(AFlagRule, ReplacesTheStrokeOfAMatchedWay) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] = MapRoadFlagRule{.flagMask = kFlagNoFoot, .widthPx = 5};

  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagNoFoot, 0)), 5);
  // An unflagged way of the same class is untouched.
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 0)), 1);
  // So is a way carrying a different bit.
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagBridge, 0)), 1);
}

TEST(AFlagRule, MatchesAnyNamedBitNotAllOfThem) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] =
      MapRoadFlagRule{.flagMask = static_cast<uint16_t>(kFlagNoFoot | kFlagNoBicycle), .widthPx = 5};

  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagNoFoot, 0)), 5);
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagNoBicycle, 0)), 5);
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagNoMotor, 0)), 1);
}

TEST(AFlagRule, CanHideAMatchedWayEntirely) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] = MapRoadFlagRule{.flagMask = kFlagNoFoot, .hidden = true};

  EXPECT_EQ(blackCount(renderWay(style, kPath, kFlagNoFoot, 0)), 0);
  EXPECT_GT(blackCount(renderWay(style, kPath, 0, 0)), 0);
}

TEST(AFlagRule, CanDashAMatchedWay) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] = MapRoadFlagRule{
      .flagMask = kFlagNoFoot, .widthPx = 1, .dashPx = 4, .gapPx = 4, .pattern = MapLinePattern::Dashed};

  const int solid = blackCount(renderWay(style, kPath, 0, 0));
  const int dashed = blackCount(renderWay(style, kPath, kFlagNoFoot, 0));
  EXPECT_GT(dashed, 0);
  EXPECT_LT(dashed, solid) << "a 4/4 dash must ink about half of what the solid line does";
}

// A hidden class stays hidden. Its ways are already intersected out of the
// rung's tile class mask (scripts/gen_mode_masks.py), so a flag rule that
// appeared to un-hide it would draw nothing on the device and read as a bug in
// the rule rather than in the mechanism.
TEST(AFlagRule, DoesNotUnhideAClassTheStyleHides) {
  MapStyle style = hairlineStyle();
  style.roadWidthPx[kPath] = 0;
  style.roadFlagRules[0] = MapRoadFlagRule{.flagMask = kFlagNoFoot, .widthPx = 5};

  EXPECT_EQ(blackCount(renderWay(style, kPath, kFlagNoFoot, 0)), 0);
}

TEST(AFlagRule, TheFirstMatchingSlotWins) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] = MapRoadFlagRule{.flagMask = kFlagNoFoot, .widthPx = 5};
  style.roadFlagRules[1] = MapRoadFlagRule{.flagMask = kFlagNoFoot, .widthPx = 9};

  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagNoFoot, 0)), 5);
}

TEST(AFlagRule, AnEmptySlotIsSkippedRatherThanMatchingEverything) {
  MapStyle style = hairlineStyle();
  // Slot 0 empty, slot 1 real. A loop that treated an all-zero slot as a match
  // would draw every way at slot 0's width, which is 0 -- i.e. an empty map.
  style.roadFlagRules[1] = MapRoadFlagRule{.flagMask = kFlagNoFoot, .widthPx = 5};

  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 0)), 1);
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagNoFoot, 0)), 5);
}

// ---------------------------------------------------------------------------
// The roughness byte. Real data: 53.8 % of the 474,178 road ways in the local
// mirror carry a non-zero roughness (docs/map-style.md has the histogram).
// ---------------------------------------------------------------------------

TEST(ARoughnessRule, MatchesAtOrAboveTheFloorAndNotBelowIt) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] = MapRoadFlagRule{.roughnessMin = 5, .widthPx = 5};

  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 4)), 1);
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 5)), 5);
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 7)), 5);
}

// roughness 0 is "unknown", not "smooth" (docs/map-data-spec.md). 46.2 % of the
// mirror's road ways are 0, so a floor that swept them in would restyle nearly
// half the network.
TEST(ARoughnessRule, NeverMatchesRoughnessZero) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] = MapRoadFlagRule{.roughnessMin = 1, .widthPx = 5};

  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 0)), 1);
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 1)), 5);
}

// The upper bits of the byte are sac_scale and trail_visibility, and the builder
// writes zeros into them today -- but a future tile will not, and reading the
// whole byte would then report a smooth track as the worst surface there is.
TEST(ARoughnessRule, IgnoresTheBitsAboveTheThreeItOwns) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] = MapRoadFlagRule{.roughnessMin = 5, .widthPx = 5};

  // roughness 1 with every upper bit set. As a raw byte that is 0xF9 = 249,
  // which is well past the floor; masked to three bits it is 1, which is not.
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 0xF9)), 1);
}

TEST(ARoughnessRule, AndAFlagBitMustBothHoldWhenBothAreNamed) {
  MapStyle style = hairlineStyle();
  style.roadFlagRules[0] = MapRoadFlagRule{.flagMask = kFlagUnpaved, .roughnessMin = 5, .widthPx = 5};

  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagUnpaved, 5)), 5);
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, kFlagUnpaved, 4)), 1);
  EXPECT_EQ(thicknessAtMidspan(renderWay(style, kPath, 0, 5)), 1);
}

// ---------------------------------------------------------------------------
// The reject margin has to know about a flag rule's width, or a flag-widened way
// just off the panel loses its ink (MapTileSource::Config::rejectMarginPx).
// ---------------------------------------------------------------------------

TEST(TheStrokeMargin, CountsAFlagRuleWiderThanAnyClass) {
  MapStyle style = hairlineStyle();
  EXPECT_EQ(mapStyleMaxStrokePx(style), 1);
  style.roadFlagRules[0] = MapRoadFlagRule{.flagMask = kFlagNoFoot, .widthPx = 9};
  EXPECT_EQ(mapStyleMaxStrokePx(style), 9);
}
