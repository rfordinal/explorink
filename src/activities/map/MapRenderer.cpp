#include "MapRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "MapAreaClass.h"
#include "MapAreaFill.h"
#include "MapLabels.h"
#include "MapPointMarks.h"
#include "MapTextMask.h"

namespace {

// Unit-ish direction vectors for the 16 snapped headings, scaled by 8 (so a
// 22.5 degree component like sin(22.5) becomes the integer 3/8, and the
// diagonal 0.7071 factor becomes 6/8) -- avoids any sin/cos call, matching
// the "no per-frame trig" design decision. Index = MapHeading value.
struct Vec2 {
  int dx, dy;
};
constexpr Vec2 kHeadingDir[16] = {
    {0, -8},   // N
    {3, -7},   // NNE
    {6, -6},   // NE
    {7, -3},   // ENE
    {8, 0},    // E
    {7, 3},    // ESE
    {6, 6},    // SE
    {3, 7},    // SSE
    {0, 8},    // S
    {-3, 7},   // SSW
    {-6, 6},   // SW
    {-7, 3},   // WSW
    {-8, 0},   // W
    {-7, -3},  // WNW
    {-6, -6},  // NW
    {-3, -7},  // NNW
};

// One way's worth of line drawing at a given width and ink.
void strokeWay(IMapCanvas& canvas, const MapWayRef& way, int lineWidth, MapInk ink) {
  if (lineWidth <= 0) return;
  for (uint16_t i = 1; i < way.pointCount; ++i) {
    canvas.drawLine(way.xs[i - 1], way.ys[i - 1], way.xs[i], way.ys[i], lineWidth, ink);
  }
}

// The same way, drawn as inked runs of `dashPx` separated by gaps of `gapPx`.
//
// Dash and gap are separate numbers because the two features that need them
// want opposite proportions. A railway is the modern-map convention: long dark
// runs with short breaks cut across them, which reads as one continuous line
// that happens to be ticked. A watercourse wants the opposite -- short marks
// with real space between them, so it reads as broken.
//
// A broken line is the only mark left that says "this is not a road you drive
// on", and two features need it: a railway (thick, the modern-map convention)
// and a watercourse (thin). Both otherwise arrive as plain black lines
// indistinguishable from a street.
//
// The dash phase carries across vertices rather than restarting at each one.
// Restarting is the obvious implementation and it is wrong: a curved way is
// made of many short segments, so every vertex would start a fresh dash and a
// bend would fill in solid -- exactly where the eye is looking.
// One mark, stamped screen-aligned at (cx, cy). Built from drawLine because that
// and fillSpan are the whole canvas -- there is no rect primitive, and a width-N
// line of length N is an N x N block.
//
// Axis-aligned on purpose (MapStyle.h, MapLineMark): the edges land on the pixel
// grid, and on 1-bit there is no grey to soften a staircase.
// `towardsX/towardsY` is the direction the mark should face when it has one -- for
// a cliff, the downslope side. Only `U` reads it; every other shape is the same
// shape turned and ignores it.
void stampMark(IMapCanvas& canvas, const int cx, const int cy, const MapLineMark mark, const int sizePx,
               const MapInk ink, const int towardsX = 0, const int towardsY = 0) {
  if (mark == MapLineMark::None || sizePx <= 0) return;
  const int half = sizePx / 2;
  switch (mark) {
    case MapLineMark::Dot:
      // A dot is a square of the same width; the name is what a style author
      // reaches for when they mean "small", so it is a size shorthand and the
      // generator halves it rather than a second shape to judge on glass.
      canvas.drawLine(cx, cy, cx, cy, sizePx, ink);
      break;
    case MapLineMark::Square:
      // Length sizePx at width sizePx: a filled block, one call.
      canvas.drawLine(cx - half, cy, cx - half + sizePx - 1, cy, sizePx, ink);
      break;
    case MapLineMark::Cross:
      canvas.drawLine(cx - half, cy, cx + half, cy, 1, ink);
      canvas.drawLine(cx, cy - half, cx, cy + half, 1, ink);
      break;
    case MapLineMark::Diamond:
      // Row by row rather than four strokes: a stroked diamond at 5 px is two
      // pixels of outline and three of hole, and what is wanted is a solid
      // silhouette. Half-width shrinks by one per row from the middle.
      for (int dy = -half; dy <= half; ++dy) {
        const int w = half - (dy < 0 ? -dy : dy);
        canvas.drawLine(cx - w, cy + dy, cx + w, cy + dy, 1, ink);
      }
      break;
    case MapLineMark::Comb: {
      // Bar along the line, three teeth towards the drop. Same four-way snap as
      // `U` and for the same reason: every edge stays on the pixel grid.
      const int ax = towardsX < 0 ? -towardsX : towardsX;
      const int ay = towardsY < 0 ? -towardsY : towardsY;
      int fx = 0, fy = 1;
      if (ax > ay) {
        fx = towardsX > 0 ? 1 : -1;
        fy = 0;
      } else if (ay > 0) {
        fy = towardsY > 0 ? 1 : -1;
        fx = 0;
      }
      const int px = -fy, py = fx;  // along the bar
      // The bar sits on the line, the teeth reach `half` towards the drop.
      canvas.drawLine(cx - px * half, cy - py * half, cx + px * half, cy + py * half, 1, ink);
      for (int t = -1; t <= 1; ++t) {
        const int bx = cx + px * half * t;
        const int by = cy + py * half * t;
        canvas.drawLine(bx, by, bx + fx * half, by + fy * half, 1, ink);
      }
      break;
    }
    case MapLineMark::U: {
      // Two uprights and a floor, the floor on the side `towards` points at.
      // Snapped to four directions so every edge stays on the pixel grid
      // (MapStyle.h, MapLineMark::U).
      const int ax = towardsX < 0 ? -towardsX : towardsX;
      const int ay = towardsY < 0 ? -towardsY : towardsY;
      // Which axis dominates decides the facing; ties and a zero direction fall
      // to "floor at the bottom", the upright U.
      int fx = 0, fy = 1;
      if (ax > ay) {
        fx = towardsX > 0 ? 1 : -1;
        fy = 0;
      } else if (ay > 0) {
        fy = towardsY > 0 ? 1 : -1;
        fx = 0;
      }
      // The floor is the edge at +f; the uprights are perpendicular to it.
      const int px = -fy, py = fx;  // along the floor
      const int f0x = cx + fx * half, f0y = cy + fy * half;
      canvas.drawLine(f0x - px * half, f0y - py * half, f0x + px * half, f0y + py * half, 1, ink);
      canvas.drawLine(f0x - px * half, f0y - py * half, f0x - px * half - fx * (sizePx - 1),
                      f0y - py * half - fy * (sizePx - 1), 1, ink);
      canvas.drawLine(f0x + px * half, f0y + py * half, f0x + px * half - fx * (sizePx - 1),
                      f0y + py * half - fy * (sizePx - 1), 1, ink);
      break;
    }
    case MapLineMark::Circle:
      // A disc, by the integer circle test per row. Not a stroked ring: a ring at
      // 5 px is one pixel wide and reads as a dash, which is the mark it has to
      // be distinguishable from.
      for (int dy = -half; dy <= half; ++dy) {
        int w = 0;
        while ((w + 1) * (w + 1) + dy * dy <= half * half + half) ++w;
        canvas.drawLine(cx - w, cy + dy, cx + w, cy + dy, 1, ink);
      }
      break;
    default:
      break;
  }
}

// A whole line with short combs hanging off one side of it: the rock-face mark
// (MapStyle.h, MapLinePattern::Hachured).
//
// **Right of the direction of travel, and that is the whole point.** OSM puts a
// cliff's lower ground on the right of the way's direction and the tile keeps the
// points in that order, so the comb direction is data rather than taste. In screen
// coordinates y grows downwards, so rotating the travel vector by +90 degrees --
// (dx, dy) -> (-dy, dx) -- lands on the right-hand side: facing east, that is
// south, which is right. Get the sign wrong and every cliff on the map claims the
// drop is uphill.
void strokeWayHachured(IMapCanvas& canvas, const MapWayRef& way, const int lineWidth, const int tickPx,
                       const int gapPx, const MapInk ink) {
  if (lineWidth <= 0 || way.pointCount < 2) return;
  // The line itself, unbroken. A cliff is continuous ground; only the combs are
  // periodic.
  strokeWay(canvas, way, lineWidth, ink);
  if (tickPx <= 0 || gapPx <= 0) return;

  int phase = 0;  // distance since the last comb
  for (uint16_t i = 1; i < way.pointCount; ++i) {
    const int x0 = way.xs[i - 1], y0 = way.ys[i - 1];
    const int x1 = way.xs[i], y1 = way.ys[i];
    const int dx = x1 - x0, dy = y1 - y0;
    const int len =
        static_cast<int>(std::lround(std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy)));
    if (len <= 0) continue;

    int walked = 0;
    while (walked < len) {
      const int step = std::min(gapPx - phase, len - walked);
      if (phase + step >= gapPx) {
        const int at = walked + step;
        const int px = x0 + dx * at / len;
        const int py = y0 + dy * at / len;
        // Right of travel, scaled to the tick length. Integer, so a short segment
        // with a large tick still points the right way.
        const int tx = -dy * tickPx / len;
        const int ty = dx * tickPx / len;
        canvas.drawLine(px, py, px + tx, py + ty, 1, ink);
        phase = 0;
      } else {
        phase += step;
      }
      walked += step;
    }
  }
}

