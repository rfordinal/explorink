#include "MapPointMarks.h"

#include "../../components/icons/poi_icons.h"

namespace {

// The real drawing path since 2026-08-25 (`feat: wire the 24px POI icon set
// into MapPointMarks for on-panel judgment`): decodes a freeink::Icon's
// packed 1bpp bits into IMapCanvas draw calls (one drawLine per contiguous
// black run per row) so drawMarkAt() below can draw kPoiIconByCategory's 24px
// icons instead of the hand-drawn primitives further down this file. Those
// primitives (glyphWater() etc.) are now the fallback for an icon-less
// category only -- see drawMarkAt()'s `if (icon) ... else ...`.
void drawIconBitmap(IMapCanvas& canvas, const freeink::Icon& icon, int x, int y) {
  const int stride = (icon.w + 7) / 8;
  for (int row = 0; row < icon.h; ++row) {
    int runStart = -1;
    for (int col = 0; col <= icon.w; ++col) {
      bool ink = false;
      if (col < icon.w) {
        const uint8_t byte = icon.bits[row * stride + col / 8];
        ink = ((byte >> (7 - (col % 8))) & 1) == 0;  // bit 0 = draw
      }
      if (ink && runStart < 0) {
        runStart = col;
      } else if (!ink && runStart >= 0) {
        canvas.drawLine(x + runStart, y + row, x + col - 1, y + row, 1, MapInk::Black);
        runStart = -1;
      }
    }
  }
}

// A 1 px rectangle outline. IMapCanvas has no outline primitive -- fillRoundedRect
// is a fill -- so four lines, which is what MapRenderer already does for a
// building ring.
void rectOutline(IMapCanvas& canvas, int x, int y, int w, int h, int lineWidth, MapInk ink) {
  if (w <= 0 || h <= 0) return;
  canvas.drawLine(x, y, x + w - 1, y, lineWidth, ink);
  canvas.drawLine(x, y + h - 1, x + w - 1, y + h - 1, lineWidth, ink);
  canvas.drawLine(x, y, x, y + h - 1, lineWidth, ink);
  canvas.drawLine(x + w - 1, y, x + w - 1, y + h - 1, lineWidth, ink);
}

void fillRect(IMapCanvas& canvas, int x, int y, int w, int h, MapInk ink) {
  if (w <= 0 || h <= 0) return;
  canvas.fillRoundedRect(x, y, w, h, 0, ink);
}

// A disc: fillRoundedRect with the radius at half the side, which is the same
// trick MapRenderer uses for a place dot (MapRenderer.cpp, the places walk).
void disc(IMapCanvas& canvas, int cx, int cy, int diameter, MapInk ink) {
  if (diameter <= 0) return;
  canvas.fillRoundedRect(cx - diameter / 2, cy - diameter / 2, diameter, diameter, diameter / 2, ink);
}

void triangle(IMapCanvas& canvas, int x0, int y0, int x1, int y1, int x2, int y2, MapInk ink) {
  const int xs[3] = {x0, x1, x2};
  const int ys[3] = {y0, y1, y2};
  canvas.fillPolygon(xs, ys, 3, ink);
}

// --- the glyphs -------------------------------------------------------------
//
// Each draws inside a g x g box whose top-left is (x, y). Shapes are the ones
// the probe judged legible at g == 9 (MapPointMarks.h); they scale with g, but
// the sizes that matter were checked at that one size, on the panel's own
// pixels.

void glyphWater(IMapCanvas& canvas, int x, int y, int g) {
  // A drop: a triangle over a disc, both filled. Filled rather than outlined
  // because at this size an outlined drop is a ring with two pixels of white in
  // it.
  //
  // The disc is two thirds of the box and sits low, retuned 2026-08-22 when the
  // mark grew from 9 px to 11: at the old proportions (disc = g - 2, centred a
  // third up) the disc swallowed the triangle and the whole glyph read as a
  // blob on the panel.
  const int discDiameter = (g * 2) / 3;
  triangle(canvas, x + g / 2, y, x + g - 2, y + g / 2, x + 1, y + g / 2, MapInk::Black);
  disc(canvas, x + g / 2, y + g - discDiameter / 2 - 1, discDiameter, MapInk::Black);
}

void glyphShelter(IMapCanvas& canvas, int x, int y, int g) {
  // A tent: two strokes to a ridge and a floor.
  canvas.drawLine(x + 1, y + g - 1, x + g / 2, y, 1, MapInk::Black);
  canvas.drawLine(x + g - 2, y + g - 1, x + g / 2, y, 1, MapInk::Black);
  canvas.drawLine(x + 1, y + g - 1, x + g - 2, y + g - 1, 1, MapInk::Black);
}

void glyphHut(IMapCanvas& canvas, int x, int y, int g) {
  // A house: a filled roof over an outlined body. The filled roof is what
  // separates it from the tent at this size.
  const int eave = y + g / 2 - 1;
  triangle(canvas, x + g / 2, y, x + g - 1, eave, x, eave, MapInk::Black);
  rectOutline(canvas, x + 2, eave, g - 4, g - (eave - y), 1, MapInk::Black);
}

void glyphLodging(IMapCanvas& canvas, int x, int y, int g) {
  // A bed: headboard, a mattress bar, two legs. The mattress is a third of the
  // box and never more -- retuned 2026-08-22 with the mark's growth to 11 px,
  // where the old `g / 2 - 1` fill read as a black slab rather than a bed.
  const int mattress = g / 3 > 2 ? g / 3 : 2;
  const int top = y + g - mattress - 2;
  canvas.drawLine(x, y + 1, x, y + g - 1, 1, MapInk::Black);
  fillRect(canvas, x + 1, top, g - 2, mattress, MapInk::Black);
  canvas.drawLine(x + 1, y + g - 1, x + 1, y + g - 2, 1, MapInk::Black);
  canvas.drawLine(x + g - 2, y + g - 1, x + g - 2, y + g - 2, 1, MapInk::Black);
}

void glyphFuel(IMapCanvas& canvas, int x, int y, int g) {
  // A pump: an outlined body and a nozzle arm off its right shoulder.
  rectOutline(canvas, x + 1, y + 1, g - 4, g - 1, 1, MapInk::Black);
  canvas.drawLine(x + g - 3, y + 3, x + g - 1, y + 3, 1, MapInk::Black);
  canvas.drawLine(x + g - 1, y + 3, x + g - 1, y + g / 2, 1, MapInk::Black);
}

void glyphHospital(IMapCanvas& canvas, int x, int y, int g) {
  // A cross, three pixels thick. The one glyph nobody misreads, which is why
  // medical gets the simplest shape rather than a building.
  const int mid = g / 2;
  fillRect(canvas, x + mid - 1, y + 1, 3, g - 2, MapInk::Black);
  fillRect(canvas, x + 1, y + mid - 1, g - 2, 3, MapInk::Black);
}

void glyphPharmacy(IMapCanvas& canvas, int x, int y, int g) {
  // A capsule: an outlined rounded rect with its left half filled. Half filled
  // is what says capsule rather than button.
  canvas.fillRoundedRect(x, y + 2, g, g - 4, 3, MapInk::Black);
  canvas.fillRoundedRect(x + 1, y + 3, g - 2, g - 6, 2, MapInk::White);
  fillRect(canvas, x + 1, y + 3, g / 2, g - 6, MapInk::Black);
}

void glyphRescue(IMapCanvas& canvas, int x, int y, int g) {
  // A life buoy: a black ring with four spokes. Drawn as a filled disc with a
  // white disc knocked out of it -- IMapCanvas has no circle outline, and white
  // is a real operation here (IMapCanvas.h).
  const int cx = x + g / 2;
  const int cy = y + g / 2;
  disc(canvas, cx, cy, g, MapInk::Black);
  disc(canvas, cx, cy, g - 4, MapInk::White);
  canvas.drawLine(cx, y, cx, y + g - 1, 1, MapInk::Black);
  canvas.drawLine(x, cy, x + g - 1, cy, 1, MapInk::Black);
}

void glyphEmergencyPhone(IMapCanvas& canvas, int x, int y, int g) {
  // An exclamation mark. A handset silhouette came out a diagonal slash in the
  // probe; this square means "call for help from here", which is the point.
  const int mid = g / 2;
  fillRect(canvas, x + mid - 1, y, 2, g - 3, MapInk::Black);
  fillRect(canvas, x + mid - 1, y + g - 2, 2, 2, MapInk::Black);
}

void glyphTransport(IMapCanvas& canvas, int x, int y, int g) {
  // A bus: body, one window line, two wheels. A way out without a vehicle of
  // your own, which is what this category is for.
  rectOutline(canvas, x + 1, y + 1, g - 2, g - 3, 1, MapInk::Black);
  canvas.drawLine(x + 1, y + g / 2, x + g - 2, y + g / 2, 1, MapInk::Black);
  fillRect(canvas, x + 2, y + g - 2, 2, 2, MapInk::Black);
  fillRect(canvas, x + g - 4, y + g - 2, 2, 2, MapInk::Black);
}

void drawGlyph(IMapCanvas& canvas, uint8_t category, int x, int y, int g) {
  switch (static_cast<MapSafetyCategory>(category)) {
    case MapSafetyCategory::Water:
      glyphWater(canvas, x, y, g);
      return;
    case MapSafetyCategory::Shelter:
      glyphShelter(canvas, x, y, g);
      return;
    case MapSafetyCategory::Hut:
      glyphHut(canvas, x, y, g);
      return;
    case MapSafetyCategory::Lodging:
      glyphLodging(canvas, x, y, g);
      return;
    case MapSafetyCategory::Fuel:
      glyphFuel(canvas, x, y, g);
      return;
    case MapSafetyCategory::Hospital:
      glyphHospital(canvas, x, y, g);
      return;
    case MapSafetyCategory::Pharmacy:
      glyphPharmacy(canvas, x, y, g);
      return;
    case MapSafetyCategory::Rescue:
      glyphRescue(canvas, x, y, g);
      return;
    case MapSafetyCategory::EmergencyPhone:
      glyphEmergencyPhone(canvas, x, y, g);
      return;
    case MapSafetyCategory::Transport:
      glyphTransport(canvas, x, y, g);
      return;
    case MapSafetyCategory::Unknown:
      break;
  }
  // An unrecognised category draws an empty square rather than nothing. The
  // point exists in the data, which is what the square says; inventing a glyph
  // for a category this build does not know would say more than the file does.
}

// One mark for `category`, forced into a `side`x`side` box centred on
// (cx, cy). `side` is the category's own natural size (icon->w, or
// style.pointSquarePx for the primitive path) for a lone point, or
// style.pointClusterCellPx for one slot for a tiled cluster -- either way the
// box is what centres the white knock-out, the border-or-icon-halo and the
// flag consistently.
void drawMarkAt(IMapCanvas& canvas, uint8_t category, uint16_t flags, int side, int cx, int cy, const MapStyle& style) {
  if (side <= 0) return;
  const freeink::Icon* icon =
      category < sizeof(kPoiIconByCategory) / sizeof(kPoiIconByCategory[0]) ? kPoiIconByCategory[category] : nullptr;
  const int left = cx - side / 2;
  const int top = cy - side / 2;

  // White under the square first: a mark sitting on a road casing or a
  // built-up tone is unreadable otherwise, and white is a real operation on
  // this canvas (IMapCanvas.h). The knock-out is the box's own area only --
  // no halo beyond it, which would eat the map around every POI.
  fillRect(canvas, left, top, side, side, MapInk::White);

  if (icon) {
    // The border is already baked into the icon bitmap -- no separate
    // rectOutline. Drawn at the icon's own natural size and centred in the
    // box: for a tiled slot (side == pointClusterCellPx, a couple of pixels
    // bigger than the 24px icon) this leaves a hairline gap between adjacent
    // tiles' icons, which is what keeps them from reading as one shape.
    drawIconBitmap(canvas, *icon, cx - icon->w / 2, cy - icon->h / 2);
  } else {
    rectOutline(canvas, left, top, side, side, style.pointBorderPx > 0 ? style.pointBorderPx : 1, MapInk::Black);
    const int glyph = style.pointGlyphPx;
    if (glyph > 0 && glyph <= side - 2) {
      drawGlyph(canvas, category, cx - glyph / 2, cy - glyph / 2, glyph);
    }
  }

  // One corner flag for "there is a condition attached", top-right, drawn last
  // so it wins over a glyph that reaches the corner. Never for not_potable:
  // that point is not written at all (MapPointMarks.h).
  const int flag = icon ? 6 : style.pointFlagPx;
  if (flag > 0 && flags != 0) {
    const int x1 = left + side - 1;
    triangle(canvas, x1 - flag, top + 1, x1 - 1, top + 1, x1 - 1, top + flag, MapInk::Black);
  }
}

int marksSide(uint8_t category, const MapStyle& style) {
  const freeink::Icon* icon =
      category < sizeof(kPoiIconByCategory) / sizeof(kPoiIconByCategory[0]) ? kPoiIconByCategory[category] : nullptr;
  return icon ? icon->w : style.pointSquarePx;
}

// One cluster: the running centroid of every merged point's screen position,
// which category ids are present, and which of those carry a condition flag.
// categoryMask/flagMask are bit i = MapSafetyCategory id i -- see
// MapPointMarks.h, "a cluster's category mask assumes every point in it is
// the same kind", for the one thing this does not yet handle.
struct Cluster {
  int32_t sumX = 0;
  int32_t sumY = 0;
  int32_t count = 0;
  uint16_t categoryMask = 0;
  uint16_t flagMask = 0;

