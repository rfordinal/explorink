#pragma once

#include <cstdint>

// Picks the zoom rung, the heading and the anchor that show the most of a
// route -- the whole thing if the zoom ladder can hold it.
// ../../../docs/route-layer-plan.md, "The overview fit", in the parent xteink
// repo.
//
// ## Why the bbox is the wrong thing to measure
//
// A route's axis-aligned Mercator bbox is not the strip it occupies. A route
// running north-east has a bbox far larger than the band it actually fills, and
// the screen is 480x800 rather than square, so **the heading changes what
// fits**. Measuring the box would zoom out further than needed and would pick
// the wrong heading whenever a route is not axis-aligned.
//
// So this measures the point set. One streaming pass, 16 sets of accumulators,
// one for each heading on MapHeading's grid:
//
//   u =  east*cos(theta) - north*sin(theta)     // screen x
//   v =  east*sin(theta) + north*cos(theta)     // screen up
//
// Exactly MapProjection's own rotation, so a fit that says "this fits" and the
// projection that then draws it cannot disagree. The 16 cos/sin pairs are
// computed once in begin(), never per point -- the same no-trig-per-point rule
// the projection follows.
//
// ## Float accumulators are safe here, and only here
//
// Absolute Mercator metres reach 2e7, past float's 24-bit mantissa. begin()
// takes a centre and every point is accumulated **relative to it**, so the
// values stay inside the route's own half-extent -- under 1e6 m for any real
// route, where a float still resolves 0.06 m. That is what keeps this object at
// 384 bytes instead of 768.
//
// ## Nothing fits, for a long route
//
// The coarsest rung is 20 m/px, which is 9.6 x 16 km of screen. A day's ride is
// longer than that. Then `fits` comes back false, the coarsest rung and the
// best heading are used anyway, and the middle of the route is what lands on
// screen. That is the "or as much as possible" case, and it is deliberate: the
// alternative is a sixth ladder rung reading a z10 tile set that does not exist
// (docs/map-data-spec.md, open question 1).
class MapRouteFit {
 public:
  // Kept off the panel edges so the route does not touch the bezel or hide
  // under the button hints. Smaller than MapViewport::kMarginPx, which inflates
  // a tile range for label overhang and is a different job.
  static constexpr double kFitMarginPx = 28.0;

  struct Result {
    bool valid = false;
    // MapHeading's 0-15, the heading the frame is drawn track-up with.
    uint8_t heading = 0;
    // Index into MapViewport::kZoomLadder.
    uint8_t zoomStep = 0;
    // False when even the coarsest rung cannot hold the whole route. The rest
    // of the fields are still the best available answer -- see the header note.
    bool fits = false;
    // Where to anchor, i.e. what the screen centre shows.
    double anchorMercX = 0.0;
    double anchorMercY = 0.0;
    double anchorLat = 0.0;
    double anchorLon = 0.0;
    // The winning heading's extent, in Mercator metres, and the ground
    // metres-per-pixel that would have been needed to hold it. Reported so a
    // log line or a debug readout can say how far off a non-fitting route is.
    double extentWidthM = 0.0;
    double extentHeightM = 0.0;
    double requiredMpp = 0.0;
  };

  // `centreX/centreY` is any point near the route -- the header bbox's centre
  // is what MapActivity passes. It only has to be close enough to keep the
  // relative coordinates inside float's precision, not to be the final anchor.
  void begin(int32_t centreX, int32_t centreY);
  void addPoint(int32_t x, int32_t y);
  uint32_t pointsSeen() const { return pointsSeen_; }

  // Fills `out`. False if no point was added, which is the only way this can
  // fail -- a route that fits nothing still produces a usable answer.
  bool finish(int screenWidth, int screenHeight, Result& out) const;

 private:
  static constexpr int kHeadings = 16;

  double centreX_ = 0.0;
  double centreY_ = 0.0;
  float cos_[kHeadings] = {};
  float sin_[kHeadings] = {};
  float minU_[kHeadings] = {};
  float maxU_[kHeadings] = {};
  float minV_[kHeadings] = {};
  float maxV_[kHeadings] = {};
  // First and last point, relative to the centre. Their difference is the
  // route's direction of travel, which is what decides the heading among the
  // several that land on the same zoom rung -- see MapRouteFit.cpp.
  float firstEast_ = 0.0f;
  float firstNorth_ = 0.0f;
  float lastEast_ = 0.0f;
  float lastNorth_ = 0.0f;
  uint32_t pointsSeen_ = 0;
};