// dash, gap, mark, gap -- repeating. The rhythm a paper map uses to separate
// route classes when it cannot use colour, which on this panel is always.
//
// Same arc-length walk as strokeWayDashed, with a four-slot period instead of
// two. The mark is stamped once, at the middle of its slot, the first time the
// walk crosses that middle -- not every step, or a segment boundary landing
// inside the slot would stamp it twice.
void strokeWayDashMark(IMapCanvas& canvas, const MapWayRef& way, const int lineWidth, const int dashPx,
                       const int gapPx, const MapLineMark mark, const int markPx, const MapInk ink) {
  if (lineWidth <= 0 || dashPx <= 0 || gapPx <= 0 || markPx <= 0) return;
  const int markStart = dashPx + gapPx;
  const int markEnd = markStart + markPx;
  const int period = markEnd + gapPx;
  const int markMid = markStart + markPx / 2;
  int phase = 0;

  for (uint16_t i = 1; i < way.pointCount; ++i) {
    const int x0 = way.xs[i - 1], y0 = way.ys[i - 1];
    const int x1 = way.xs[i], y1 = way.ys[i];
    const int dx = x1 - x0, dy = y1 - y0;
    const int len =
        static_cast<int>(std::lround(std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy)));
    if (len <= 0) continue;

    int walked = 0;
    while (walked < len) {
      // The next boundary in the period, so a step never straddles two slots.
      int nextEdge = period;
      if (phase < dashPx) {
        nextEdge = dashPx;
      } else if (phase < markStart) {
        nextEdge = markStart;
      } else if (phase < markEnd) {
        nextEdge = markEnd;
      }
      const int step = std::min(nextEdge - phase, len - walked);
      if (phase < dashPx) {
        const int ax = x0 + dx * walked / len;
        const int ay = y0 + dy * walked / len;
        const int bx = x0 + dx * (walked + step) / len;
        const int by = y0 + dy * (walked + step) / len;
        canvas.drawLine(ax, ay, bx, by, lineWidth, ink);
      } else if (phase <= markMid && phase + step > markMid) {
        const int at = walked + (markMid - phase);
        // Right of travel, which on a cliff record is the downslope side (OSM
        // order; docs/map-data-spec.md, "The relief layer's second class"). y grows
        // downwards on screen, so +90 degrees is (dx, dy) -> (-dy, dx).
        stampMark(canvas, x0 + dx * at / len, y0 + dy * at / len, mark, markPx, ink, -dy, dx);
      }
      walked += step;
      phase = (phase + step) % period;
    }
  }
}

void strokeWayDashed(IMapCanvas& canvas, const MapWayRef& way, int lineWidth, int dashPx, int gapPx, MapInk ink) {
  if (lineWidth <= 0 || dashPx <= 0 || gapPx <= 0) return;
  const int period = dashPx + gapPx;
  int phase = 0;  // distance into the current period, 0..period-1

  for (uint16_t i = 1; i < way.pointCount; ++i) {
    const int x0 = way.xs[i - 1], y0 = way.ys[i - 1];
    const int x1 = way.xs[i], y1 = way.ys[i];
    const int dx = x1 - x0, dy = y1 - y0;
    const int len =
        static_cast<int>(std::lround(std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy)));
    if (len <= 0) continue;

    int walked = 0;
    while (walked < len) {
      const int remainingInPhase = (phase < dashPx ? dashPx : period) - phase;
      const int step = std::min(remainingInPhase, len - walked);
      if (phase < dashPx) {  // the inked half of the period
        const int ax = x0 + dx * walked / len;
        const int ay = y0 + dy * walked / len;
        const int bx = x0 + dx * (walked + step) / len;
        const int by = y0 + dy * (walked + step) / len;
        canvas.drawLine(ax, ay, bx, by, lineWidth, ink);
      }
      walked += step;
      phase = (phase + step) % period;
    }
  }
}

// The stroke one way is actually drawn with. The class's own numbers, unless a
// flag rule matched and replaced them (MapStyle.h, MapRoadFlagRule).
//
// Resolved once per way and shared by both road passes, so the black stroke and
// the white fill inside it can never disagree about how wide the road is. Width
// 0 means "do not draw".
struct RoadStroke {
  int width = 0;
  int casing = 0;
  MapLinePattern pattern = MapLinePattern::Solid;
  int dash = 0;
  int gap = 0;
  MapAreaTone tone = MapAreaTone::None;
  MapLineMark mark = MapLineMark::None;
  int markPx = 0;
};

