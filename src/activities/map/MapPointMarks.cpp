#include "MapPointMarks.h"

namespace {

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
  // because at 9 px an outlined drop is a ring with two pixels of white in it.
  triangle(canvas, x + g / 2, y, x + g - 2, y + g / 2, x + 1, y + g / 2, MapInk::Black);
  disc(canvas, x + g / 2, y + g - g / 3, g - 2, MapInk::Black);
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
  // A bed: headboard, a filled mattress, one leg.
  canvas.drawLine(x, y + 1, x, y + g - 1, 1, MapInk::Black);
  fillRect(canvas, x + 1, y + g / 2, g - 1, g / 2 - 1, MapInk::Black);
  canvas.drawLine(x + g - 1, y + g - 2, x + g - 1, y + g - 1, 1, MapInk::Black);
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

}  // namespace

namespace MapPointMarks {

void draw(IMapCanvas& canvas, const MapPointRef& point, const MapStyle& style) {
  const int side = style.pointSquarePx;
  if (side <= 0) return;
  if (point.kind == MapPointKind::Safety && !style.pointsSafetyEnabled) return;
  if (point.kind == MapPointKind::Landmark && !style.pointsLandmarkEnabled) return;

  const int left = static_cast<int>(point.x) - side / 2;
  const int top = static_cast<int>(point.y) - side / 2;

  // White under the square first: a mark sitting on a road casing or a
  // built-up tone is unreadable otherwise, and white is a real operation on
  // this canvas (IMapCanvas.h). The knock-out is the square's own area only --
  // no halo, which would eat the map around every POI.
  fillRect(canvas, left, top, side, side, MapInk::White);
  rectOutline(canvas, left, top, side, side, style.pointBorderPx > 0 ? style.pointBorderPx : 1, MapInk::Black);

  const int glyph = style.pointGlyphPx;
  if (glyph > 0 && glyph <= side - 2) {
    drawGlyph(canvas, point.category, left + (side - glyph) / 2, top + (side - glyph) / 2, glyph);
  }

  // One corner flag for "there is a condition attached", top-right, drawn last
  // so it wins over a glyph that reaches the corner. Never for not_potable:
  // that point is not written at all (MapPointMarks.h).
  const int flag = style.pointFlagPx;
  if (flag > 0 && (point.flags & kPointFlaggedOnMapMask) != 0) {
    const int x1 = left + side - 1;
    triangle(canvas, x1 - flag, top + 1, x1 - 1, top + 1, x1 - 1, top + flag, MapInk::Black);
  }
}

}  // namespace MapPointMarks
