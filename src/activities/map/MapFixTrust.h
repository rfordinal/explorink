#pragma once

#include <cstdint>

// How much the marker is allowed to claim about a fix.
//
// Two independent questions, and the marker answers them with two independent
// parts of its own shape:
//
//   - **Where am I** -- the ring. Whole ring, or a ring broken into eight arcs.
//   - **Which way do I face** -- the centre glyph. Today's hand/arrow, a wedge,
//     or nothing at all.
//
// ## Why this is its own header
//
// Pure arithmetic, no Arduino, no driver, same shape and same reason as
// MapFollow::decide() and MapGnssHeading::stepFor() next door: a decision worth
// testing on the host, and one a second client has to be able to reproduce
// exactly (CLAUDE.md, "The phone app must stay portable to iOS"). MapActivity
// owns the state and calls in.
//
// It is also the seam the sources hang off. A BLE fix carries metres and a
// bearing-accuracy code; a GNSS fix carries HDOP, satellites and a GGA quality
// digit. Neither vocabulary reaches drawPositionMarker(): both are turned into
// the enums below first, so the renderer never learns what a receiver is.
//
// ## Why the drawn states are categorical, not a gauge
//
// The panel is 1-bit and it is read at arm's length in sunlight. A reader can
// see *whether* a ring is broken with nothing to compare against; a reader
// cannot read a *value* off a stroke width without a second stroke beside it.
// So every channel here is a small set of named states, never a continuous
// quantity mapped onto a size.
//
// It also keeps the panel still. Every change to the marker's shape costs a
// windowed refresh, and on an X4 that costs the same as a full one whatever its
// area -- ~500 ms, measured 2026-08-05, docs/map-follow.md. On a T5 S3 Pro it is
// dearer than a full frame rather than equal to it: ~1,030 ms windowed against
// ~545 ms whole-panel (docs/refresh-modes.md, measured 2026-09-07), because
// LgfxEpdDriver does not override displayWindow() and every window request there
// falls through to a whole-panel push. That is a driver gap, not a panel law --
// docs/t5s3-partial-refresh.md is the plan to close it. A
// continuous mapping would repaint the marker on accuracy noise alone, with the
// rider parked. Hence the hysteresis below, and hence three states rather than
// ten.
namespace MapFixTrust {

// How much the ring may claim about position.
//
// `Unstated` is not "bad" and not a fallback -- it is a real answer, and it
// means the source does not speak this vocabulary at all. It draws as today's
// marker. A client that knows nothing about accuracy must not look like a
// client reporting a fault, which is what would happen if the default state
// were the broken ring. See dirTrustFromWireCode() for the same rule on the
// wire.
enum class Pos : uint8_t {
  Unstated,  // no accuracy figure at all -- draw the marker as it was before this existed
  Trusted,   // the true position is inside the drawn marker
  Loose,     // it is not; the ring breaks and stops pretending
};

// How much the centre glyph may claim about heading.
enum class Dir : uint8_t {
  Unstated,  // source does not report heading quality -- draw today's glyph
  Good,      // within one heading step; today's hand or arrow
  Coarse,    // somewhere in a neighbourhood; a wedge
  Unknown,   // nothing believable; draw no heading at all
};

struct Trust {
  Pos pos = Pos::Unstated;
  Dir dir = Dir::Unstated;
};

// ## The position thresholds, and where the numbers come from
//
// Not chosen by feel. The marker's ring has a radius of 27 px
// (kMarkerRingDiameter / 2) and the finest zoom rung draws 1 m of ground per
// pixel (MapViewport::kZoomLadder[0]). So **a 27 m error is exactly the error
// that still fits inside the drawn marker at the closest the device ever
// zooms** -- at that point the true position is somewhere under the glyph, and
// the marker is not lying by being drawn whole. Rounded down to 25 m.
//
// The marker cannot express a precision finer than itself, so this is the only
// non-arbitrary place to put the line.
//
// Beware: this makes `Trusted` generous for a phone, which typically reports 5
// to 15 m under open sky. That is intended. The broken ring is an alarm, not a
// quality meter -- it should be quiet on a normal ride and fire in a street
// canyon, a tunnel mouth or indoors.
inline constexpr uint16_t kLooseAtOrAboveM = 25;
// Coming back is harder than going: 7 m of gap, so a fix hovering at the line
// does not repaint the marker on every packet. Without this the panel would
// take a refresh per fix with the rider standing still -- ~500 ms on an X4,
// ~1,081 ms on a T5 S3 Pro (docs/refresh-modes.md).
inline constexpr uint16_t kTrustedAtOrBelowM = 18;

// Accuracy is carried on the wire as a saturating byte, and zero has always
// meant "the phone had no figure" -- PositionPacket.build() writes 0 for a NaN
// and Android reports no accuracy at all on some fixes. Zero metres is
// physically impossible, so reusing it as the sentinel costs no real value; it
// is written down here rather than left as an accident of the encoder.
inline constexpr uint16_t kAccuracyUnstated = 0;

// ## The same line, in the vocabulary a GNSS receiver actually speaks
//
// A receiver reports no metres. It reports **HDOP** -- horizontal dilution of
// precision, the factor by which the satellites' geometry multiplies a ranging
// error into a position error. Satellites spread across the sky cross at good
// angles and give HDOP near 1; satellites crowded into the strip of sky a
// street canyon leaves open cross at flat angles and give 10 and more. It is
// geometry, not signal: the 85 m jump on the walk of 2026-09-05 carried an
// unremarkable HDOP of 2.7, because a reflection off a wall is not a geometry
// problem.
//
// Horizontal error is roughly HDOP times the per-satellite ranging error, which
// is about 5 m on a consumer receiver. So the metre thresholds above land at
// **HDOP 5 and 3.5**, and the same 40 % hysteresis gap comes with them.
//
// **Checked against 7466 real fixes** (a 2h45 walk across Barcelona,
// 2026-09-05): HDOP under 2 on 73.4 % of them, 2 to 5 on 23.1 %, and only 3.6 %
// at 5 or above -- nearly all of it in the second half, in the narrow streets
// of l'Eixample, where the satellite count had halved. So the ring stays whole
// for the ordinary walk and breaks in the canyon, which is what the metre side
// promises too.
inline constexpr float kLooseAtOrAboveHdop = 5.0f;
inline constexpr float kTrustedAtOrBelowHdop = 3.5f;

// Four satellites is what a three-dimensional solution needs; three gives a 2D
// fix that assumed an altitude. That assumption is wrong by whatever the ground
// has done since, so the position is not inside the marker whatever HDOP says.
// A hard Loose, not a threshold, because it is a different kind of statement.
inline constexpr uint8_t kMinSatsForTrust = 4;

// ## There is no degrees-to-state mapping here, on purpose
//
// The render has 16 heading steps of 22.5 degrees (MapHeading) and nothing
// finer, which is why the wedge is one step either side and why **a sharp arrow
// is already a claim of +-11.25 degrees** whether or not anybody decided to
// make it. That arithmetic belongs to the drawing, not to this file.
//
// A `dirTrustFromDegrees()` stood here until it was noticed that no source can
// feed it. The phone does not send a bearing accuracy -- its heading is a
// conclusion its own trend gate either reached or did not, so it produces Good
// or Unknown and never a degree figure. A GNSS receiver reports a course and no
// uncertainty for it, so its state comes from the speed gate
// (MapGnssHeading::State::moving), also not from degrees. An API with no
// possible caller is a guess about the future dressed as a contract.

// Carried between fixes by the caller, exactly like MapGnssHeading::State: the
// hysteresis needs to know which side of the band it was on. Only position has
// state -- heading trust comes off the source already quantised, and the
// sources do their own smoothing (MapGnssHeading's speed gate, the phone's
// fused provider).
struct State {
  Pos pos = Pos::Unstated;
};

// The ring's verdict for this fix. Updates `state`.
//
// `accuracyM` is metres, with kAccuracyUnstated meaning no figure -- which
// resolves to Pos::Unstated and never latches, so a source that reports
// accuracy on some fixes and not others does not drag the ring back and forth.
Pos posTrustFor(uint16_t accuracyM, State& state);

// The same verdict from a GNSS fix, which speaks HDOP and satellite count
// instead of metres. Updates `state` with the same latch and dead band.
//
// An hdop of 0 means the receiver stated none -- GnssFix initialises it to 0
// and only a parsed GGA sets it -- so it resolves to Pos::Unstated and does not
// latch, exactly as kAccuracyUnstated does on the metre side.
Pos posTrustForHdop(float hdop, uint8_t satsUsed, State& state);

// ## The wire
//
// The BLE position packet is a fixed 21 bytes and the firmware drops a write of
// any other length (BlePositionServer.h), so heading quality cannot have a byte
// of its own without breaking every client at once. It goes in two spare bits
// of the existing flags byte instead, and the encoding is chosen so that **zero
// means Unstated**: an old phone, which writes no bits, keeps drawing exactly
// the marker it drew before this feature existed.
inline constexpr uint8_t kDirTrustFlagShift = 2;
inline constexpr uint8_t kDirTrustFlagMask = 0x0C;  // bits 2-3

// 0 -> Unstated, 1 -> Good, 2 -> Coarse, 3 -> Unknown. Total, so a garbled
// packet cannot produce a state that does not exist.
Dir dirTrustFromWireCode(uint8_t code);
uint8_t wireCodeForDirTrust(Dir dir);

// What the marker actually draws. The one thing drawPositionMarker() reads.
//
// Deliberately a description of shape, not of quality: the renderer is told
// "break the ring" and "draw a wedge", never "the fix is bad". That is what
// keeps HDOP, GGA quality digits and Android accuracy figures out of the draw
// path entirely.
struct MarkerStyle {
  // Punch gaps in the ring instead of drawing it whole.
  bool ringBroken = false;
  enum class Head : uint8_t {
    Glyph,  // today's hand (hike) or arrow (cycle/ride)
    Wedge,  // an outlined cone one heading step either side
    None,   // no heading mark at all
  };
  Head head = Head::Glyph;

  // So a caller can ask "is the marker on the panel still claiming what this
  // fix claims". MapActivity needs exactly that to know whether a fix under the
  // move floor still has to repaint (MapFollow::Request::markerStyleChanged).
  bool operator==(const MarkerStyle& other) const { return ringBroken == other.ringBroken && head == other.head; }
  bool operator!=(const MarkerStyle& other) const { return !(*this == other); }
};

MarkerStyle styleFor(Trust trust);

}  // namespace MapFixTrust