// A class id past the enum's slots can only come from a corrupt tile; reserved
// slots carry width 0 anyway, so that guard is about the array bound, not the
// style.
//
// Costs one loop over kMapRoadFlagRuleSlots per way per pass, and every slot is
// empty in every style shipped today, so it is four compares against a `flagMask
// == 0 && roughnessMin == 0` that is in flash beside the widths already being
// read. No allocation, no per-way state.
RoadStroke roadStrokeFor(const MapStyle& style, const MapWayRef& way) {
  RoadStroke stroke;
  if (way.classId >= kClassEnumSlots) return stroke;
  stroke.width = style.roadWidthPx[way.classId];
  // A class the style hides stays hidden. Its ways are already intersected out
  // of the rung's tile class mask (gen_mode_masks.py), so a flag rule that
  // un-hid it would draw nothing at all -- see MapStyle::roadFlagRules.
  if (stroke.width == 0) return stroke;
  stroke.casing = style.roadCasingPx[way.classId];
  stroke.pattern = style.roadPattern[way.classId];
  stroke.dash = style.roadDashPx[way.classId];
  stroke.gap = style.roadGapPx[way.classId];
  stroke.mark = style.roadMark[way.classId];
  stroke.markPx = style.roadMarkPx[way.classId];
  stroke.tone = style.roadFillTone[way.classId];

  // First match wins, in file order, and a match replaces the whole stroke
  // rather than patching fields of it.
  for (uint8_t slot = 0; slot < kMapRoadFlagRuleSlots; ++slot) {
    const MapRoadFlagRule& rule = style.roadFlagRules[slot];
    if (rule.flagMask == 0 && rule.roughnessMin == 0) continue;  // unused slot
    if (rule.flagMask != 0 && (way.flags & rule.flagMask) == 0) continue;
    if (rule.roughnessMin != 0 && (way.roughness & kMapRoughnessValueMask) < rule.roughnessMin) continue;
    if (rule.hidden) {
      stroke = RoadStroke{};
      return stroke;
    }
    stroke.width = rule.widthPx;
    stroke.casing = rule.casingPx;
    stroke.pattern = rule.pattern;
    stroke.dash = rule.dashPx;
    stroke.gap = rule.gapPx;
    // A flag rule carries no tone (MapStyle.h), so a matched way loses the one
    // its class had. Stated rather than inherited: a shaded ribbon whose width
    // was just replaced has no interior anyone reasoned about.
    stroke.tone = MapAreaTone::None;
    // Same reasoning for the mark: a flag rule replaces the stroke, so it does
    // not inherit a rhythm the class chose. A flag rule that wants a marked line
    // says so itself.
    stroke.mark = MapLineMark::None;
    stroke.markPx = 0;
    return stroke;
  }
  return stroke;
}

// One landuse class, drawn as tone then hatch then outline. Called once per
// class rather than once per record because forest and built-up sit at
// different depths in the draw order and share a single tile layer.
void drawLanduseClass(IMapCanvas& canvas, IMapSource& source, const MapStyle& style, const MapLanduseClass wanted) {
  const uint8_t index = static_cast<uint8_t>(wanted);
  if (style.landuseTone[index] == MapAreaTone::None && style.landuseHatch[index] == MapAreaFill::Pattern::None &&
      style.landuseOutlinePx[index] == 0) {
    return;  // nothing to draw for this class, so do not walk the layer for it
  }
  if (!source.beginLanduse()) return;
  MapWayRef ring;
  while (source.nextLanduse(ring)) {
    if (ring.classId != index) continue;
    MapAreaFill::toneRing(canvas, ring.xs, ring.ys, ring.pointCount, style.landuseTone[index]);
    MapAreaFill::hatchRing(canvas, ring.xs, ring.ys, ring.pointCount, style.landuseHatch[index],
                           style.landuseHatchSpacingPx[index], MapInk::Black);
    MapAreaFill::outlineRingDashed(canvas, ring.xs, ring.ys, ring.pointCount, style.landuseOutlinePx[index],
                                   style.landuseOutlineDashPx[index], style.landuseOutlineGapPx[index], MapInk::Black);
  }
}

// A height number waiting to be drawn, picked while the contour it belongs to
// streams past. Collected during the draw pass rather than in a second walk of
// the layer: the elevation and the geometry are both in the record, and a third
// read of a 39 KB layer to find them again buys nothing.
struct ContourLabelSlot {
  int32_t x = 0;
  int32_t y = 0;
  int32_t elevation = 0;
  // The uphill direction at that vertex, in screen pixels. Derived from the
  // order the contour's points are stored in, which the tile builder guarantees:
  // find_contours runs with positive_orientation="low", so higher ground is at
  // (-dy, dx) from the direction of travel (mapbuilder/tilegen/contour.py,
  // contour_lines). It costs no bytes in the tile and it is what lets the number
  // be turned so its top points up the slope.
  int32_t upX = 0;
  int32_t upY = 0;
  // |cross product| of the two segments meeting at the chosen vertex, scaled.
  // Lower is straighter, and straighter is where a number sits without looking
  // like it fell off the line.
  int32_t bend = 0;
  bool used = false;
};

// The straightest on-screen vertex of one contour, or none.
//
// Straightness is the cheap proxy for what a cartographer does by eye. A number
// placed on a hairpin reads as belonging to whichever arm the eye follows first,
// and on a 1-bit panel there is no second cue to fix that.
bool bestLabelVertex(const MapWayRef& line, int rectX, int rectY, int rectW, int rectH, int margin, int32_t& outX,
                     int32_t& outY, int32_t& outBend, int32_t& outUpX, int32_t& outUpY) {
  bool found = false;
  int32_t bestBend = 0;
  for (uint16_t i = 1; i + 1 < line.pointCount; ++i) {
    const int32_t x = line.xs[i];
    const int32_t y = line.ys[i];
    // Against the canvas's own drawable rect, not against 0..w/0..h: on the
    // device the header band is off limits and rectY is not zero
    // (IMapCanvas::drawableRect).
    if (x < rectX + margin || y < rectY + margin || x > rectX + rectW - margin || y > rectY + rectH - margin) {
      continue;
    }
    const int32_t ax = x - line.xs[i - 1];
    const int32_t ay = y - line.ys[i - 1];
    const int32_t bx = line.xs[i + 1] - x;
    const int32_t by = line.ys[i + 1] - y;
    int32_t bend = ax * by - ay * bx;
    if (bend < 0) bend = -bend;
    if (!found || bend < bestBend) {
      found = true;
      bestBend = bend;
      outX = x;
      outY = y;
      // Direction of travel across this vertex, then a quarter turn clockwise on
      // a screen whose y grows downward -- which is uphill, per contour.py.
      const int32_t tx = line.xs[i + 1] - line.xs[i - 1];
      const int32_t ty = line.ys[i + 1] - line.ys[i - 1];
      outUpX = -ty;
      outUpY = tx;
    }
  }
  outBend = bestBend;
  return found;
}