  int32_t cx() const { return count > 0 ? sumX / count : 0; }
  int32_t cy() const { return count > 0 ? sumY / count : 0; }
};

// A viewport rarely opens more than a couple of z10 point shards
// (docs/point-file-spec.md; safety.py's own docstring: "tens of nodes ... per
// z10 shard"), so a few dozen distinct clusters is the expected ceiling, not
// a hard limit on how many points can exist -- see the overflow branch in
// addPoint() for what happens past it. 40 * sizeof(Cluster) is 640 B on the
// stack, freed when drawAll() returns; no heap.
constexpr int kMaxClusters = 40;

int32_t dist2(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
  const int64_t dx = x1 - x2;
  const int64_t dy = y1 - y2;
  const int64_t d2 = dx * dx + dy * dy;
  return d2 > INT32_MAX ? INT32_MAX : static_cast<int32_t>(d2);
}

// Merges `point` into the nearest cluster within `radiusPx`, or starts a new
// one. Past kMaxClusters, merges into the nearest cluster regardless of
// radius rather than dropping the point -- a point folded into a slightly
// wrong cluster is still on the map; a point silently skipped is not, and
// this only fires when a single viewport opens more distinct locations than
// any shard measured so far has shown.
void addPoint(Cluster* clusters, int& count, int32_t radiusPx, int32_t x, int32_t y, uint8_t category, bool flagged) {
  int best = -1;
  int32_t bestD2 = 0;
  const int32_t r2 = radiusPx * radiusPx;
  for (int i = 0; i < count; ++i) {
    const int32_t d2 = dist2(x, y, clusters[i].cx(), clusters[i].cy());
    if (d2 <= r2 && (best < 0 || d2 < bestD2)) {
      best = i;
      bestD2 = d2;
    }
  }
  if (best < 0) {
    if (count < kMaxClusters) {
      best = count++;
      clusters[best] = Cluster{};
    } else {
      for (int i = 0; i < count; ++i) {
        const int32_t d2 = dist2(x, y, clusters[i].cx(), clusters[i].cy());
        if (best < 0 || d2 < bestD2) {
          best = i;
          bestD2 = d2;
        }
      }
    }
  }
  Cluster& c = clusters[best];
  c.sumX += x;
  c.sumY += y;
  ++c.count;
  if (category < 16) {
    c.categoryMask |= static_cast<uint16_t>(1u << category);
    if (flagged) c.flagMask |= static_cast<uint16_t>(1u << category);
  }
}

void drawCluster(IMapCanvas& canvas, const Cluster& c, const MapStyle& style) {
  uint8_t categories[16];
  int n = 0;
  for (uint8_t cat = 0; cat < 16 && n < 16; ++cat) {
    if (c.categoryMask & (1u << cat)) categories[n++] = cat;
  }
  if (n == 0) return;

  const int32_t cx = c.cx();
  const int32_t cy = c.cy();
  if (n == 1) {
    const uint16_t flags = (c.flagMask & (1u << categories[0])) ? kPointFlaggedOnMapMask : 0;
    drawMarkAt(canvas, categories[0], flags, marksSide(categories[0], style), cx, cy, style);
    return;
  }

  // ceil(sqrt(n)): 2-4 categories -> 2x2, 5-9 -> 3x3, and so on. Never caps --
  // the maintainer's call 2026-08-25 was that a crowded spot grows the tile
  // rather than drop a category off the map.
  int side = 1;
  while (side * side < n) ++side;
  const int cell = style.pointClusterCellPx;
  const int32_t originX = cx - (side * cell) / 2;
  const int32_t originY = cy - (side * cell) / 2;
  for (int i = 0; i < n; ++i) {
    const int row = i / side;
    const int col = i % side;
    const int32_t slotCx = originX + col * cell + cell / 2;
    const int32_t slotCy = originY + row * cell + cell / 2;
    const uint16_t flags = (c.flagMask & (1u << categories[i])) ? kPointFlaggedOnMapMask : 0;
    drawMarkAt(canvas, categories[i], flags, cell, slotCx, slotCy, style);
  }
}

}  // namespace

