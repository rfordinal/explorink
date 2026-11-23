#pragma once

#include <cstdint>

#include "MapGnssBars.h"
#include "images/Mountains.h"

// Where a satellite lands on the acquisition screen's sky, and how strong its
// signal reads. Pure arithmetic, no renderer, no driver -- so it is host-tested
// (test/gnss_sky_view) and the activity above it only draws.
//
// ## The projection is a panorama, not a skyplot
//
// A GNSS skyplot is conventionally a circle seen from above: azimuth around the
// rim, elevation towards the centre. That is the right picture for an engineer
// checking geometry and the wrong one for a rider deciding where to stand,
// because the thing they are looking at is a horizon. **North up on a circle
// answers "which satellites"; a panorama answers "which way is the sky open",
// which is the only action available to someone waiting for a fix.**
//
// So: azimuth runs left to right across the box, elevation runs up from the
// ridge at its bottom edge. South is at both ends and north is in the middle,
// which is what a rider facing north sees. It is also cheap -- two divisions
// per satellite, no trigonometry, on a board that has a fix to wait for and no
// cycles to spare on drawing while it waits.
//
// ## The ridge is not decoration, and it is not a second drawing either
//
// The bottom of the box is the mountain line art (src/images/mountains.svg,
// baked by scripts/gen_mountains.py), and it states the physical fact behind a
// slow fix: a satellite low in the sky is behind terrain. A dot that sits in
// the ridge is one the rider cannot expect to help.
//
// **The geometry below reads the asset's own top edge**, `MountainsTop`, rather
// than a profile typed next to it. The first version had eleven hand-typed
// numbers under art that came from somewhere else -- a drawing and a claim that
// can disagree, on the one screen whose whole job is to say which way the sky
// is open. Now moving the SVG moves both.
namespace GnssSkyView {

// The sky area in logical screen pixels. y is its top, y + h - 1 the horizon
// line the ridge is drawn on.
struct Box {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct Dot {
  int x = 0;
  int y = 0;
  int radius = 0;
  // Filled means tracked (a non-zero C/N0); an outline means the receiver knows
  // the satellite is up there and is not hearing it. The distinction is the
  // whole point of the screen -- a sky full of outlines is a sky full of
  // satellites the antenna cannot use, and no amount of waiting fixes it.
  bool filled = false;
};

// Elevation above which nothing is drawn any higher. 90 is the zenith and a
// receiver has been seen to report more than it should, so this clamps rather
// than trusting the field.
inline constexpr uint8_t kMaxElevation = 90;

// How strong one satellite reads, bucketed for the dot ladder.
//
// **The thresholds are not this screen's own.** They are the map header's
// calibrated C/N0 rungs (MapGnssBars::kBestSnrForHeightStep -- 26, 31, 36, 40
// dB-Hz, the maintainer's numbers against real readings from this L76K). Two
// screens that scored the same sky differently would teach the rider two
// instruments, and the header's ladder is the one with evidence behind it.
//
// Bucketed rather than continuous because the panel cannot show the difference:
// one device pixel of radius is the smallest step there is. The top rung folds
// into the one below it for the same reason -- a fifth radius would need a
// 14 px wide mark, which is too big for a plot holding sixteen of them.
//
// 0 is reserved for "in view, not tracked" (snr == 0), which is the distinction
// the whole plot exists to draw.
inline constexpr uint8_t kMaxBucket = 4;

inline uint8_t snrBucket(uint8_t snr) {
  if (snr == 0) return 0;
  // 1 is "tracked, below the first rung", so a satellite the antenna hears
  // weakly is still a mark and still different from one it does not hear at
  // all. Every rung passed adds one from there.
  int bucket = 1;
  for (int step = 0; step < MapGnssBars::kHeightStepCount; ++step) {
    if (snr >= MapGnssBars::kBestSnrForHeightStep[step]) bucket = step + 2;
  }
  return static_cast<uint8_t>(bucket > kMaxBucket ? kMaxBucket : bucket);
}

// Radius in device pixels for a bucket. Deliberately small and deliberately
// non-linear: the useful reading is "is this dot solid", not its exact size.
inline int dotRadius(uint8_t bucket) {
  switch (bucket) {
    case 0:
      return 3;  // outline, same size as a weak tracked one so the fill reads
    case 1:
      return 3;
    case 2:
      return 4;
    case 3:
      return 5;
    default:
      return 6;
  }
}

// Places one satellite. `azimuth` is degrees true, `elevation` degrees above
// the horizon; both come straight from GSV and are clamped here rather than at
// the call site, because a receiver that reports 91 or 400 must not draw
// outside the box.
//
// North is the centre of the box. The mapping is (azimuth + 180) mod 360, so
// the left edge is south, the middle north, the right edge south again.
inline Dot plot(const Box& box, uint8_t elevation, uint16_t azimuth, uint8_t snr) {
  Dot dot;
  const uint8_t bucket = snrBucket(snr);
  dot.filled = snr > 0;
  dot.radius = dotRadius(bucket);

  const int wrapped = static_cast<int>((azimuth % 360 + 180) % 360);
  const int usableWidth = box.w > 1 ? box.w - 1 : 1;
  dot.x = box.x + wrapped * usableWidth / 360;

  const uint8_t clamped = elevation > kMaxElevation ? kMaxElevation : elevation;
  const int horizon = box.y + (box.h > 1 ? box.h - 1 : 0);
  const int usableHeight = box.h > 1 ? box.h - 1 : 0;
  dot.y = horizon - clamped * usableHeight / kMaxElevation;
  return dot;
}

// How much of the art's bottom sits below the horizon and is never drawn. Must
// match GnssAcquireActivity's kRidgeCrop -- the drawing and the geometry read
// the same asset and have to read it from the same baseline.
inline constexpr int kRidgeCrop = 60;

// The art is drawn 1:1 and centred, never scaled -- the parent repo's standing
// rule, and the reason this returns the asset's own pixels rather than a
// fraction of the box height. The offset is NEGATIVE on every panel here: the
// asset is deliberately wider than the screen so the ridge runs off both edges.
inline int ridgeOffset(const Box& box) { return (box.w - MOUNTAINS_WIDTH) / 2; }

// Height of the silhouette above the horizon at a pixel column, taken as an
// offset from box.x, with the cropped-off bottom already subtracted.
inline int ridgeHeight(const Box& box, int column) {
  if (box.w <= 0 || box.h <= 0) return 0;
  const int assetColumn = column - ridgeOffset(box);
  if (assetColumn < 0 || assetColumn >= MOUNTAINS_WIDTH) return 0;
  const int height = static_cast<int>(MountainsTop[assetColumn]) - kRidgeCrop;
  if (height <= 0) return 0;
  return height > box.h ? box.h : height;
}

// The band the satellites are plotted across, inset from the sky's own edges.
//
// Two reasons it is not the full band. The ticks: with south at both ends, the
// two S labels sat half off the screen (seen on the panel 2026-09-10). And the
// marks: a satellite due south is a 6 px diamond centred on the edge column, so
// half of it would be off the panel.
inline constexpr int kPlotInset = 22;

inline Box plotArea(const Box& band) {
  Box plot = band;
  plot.x = band.x + kPlotInset;
  plot.w = band.w - kPlotInset * 2;
  if (plot.w < 1) plot = band;
  return plot;
}

// True when a satellite is drawn inside the terrain rather than above it. Not
// used to hide it -- a rider needs to see that the sky is blocked in that
// direction, which is exactly the case the dot is reporting.
inline bool behindRidge(const Box& box, const Dot& dot) {
  const int horizon = box.y + (box.h > 1 ? box.h - 1 : 0);
  return dot.y >= horizon - ridgeHeight(box, dot.x - box.x);
}

}  // namespace GnssSkyView