// One contour class, stroked. Called once per class for the same reason as
// landuse: minor and index share a single tile layer and sit at different
// depths, so the heavy line has to land on the fine one and not the other way
// round. A width of 0 is the style hiding this class at this rung, and it
// returns before walking the layer rather than reading records to drop them.
//
// `slots`, when given, collects height-number candidates from this class. Only
// the index pass passes it: a number on a minor contour would be a number every
// 20 m, which is the opposite of the two-or-three rule.
void drawContourClass(IMapCanvas& canvas, IMapSource& source, const MapStyle& style, const MapReliefClass wanted,
                      ContourLabelSlot* slots = nullptr, int slotCount = 0, uint32_t* drawn = nullptr) {
  const uint8_t index = static_cast<uint8_t>(wanted);
  if (index >= kReliefClassSlots || style.contourWidthPx[index] == 0) return;
  if (!source.beginContours()) return;
  int canvasX = 0, canvasY = 0, canvasW = 0, canvasH = 0;
  canvas.drawableRect(canvasX, canvasY, canvasW, canvasH);
  const int minGap = static_cast<int>(style.contourLabelMinGapPx);
  MapWayRef line;
  const int lineWidth = style.contourWidthPx[index];
  const MapLinePattern pattern = style.contourPattern[index];
  while (source.nextContour(line)) {
    if (line.classId != index) continue;
    // **The per-class pattern, which this loop used to ignore.** MapStyle grew
    // contourPattern/TickPx/GapPx/Mark/MarkPx and gen_mapstyle.py learned to parse
    // and emit them, and `strokeWayHachured` was wired into the roads pass only --
    // so a style could say `"pattern": "hachured"` on the cliff class, be
    // validated, and draw a plain line. Which is the exact shape of defect this
    // file keeps producing: a field the generator accepts and nothing reads.
    //
    // It mattered here more than most. The obvious first thing to do with the
    // cliff class is give it a comb, and at 2 px solid a cliff is the same mark as
    // an index contour -- one says "a hundred metres of height", the other says
    // "you fall here". A panel pass would have been judging a render the style did
    // not perform.
    switch (pattern) {
      case MapLinePattern::Hachured:
        strokeWayHachured(canvas, line, lineWidth, style.contourTickPx[index], style.contourGapPx[index],
                          MapInk::Black);
        break;
      case MapLinePattern::DashMark:
        strokeWayDashMark(canvas, line, lineWidth, style.contourDashPx[index], style.contourGapPx[index],
                          style.contourMark[index], style.contourMarkPx[index], MapInk::Black);
        break;
      case MapLinePattern::None:
        break;
      default:
        strokeWay(canvas, line, lineWidth, MapInk::Black);
        break;
    }
    if (drawn != nullptr) ++*drawn;
    if (slots == nullptr || style.contourLabelPx == 0) continue;
    int32_t vx = 0, vy = 0, bend = 0, upX = 0, upY = 0;
    // The margin has to cover half the *box*, not half the text height: a
    // four-digit height is about three label heights wide, and a margin of one
    // put "1000" half off the left edge. Three is that half-width with room,
    // and it needs no text measured before a vertex is chosen.
    if (!bestLabelVertex(line, canvasX, canvasY, canvasW, canvasH, static_cast<int>(style.contourLabelPx) * 3, vx, vy,
                         bend, upX, upY)) {
      continue;
    }
    const int32_t elevation = static_cast<int32_t>(static_cast<int16_t>(line.flags));
    int target = -1;
    bool duplicate = false;
    for (int i = 0; i < slotCount; ++i) {
      // Three numbers reading 900, 900, 900 say less than 900, 1000, 1200: the
      // second set gives the direction of the slope for free. A level already
      // claimed elsewhere on the frame does not get a second number.
      if (slots[i].used && slots[i].elevation == elevation) duplicate = true;
    }
    if (duplicate) continue;
    for (int i = 0; i < slotCount; ++i) {
      if (!slots[i].used) {
        if (target < 0) target = i;
        continue;
      }
      const int32_t dx = slots[i].x - vx;
      const int32_t dy = slots[i].y - vy;
      if (dx * dx + dy * dy < static_cast<int32_t>(minGap) * minGap) {
        // Too close to a number already claimed. Keep the straighter of the
        // two rather than whichever the stream offered first.
        if (bend < slots[i].bend) {
          slots[i] = ContourLabelSlot{vx, vy, elevation, upX, upY, bend, true};
        }
        target = -1;
        break;
      }
    }
    if (target >= 0) {
      slots[target] = ContourLabelSlot{vx, vy, elevation, upX, upY, bend, true};
    }
  }
}

