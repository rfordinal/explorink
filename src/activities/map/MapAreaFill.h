#pragma once

#include <cstdint>

#include "IMapCanvas.h"
#include "MapAreaTone.h"

// Hatch fill for a closed ring: buildings and water areas.
//
// **Never a solid fill.** A solid black building on 1-bit e-ink swallows the
// roads around it and reads as a hole in the map (docs/map-render-spec.md,
// "What must be drawn": outlines and hatch, never solid fills). A hatch says
// "built-up here" while leaving the road network legible through it.
//
// Drawn as hatch lines clipped to the ring, using IMapCanvas::drawLine only.
// No new canvas primitive, which matters twice: the device gets it without a
// scanline-fill callback GfxRenderer does not have, and the laptop preview
// gets exactly the same pixels because both run this same code.
namespace MapAreaFill {

// Which way the hatch lines run. From mapstyle.json's matplotlib hatch string:
// `/` diagonal, `\` antidiagonal, `-` horizontal, `|` vertical, `X` or `x`
// cross (horizontal and vertical together). The repeat count in a string like
// "XXXX" is a matplotlib density knob and has no meaning here -- spacing comes
// from hatch_spacing_px, in device pixels like every other length.
// `Wave` is the water one: a row of tildes rather than a straight rule, so a
// filled river cannot be mistaken for a filled anything else. Drawn as a
// triangle wave, not as a `~` glyph from a font -- IMapCanvas has no text
// primitive (drawLine, fillRoundedRect, fillPolygon, fillSpan) and the host
// preview's PpmCanvas has no font system at all, so a glyph would have to be
// plumbed through both before it could be judged. When map labels arrive and
// IMapCanvas grows text, a real `~` becomes an option worth revisiting.
enum class Pattern : uint8_t { None = 0, Diagonal, AntiDiagonal, Horizontal, Vertical, Cross, Wave };

// Fills the ring with a flat tone (MapAreaTone.h): scan lines paired the same
// way the hatch is, but painted as spans rather than as lines. This is what a
// built-up area should use -- a period-2 or period-3 pixel pattern reads as grey
// at arm's length, while hatch lines read as scratches, and at map zooms a
// building is a few pixels across and far too small to carry a line pattern.
void toneRing(IMapCanvas& canvas, const int16_t* xs, const int16_t* ys, uint16_t pointCount, MapAreaTone tone);

// Points are the ring as stored in a tile: closed, first point repeated last.
// `spacingPx` is the gap between hatch lines. `maxPoints` bounds the scratch
// this needs, which is why it is a compile-time constant rather than a heap
// allocation -- MapTileReader::kMaxWayPoints ways come in on the source's own
// buffer and this must not add a second one that grows with them.
inline constexpr int kMaxCrossings = 32;

// Draws the hatch. Silently does nothing for a degenerate ring, a None
// pattern, or a spacing of 0 -- all three mean "no fill", and a fill that
// throws away its own guard conditions ends up drawing a solid black blob.
void hatchRing(IMapCanvas& canvas, const int16_t* xs, const int16_t* ys, uint16_t pointCount, Pattern pattern,
               int spacingPx, MapInk ink);

// The ring's own outline, `lineWidth` px. Separate from the hatch because
// mapstyle.json has separate numbers for them (outline_width and hatch), and a
// style may want one without the other.
void outlineRing(IMapCanvas& canvas, const int16_t* xs, const int16_t* ys, uint16_t pointCount, int lineWidth,
                 MapInk ink);

}  // namespace MapAreaFill