namespace MapPointMarks {

int16_t reachPx(const MapStyle& style) {
  const int single = style.pointSquarePx / 2 + 1;
  if (style.pointClusterRadiusPx == 0) return static_cast<int16_t>(single);
  // Worst case for the query margin: every category a build knows about
  // landed in one cluster, tiled as big as that makes it, merged from as far
  // as pointClusterRadiusPx away.
  int side = 1;
  while (side * side < kSafetyCategoryCount) ++side;
  const int tileHalf = (side * style.pointClusterCellPx) / 2;
  const int reach = style.pointClusterRadiusPx + tileHalf + 1;
  return static_cast<int16_t>(reach > single ? reach : single);
}

void draw(IMapCanvas& canvas, const MapPointRef& point, const MapStyle& style) {
  if (point.kind == MapPointKind::Safety && !style.pointsSafetyEnabled) return;
  if (point.kind == MapPointKind::Landmark && !style.pointsLandmarkEnabled) return;
  const uint16_t flags = (point.flags & kPointFlaggedOnMapMask) != 0 ? kPointFlaggedOnMapMask : 0;
  drawMarkAt(canvas, point.category, flags, marksSide(point.category, style), static_cast<int>(point.x),
             static_cast<int>(point.y), style);
}

void drawAll(IMapCanvas& canvas, IMapPointSource& source, const MapStyle& style) {
  if (style.pointClusterRadiusPx == 0) {
    // Clustering off: exactly today's per-point walk, no scratch buffer.
    MapPointRef point;
    while (source.nextMapPoint(point)) {
      draw(canvas, point, style);
    }
    return;
  }

  Cluster clusters[kMaxClusters];
  int clusterCount = 0;
  MapPointRef point;
  while (source.nextMapPoint(point)) {
    if (point.kind == MapPointKind::Safety && !style.pointsSafetyEnabled) continue;
    if (point.kind == MapPointKind::Landmark && !style.pointsLandmarkEnabled) continue;
    addPoint(clusters, clusterCount, style.pointClusterRadiusPx, static_cast<int32_t>(point.x),
             static_cast<int32_t>(point.y), point.category, (point.flags & kPointFlaggedOnMapMask) != 0);
  }
  for (int i = 0; i < clusterCount; ++i) {
    drawCluster(canvas, clusters[i], style);
  }
}

}  // namespace MapPointMarks