// The numbers, drawn after the place names so a settlement's name wins the
// space.
//
// A 1 px white outline around the digits, not a white box behind them. The box
// was the first attempt and it reads as a hole punched in the map: it wipes the
// contour, the forest tone and anything else under a rectangle far bigger than
// the strokes need. The outline knocks out only what the digits themselves
// occupy, so the line still runs visibly behind the number
// (maintainer's call, 2026-08-26). It is drawn by the canvas from the same
// rotated mask, so it follows the number's angle -- eight extra draws at offsets
// would not.
void drawContourLabels(IMapCanvas& canvas, const MapStyle& style, MapLabelScratch* labels,
                       const ContourLabelSlot* slots, int slotCount, uint32_t* placedOut = nullptr) {
  if (style.contourLabelPx == 0) return;
  const int sizePx = static_cast<int>(style.contourLabelPx);
  const bool bold = style.contourLabelBold;
  int placed = 0;
  for (int i = 0; i < slotCount && placed < static_cast<int>(style.contourLabelMax); ++i) {
    if (!slots[i].used) continue;
    char text[8];
    snprintf(text, sizeof(text), "%ld", static_cast<long>(slots[i].elevation));
    int textW = 0, textH = 0;
    if (!canvas.measureText(text, sizePx, bold, textW, textH)) continue;
    // **The number sits along the contour, at the line's own bearing at that
    // spot, with the digits' top toward the higher ground.** Not quantised: a
    // contour running south-east given a number turned due south is exactly the
    // mismatch a map reader notices first, because the point of the label is that
    // it and the piece of line under it read as one thing.
    //
    // One vector decides all of it. Uphill is (-dy, dx) from the order the tile
    // builder stores contour points in (mapbuilder/tilegen/contour.py), and
    // mapTextBasisFromUp turns that into the basis -- which is where this used to
    // go wrong: giving it the tangent and the uphill normal separately let the two
    // disagree, and that pair is a reflection, so every number came out mirrored.
    // From `up` alone the determinant is positive by construction.
    const int32_t ux = slots[i].upX;
    const int32_t uy = slots[i].upY;
    int rightX = 1024, rightY = 0, downX = 0, downY = 1024;
    mapTextBasisFromUp(ux, uy, rightX, rightY, downX, downY);

    // The claimed box is the rotated extent, so two numbers at different bearings
    // cannot be judged as if both were horizontal.
    const int32_t absR = rightX < 0 ? -rightX : rightX;
    const int32_t absRY = rightY < 0 ? -rightY : rightY;
    const int spanW = static_cast<int>((textW * absR + textH * absRY) >> 10) + 2;
    const int spanH = static_cast<int>((textW * absRY + textH * absR) >> 10) + 2;

    // **On the line, not beside it.** The number is centred on the vertex, so the
    // contour runs through it and the 1 px white outline breaks the line only
    // where the digits actually are. Sitting the label off to one side put its
    // baseline above the contour and read as a caption for it rather than as part
    // of it (maintainer's call, 2026-08-26).
    //
    // An earlier version stepped off to the uphill side deliberately, because the
    // turn was quantised then and the offset was the only thing carrying the slope
    // direction. The rotation carries it now, so the offset is redundant as well
    // as wrong.
    const int centreX = static_cast<int>(slots[i].x);
    const int centreY = static_cast<int>(slots[i].y);
    const int boxX = centreX - spanW / 2;
    const int boxY = centreY - spanH / 2;
    const int boxW = spanW;
    const int boxH = spanH;
    // Yield to a place name that already claimed this ground. A height is
    // countable from the next one along; a settlement's name is not.
    if (labels != nullptr && labels->taken.anySet(boxX, boxY, boxW, boxH)) continue;

    // One call: the canvas draws the 1 px white outline from the same mask, so the
    // outline follows the rotation instead of being eight more draws at the wrong
    // angle.
    if (!canvas.drawTextRotated(centreX, centreY, text, sizePx, bold, MapInk::Black,
                                static_cast<int>(style.contourLabelHaloPx), static_cast<int>(rightX),
                                static_cast<int>(rightY), static_cast<int>(downX), static_cast<int>(downY))) {
      // Nothing was inked, so nothing may be claimed: marking the rect here let a
      // label that does not exist block a place name, and counting it spent one of
      // contourLabelMax on empty ground. The canvas logs why.
      continue;
    }
    if (labels != nullptr) labels->taken.markRect(boxX, boxY, boxW, boxH);
    ++placed;
  }
  if (placedOut != nullptr) *placedOut = static_cast<uint32_t>(placed);
}

// The route: one thick black polyline, with a filled arrowhead at the far end
// (docs/map-render-spec.md item 3). Streamed, never collected -- two points are
// live at a time whatever the route's length (IMapRouteSource.h).
//
// Coordinates are int32_t. In follow mode the far end of a long route is
// millions of pixels off screen, and the canvas clips per segment, so an honest
// off-screen coordinate is cheap while a wrapped int16_t one is a line drawn
// across the middle of the map.
// `occupancy`, when given, is marked with every route segment as it is drawn.
// The label placer needs to know where the route is and there is no second
// cheap way to find out: the route lives in a file, and walking it again is a
// second pass over SD (IMapRouteSource.h). So the pass that draws it records it.
// Dither the inside of a cased road, one segment at a time.
//
// A stroke is not a ring, so it cannot go through toneRing() whole -- but each
// segment's interior *is* a quad, and a quad is a ring with four corners. That
// is the whole trick: no new canvas primitive, no new fill code, and the tone
// stays the same screen-anchored dither an area uses, so a road and a built-up
// area under it cannot drift out of phase.
//
// The offset here is the true perpendicular, unlike MapStroke's stacking. The
// reason MapStroke avoids it does not apply: it stripes when you stack 1 px
// *lines* 1.41 px apart on a diagonal, and this is a filled quad with no gaps
// to stripe.
//
// Joints are left as overlapping quads rather than mitred. Two consequences,
// both checked by looking: the overlap costs nothing because the tone is a
// position test and painting a pixel twice is the same pixel, and the outside
// of a bend keeps a small unfilled wedge. On a texture that is at most every
// other pixel, that wedge is far less visible than it would be in a solid fill.
void toneWayInterior(IMapCanvas& canvas, const MapWayRef& way, const int innerWidth, const MapAreaTone tone) {
  // Below 2 px there is no interior to texture: a period-2 tone needs two
  // pixels to say anything and the period-3 stipple needs three.
  if (tone == MapAreaTone::None || innerWidth < 2 || way.pointCount < 2) return;
  const double half = innerWidth * 0.5;
  for (uint16_t i = 1; i < way.pointCount; ++i) {
    const double x0 = way.xs[i - 1], y0 = way.ys[i - 1];
    const double x1 = way.xs[i], y1 = way.ys[i];
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.5) continue;  // a zero-length segment has no perpendicular
    const double nx = -dy / len * half, ny = dx / len * half;
    const int16_t rx[5] = {static_cast<int16_t>(std::lround(x0 + nx)), static_cast<int16_t>(std::lround(x1 + nx)),
                           static_cast<int16_t>(std::lround(x1 - nx)), static_cast<int16_t>(std::lround(x0 - nx)),
                           static_cast<int16_t>(std::lround(x0 + nx))};
    const int16_t ry[5] = {static_cast<int16_t>(std::lround(y0 + ny)), static_cast<int16_t>(std::lround(y1 + ny)),
                           static_cast<int16_t>(std::lround(y1 - ny)), static_cast<int16_t>(std::lround(y0 - ny)),
                           static_cast<int16_t>(std::lround(y0 + ny))};
    MapAreaFill::toneRing(canvas, rx, ry, 5, tone);
  }
}

void drawRoute(IMapCanvas& canvas, IMapRouteSource& route, const MapStyle& style, MapOccupancyGrid* occupancy) {
  if (style.routeWidthPx == 0) return;
  if (!route.beginRoute()) return;

  int32_t x = 0;
  int32_t y = 0;
  if (!route.nextRoutePoint(x, y)) return;

  int32_t prevX = x;
  int32_t prevY = y;
  // The segment before the last point, kept for the arrowhead's direction. A
  // route is at least two points (the format enforces it), so by the end of the
  // walk this is always a real segment.
  int32_t tailX = x;
  int32_t tailY = y;
  bool drewAny = false;

  while (route.nextRoutePoint(x, y)) {
    canvas.drawLine(static_cast<int>(prevX), static_cast<int>(prevY), static_cast<int>(x), static_cast<int>(y),
                    style.routeWidthPx, MapInk::Black);
    if (occupancy != nullptr) occupancy->markSegment(prevX, prevY, x, y, style.routeWidthPx / 2);
    tailX = prevX;
    tailY = prevY;
    prevX = x;
    prevY = y;
    drewAny = true;
  }
  if (!drewAny) return;

  // Arrowhead at the far end, pointing along the last segment. `arrow_len_px`
  // is tip-to-base and `arrow_width_px` is the base, both device pixels: screen
  // decorations stay legible at any map zoom, so they do not scale with mpp
  // (data/mapstyle.json, layers.route).
  const int arrowLen = style.routeArrowLenPx;
  const int arrowWidth = style.routeArrowWidthPx;
  if (arrowLen <= 0 || arrowWidth <= 0) return;

  const double dx = static_cast<double>(prevX - tailX);
  const double dy = static_cast<double>(prevY - tailY);
  const double len = std::sqrt(dx * dx + dy * dy);
  // A zero-length last segment has no direction to point along. Two identical
  // points in a row are deduped by the builder, so this is a corrupt file rather
  // than a normal route -- draw the line, skip the head.
  if (len < 1e-6) return;
  const double ux = dx / len;
  const double uy = dy / len;

  const double baseX = static_cast<double>(prevX) - ux * arrowLen;
  const double baseY = static_cast<double>(prevY) - uy * arrowLen;
  const double halfWidth = arrowWidth / 2.0;
  // Perpendicular, same length scale.
  const double px = -uy * halfWidth;
  const double py = ux * halfWidth;

  const int xs[3] = {static_cast<int>(prevX), static_cast<int>(std::lround(baseX + px)),
                     static_cast<int>(std::lround(baseX - px))};
  const int ys[3] = {static_cast<int>(prevY), static_cast<int>(std::lround(baseY + py)),
                     static_cast<int>(std::lround(baseY - py))};
  canvas.fillPolygon(xs, ys, 3, MapInk::Black);
}

}  // namespace

void MapRenderer::render(IMapCanvas& canvas, IMapSource& source, const MapViewState& state, const MapStyle& style,
                         IMapRouteSource* route, MapRenderTiming* timing, MapNearestPlaces* nearestOut,
                         MapLabelScratch* labels, IMapPointSource* points) {
  if (labels != nullptr) labels->reset();
  // Instrumentation only. `mark` holds the last stamp; each layer's field gets
  // the delta since it. With no timing, or no clock in it, both of these are a
  // null check and nothing else -- no clock call, no pixel changed.
  const auto stamp = [timing]() -> uint32_t { return (timing && timing->nowMs) ? timing->nowMs() : 0; };
  const auto lap = [&stamp](uint32_t& field, uint32_t& mark) {
    const uint32_t now = stamp();
    field = now - mark;
    mark = now;
  };
  uint32_t mark = stamp();

  // Draw order is fixed and not arbitrary (docs/map-data-spec.md, "A tile is a
  // storage unit, not a render unit"): built-up area, green area, water,
  // buildings, road casings, road fills, then places. Landuse tones are
  // background; everything above has to stay readable over them, which is why a
  // tone is a sparse pattern and not a grey block.
  //
  // A layer the style has off is never read, not read and skipped: buildings
  // alone were 277 KB of the 364 KB a four-tile viewport read
  // (docs/map-data-spec.md, "RAM budget").
  if (style.landuseEnabled) {
    // Two walks over one layer, built-up first: a park inside a housing estate
    // has to land on top of it, and a single walk would draw them in whatever
    // order the tile happens to store.
    // Built-up is per rung, via the style (its landuse rule is hidden at rung 0,
    // where the buildings themselves carry the settlement). drawLanduseClass
    // already returns without a card walk when a class draws nothing, so there
    // is no flag to test here. Forest is drawn at every
    // rung, because no building shows where a wood is.
    drawLanduseClass(canvas, source, style, MapLanduseClass::BuiltUp);
    drawLanduseClass(canvas, source, style, MapLanduseClass::Forest);
  }
  if (timing) lap(timing->landuseMs, mark);

  // Contours sit over the landuse wash and under everything else. Over, because
  // a contour hidden under forest hatch is not a contour; under, because a
  // contour crossing a road or a river must not break it -- black over black is
  // nothing (docs/map-render-spec.md, "1-bit rules") and the wider feature has
  // to win. Minor first, so an index line lands on top where the two touch.
  // Room for a couple more candidates than may be drawn, so the min-gap filter
  // has something to choose between rather than taking the first that fits.
  ContourLabelSlot contourLabels[6] = {};
  if (style.contoursEnabled) {
    uint32_t* const lineCount = timing != nullptr ? &timing->contourLines : nullptr;
    drawContourClass(canvas, source, style, MapReliefClass::ContourMinor, nullptr, 0, lineCount);
    drawContourClass(canvas, source, style, MapReliefClass::ContourIndex, contourLabels,
                     static_cast<int>(sizeof(contourLabels) / sizeof(contourLabels[0])), lineCount);
    // Cliffs last, so the one line that means "you cannot go this way" lands on
    // top of the height lines it crosses. No label slots: `flags` on a cliff
    // record is 0 rather than an elevation (tilegen relief_class.py), so a
    // number hung on one would read "0 m".
    drawContourClass(canvas, source, style, MapReliefClass::Cliff, nullptr, 0, lineCount);
  }
  if (timing) lap(timing->contoursMs, mark);

  // Both gates read: the style says whether buildings are drawn at all, the view
  // says whether this rung draws them. Either one false and the layer is not
  // opened -- and not opening it is what saves the card read, not just the
  // drawing (MapStyle::buildingsEnabled, which is per rung since 2026-08-25).
  if (style.buildingsEnabled && source.beginBuildings()) {
    MapWayRef ring;
    while (source.nextBuilding(ring)) {
      // Tone or hatch, whichever the style chose, then the outline on top so a
      // building keeps a crisp edge over its own texture.
      MapAreaFill::toneRing(canvas, ring.xs, ring.ys, ring.pointCount, style.buildingTone);
      MapAreaFill::hatchRing(canvas, ring.xs, ring.ys, ring.pointCount, style.buildingHatch,
                             style.buildingHatchSpacingPx, MapInk::Black);
      MapAreaFill::outlineRing(canvas, ring.xs, ring.ys, ring.pointCount, style.buildingOutlinePx, MapInk::Black);
    }
  }
  if (timing) lap(timing->buildingsMs, mark);

  if (style.waterEnabled && source.beginWater()) {
    MapWayRef way;
    while (source.nextWater(way)) {
      // A closed ring is a lake, an open one a waterway. Same layer, same
      // record shape -- the ring is the only thing that tells them apart
      // (IMapSource.h, mapWayIsClosedRing).
      // Width per water class now that tiles carry one: a river is not a
      // ditch. A class byte past the table can only come from a corrupt
      // record.
      const uint8_t waterClass = way.classId < kWaterClassSlots ? way.classId : 0;
      const int lineWidth = style.waterLinePx[waterClass];
      const MapAreaTone tone = style.waterTone[waterClass];
      if (mapWayIsClosedRing(way)) {
        MapAreaFill::toneRing(canvas, way.xs, way.ys, way.pointCount, tone);
        // Tone first, then the pattern knocked out of it in white -- the order
        // is what makes waves on water rather than waves under it.
        MapAreaFill::hatchRing(canvas, way.xs, way.ys, way.pointCount, style.waterHatch, style.waterHatchSpacingPx,
                               style.waterHatchWhite ? MapInk::White : MapInk::Black);
        // The ring's own border width, not the class's line width. A lake edge
        // and a river's stroke are different decisions and used to share one
        // number (MapStyle::waterOutlinePx).
        MapAreaFill::outlineRing(canvas, way.xs, way.ys, way.pointCount, style.waterOutlinePx[waterClass],
                                 MapInk::Black);
      } else if (tone != MapAreaTone::None) {
        // **A watercourse that carries a surface.** `mapWayIsClosedRing` is the
        // only thing separating a lake or an area-mapped river from a stream, and
        // a stream is always an open way -- so before 2026-08-27 `fill: tone` on a
        // stream rule drew nothing at all, because toneRing is reached only above.
        // A walker crossing a stream cares as much as a rider crossing the
        // Danube, and width alone could not say it.
        //
        // `casing_px` decides whether the ribbon has edges, and 0 is a real answer
        // rather than a missing one (maintainer's call 2026-08-27: "I want the
        // interior to eat the edge too, I do not want a border").
        const int casing = style.waterCasingPx[waterClass];
        if (casing > 0) {
          // Edged: the same three steps a cased road takes, deliberately the same
          // ones rather than a second way of saying it. Black at the full width,
          // white at the interior, then the tone laid into the cleared middle. The
          // generator guarantees 2 * casing < width, so the interior is >= 1.
          const int inner = lineWidth - 2 * casing;
          strokeWay(canvas, way, lineWidth, MapInk::Black);
          strokeWay(canvas, way, inner, MapInk::White);
          toneWayInterior(canvas, way, inner, tone);
        } else {
          // Edgeless: the tone is the whole stroke, full width, no black rim. What
          // this buys is that the mark reads as a *surface* and not as a channel
          // with banks -- a 7 px ribbon with 2 px of black on each side is mostly
          // rim, and at that point the tone is a detail inside a border rather
          // than the thing itself.
          toneWayInterior(canvas, way, lineWidth, tone);
        }
        // The dash still applies, over the finished surface rather than instead of
        // it: a dashed watercourse is a broken ribbon, which is what an
        // intermittent stream should look like.
        if (style.waterPattern[waterClass] == MapLinePattern::Dashed) {
          strokeWayDashed(canvas, way, lineWidth, style.waterDashPx[waterClass], style.waterGapPx[waterClass],
                          MapInk::Black);
        }
      } else if (style.waterPattern[waterClass] == MapLinePattern::Dashed) {
        strokeWayDashed(canvas, way, lineWidth, style.waterDashPx[waterClass], style.waterGapPx[waterClass],
                        MapInk::Black);
      } else if (style.waterPattern[waterClass] != MapLinePattern::None) {
        MapAreaFill::outlineRing(canvas, way.xs, way.ys, way.pointCount, lineWidth, MapInk::Black);
      }
      // `None` with no tone draws nothing, which is a style saying so rather than
      // a mistake -- `hidden: true` is the way to also stop reading the class.

    }
  }
  if (timing) lap(timing->waterMs, mark);

  // One pass per layer, each pass pulled straight from the source and
  // forgotten. Nothing is collected first -- see IMapSource.h.
  //
  // Roads are two passes over the same layer, and the order is a correctness
  // requirement, not a preference: **all black strokes first, then all white
  // fills**. Finishing one road before starting the next leaves a broken
  // casing at every junction, because the later road's white fill punches
  // through the earlier road's black edge (docs/map-render-spec.md, "What must
  // be drawn"). beginWays() is rewindable exactly so this can be two walks
  // instead of one buffered pass (IMapSource.h:22-27).
  if (source.beginWays()) {
    MapWayRef way;
    while (source.nextWay(way)) {
      // Width 0 is the style hiding this class outright (mapstyle.json's
      // `hidden`). Distinct from the mode mask, which drops the way earlier,
      // in the source, and per travel mode rather than for every mode.
      const RoadStroke stroke = roadStrokeFor(style, way);
      if (stroke.pattern == MapLinePattern::Dashed) {
        strokeWayDashed(canvas, way, stroke.width, stroke.dash, stroke.gap, MapInk::Black);
      } else if (stroke.pattern == MapLinePattern::DashMark) {
        strokeWayDashMark(canvas, way, stroke.width, stroke.dash, stroke.gap, stroke.mark, stroke.markPx,
                          MapInk::Black);
      } else if (stroke.pattern == MapLinePattern::Hachured) {
        // Wired here too, not only on the relief layer: a road class can want it
        // (an embankment, a cutting), and without this branch it fell through to a
        // plain stroke -- the pattern silently doing nothing, which is the failure
        // mode this file keeps having to be saved from. `dash` is the comb reach on
        // a road, since a hachured line has no dash of its own.
        strokeWayHachured(canvas, way, stroke.width, stroke.dash, stroke.gap, MapInk::Black);
      } else if (stroke.pattern != MapLinePattern::None) {
        strokeWay(canvas, way, stroke.width, MapInk::Black);
      }
    }
  }

  // Second walk, white: the inside of every cased road. A class with
  // casing 0 is not touched here, so it stays the solid black line the first
  // pass drew.
  if (source.beginWays()) {
    MapWayRef way;
    while (source.nextWay(way)) {
      const RoadStroke stroke = roadStrokeFor(style, way);
      const int lineWidth = stroke.width;
      if (lineWidth == 0) continue;
      const int casing = stroke.casing;
      if (casing > 0) {
        // The generator guarantees 2 * casing < width, so this is at least 1.
        const int inner = lineWidth - 2 * casing;
        strokeWay(canvas, way, inner, MapInk::White);
        // The tone goes on after that white, never instead of it: the white is
        // what clears the first pass's black, and the tone is a texture laid
        // into the cleared middle.
        toneWayInterior(canvas, way, inner, stroke.tone);
      }
      // Sleepers, drawn in this pass rather than a third one. They have to
      // land after their own way's white fill or that fill erases them, and
      // doing it here costs nothing; a third walk would re-read the whole
      // roads layer off the SD card for a few ticks (kRoadPasses is
      // load-bearing -- MapTileSource and MapTileReader size their work by it).
      //
      // The residue: a *later* cased road crossing this one paints its white
      // fill over these ticks. That is a level crossing, where the road is
      // meant to read as on top anyway, so it looks right rather than broken.
      if (stroke.pattern == MapLinePattern::Ticked) {
        strokeWayDashed(canvas, way, lineWidth, stroke.dash, stroke.gap, MapInk::Black);
      }
    }
  }
  if (timing) lap(timing->roadsMs, mark);

  // The route sits over the roads it follows and under the place dots -- the
  // draw order docs/map-data-spec.md fixes ("built-up area, green area, water,
  // buildings, roads, route, junctions, places"). Over the roads because a
  // route hidden under a casing is not a route; under the dots because a dot on
  // the route is exactly what item 4 of the render spec draws.
  if (route != nullptr) drawRoute(canvas, *route, style, labels != nullptr ? &labels->route : nullptr);
  if (timing) lap(timing->routeMs, mark);

  // The panel, handed to the label scratch once per frame so offer() can refuse
  // a place that could never be named (MapLabels.h, MapLabelScratch::setClip).
  if (labels != nullptr) {
    int clipX = 0, clipY = 0, clipW = 0, clipH = 0;
    canvas.drawableRect(clipX, clipY, clipW, clipH);
    labels->setClip(clipX, clipY, clipW, clipH);
  }

  const int dotDiameter = style.placeDotDiameterPx;
  if (nearestOut) *nearestOut = MapNearestPlaces{};
  // Rank <= 1 is city/town (mapbuilder/build_config.json's place_ranks:
  // city=0, town=1); everything above is the fine-grained tier
  // (village/suburb/hamlet/farm). Opened even with dots hidden
  // (`nearestOut != nullptr` alone opens the layer) -- see MapRenderer.h.
  constexpr uint8_t kCoarseMaxRank = 1;
  if ((dotDiameter > 0 || nearestOut != nullptr || labels != nullptr) && source.beginPlaces()) {
    MapPlaceRef place;
    long bestFineDistSq = -1;
    long bestCoarseDistSq = -1;
    while (source.nextPlace(place)) {
      if (dotDiameter > 0) {
        canvas.fillRoundedRect(place.x - dotDiameter / 2, place.y - dotDiameter / 2, dotDiameter, dotDiameter,
                               dotDiameter / 2, MapInk::Black);
      }
      // Labels cannot be drawn inside this walk: they are placed rank-first and
      // each one has to know where the ones before it went, which is not known
      // until the layer has been walked to the end. So the walk only collects,
      // and the layout runs after it (MapLabels.h).
      //
      // Layout is measured from the viewport anchor, never from state.markerX/Y:
      // the marker moves between redraws and the map underneath must not
      // (docs/map-render-spec.md).
      if (labels != nullptr) MapLabels::offer(*labels, place, style.markerXPx, style.markerYPx);
      if (nearestOut == nullptr || place.name[0] == '\0') continue;
      const long dx = place.x - state.markerX;
      const long dy = place.y - state.markerY;
      const long distSq = dx * dx + dy * dy;
      if (place.rank <= kCoarseMaxRank) {
        if (bestCoarseDistSq >= 0 && distSq >= bestCoarseDistSq) continue;
        bestCoarseDistSq = distSq;
        snprintf(nearestOut->coarseName, MapNearestPlaces::kNameBufferLen, "%s", place.name);
        nearestOut->hasCoarse = true;
      } else {
        if (bestFineDistSq >= 0 && distSq >= bestFineDistSq) continue;
        bestFineDistSq = distSq;
        snprintf(nearestOut->fineName, MapNearestPlaces::kNameBufferLen, "%s", place.name);
        nearestOut->hasFine = true;
      }
    }
  }
  if (timing) lap(timing->placesMs, mark);

  // The POI marks: over the place dots, under the names. A square says "this
  // exists in the data" and a dot says "this is a settlement"
  // (docs/map-render-spec.md, "Point mark vocabulary"), so the bigger mark goes
  // on top -- a square half hidden by a place dot reads as neither.
  //
  // The source decides which categories are in the walk
  // (MapPointSource::Config::categoryMask): `Nearby -> Show on map` is a
  // temporary view, so it filters the walk and never the style.
  if (points != nullptr && style.pointSquarePx > 0 && points->beginMapPoints()) {
    MapPointMarks::drawAll(canvas, *points, style);
  }
  if (timing) lap(timing->pointsMs, mark);

  // Names last, over everything the map drew and under the marker the caller
  // draws next. A label is the only thing here that is placed rather than
  // simply drawn, so it has to see the finished picture (MapLabels.h).
  if (labels != nullptr) MapLabels::draw(canvas, *labels, style);
  if (style.contoursEnabled) {
    drawContourLabels(canvas, style, labels, contourLabels,
                      static_cast<int>(sizeof(contourLabels) / sizeof(contourLabels[0])),
                      timing != nullptr ? &timing->contourLabels : nullptr);
  }
  if (timing) lap(timing->labelsMs, mark);

  // No marker draw here -- MapActivity draws its own mode-specific one (ring +
  // dot/arrow, sized per hike/cycle/ride) after this call returns. Drawing the
  // generic puck here as well left it peeking out from under the real marker
  // whenever the real one was smaller, e.g. hike's dot.
  //
  // The reason that marker bypasses IMapCanvas no longer holds: it needed a
  // white halo fill, and IMapCanvas can paint white now (MapInk). Moving it
  // behind the canvas is what would make the laptop preview show the marker
  // the device actually draws -- until then the preview shows the style's puck
  // (drawMarker below) and the device shows the mode marker. See
  // docs/map-style.md.
  (void)state;
}

void MapRenderer::drawMarker(IMapCanvas& canvas, int16_t cx, int16_t cy, MapHeading heading, const MapStyle& style) {
  // Puck under the arrow: a black disc, then a white one inside it, which
  // leaves a ring of `puckRingPx`. Radius 0 skips both and draws the bare
  // arrow -- see MapStyle.h.
  const int radius = style.puckRadiusPx;
  if (radius > 0) {
    const int diameter = 2 * radius;
    canvas.fillRoundedRect(cx - radius, cy - radius, diameter, diameter, radius, MapInk::Black);
    const int inner = radius - style.puckRingPx;
    if (inner > 0) {
      canvas.fillRoundedRect(cx - inner, cy - inner, 2 * inner, 2 * inner, inner, MapInk::White);
    }
  }

  // Arrow proportions off one style number: `arrow_px` is the tip-to-tail
  // length, the tail sits a quarter of it behind the centre, and the base is
  // half of it wide. At the style's 28 px that reproduces the triangle this
  // drew before the puck existed (14 forward, 7 back, 7 to each side).
  const int arrow = style.puckArrowPx;
  if (arrow <= 0) return;
  const int tipLen = arrow / 2;
  const int tailLen = arrow / 4;
  const int halfWidth = arrow / 4;

  const Vec2& dir = kHeadingDir[static_cast<uint8_t>(heading)];
  const Vec2 perp{-dir.dy, dir.dx};  // rotate 90 degrees, same /8 scale

  const int tipX = cx + dir.dx * tipLen / 8;
  const int tipY = cy + dir.dy * tipLen / 8;
  const int baseCx = cx - dir.dx * tailLen / 8;
  const int baseCy = cy - dir.dy * tailLen / 8;
  const int baseLeftX = baseCx + perp.dx * halfWidth / 8;
  const int baseLeftY = baseCy + perp.dy * halfWidth / 8;
  const int baseRightX = baseCx - perp.dx * halfWidth / 8;
  const int baseRightY = baseCy - perp.dy * halfWidth / 8;

  const int xs[3] = {tipX, baseLeftX, baseRightX};
  const int ys[3] = {tipY, baseLeftY, baseRightY};
  canvas.fillPolygon(xs, ys, 3, MapInk::Black);
}
