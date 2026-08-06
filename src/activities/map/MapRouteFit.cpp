#include "MapRouteFit.h"

#include <cmath>

#include "MapProjection.h"
#include "MapViewport.h"

namespace {
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
// MapHeading's grid: 16 steps of 22.5 degrees, clockwise from north.
constexpr double kStepDegrees = 22.5;

// How far apart two headings are on the 16-step ring, the short way round. 8 is
// the maximum, i.e. opposite.
int headingDistance(int a, int b) {
  const int diff = ((a - b) % 16 + 16) % 16;
  return diff <= 8 ? diff : 16 - diff;
}
}  // namespace

void MapRouteFit::begin(int32_t centreX, int32_t centreY) {
  centreX_ = static_cast<double>(centreX);
  centreY_ = static_cast<double>(centreY);
  pointsSeen_ = 0;
  for (int h = 0; h < kHeadings; ++h) {
    const double theta = h * kStepDegrees * kDegToRad;
    cos_[h] = static_cast<float>(std::cos(theta));
    sin_[h] = static_cast<float>(std::sin(theta));
    minU_[h] = 0.0f;
    maxU_[h] = 0.0f;
    minV_[h] = 0.0f;
    maxV_[h] = 0.0f;
  }
}

void MapRouteFit::addPoint(int32_t x, int32_t y) {
  // Relative to the centre before anything else: this is what keeps the
  // accumulators inside float's precision (see the header).
  const float east = static_cast<float>(static_cast<double>(x) - centreX_);
  const float north = static_cast<float>(static_cast<double>(y) - centreY_);

  const bool first = pointsSeen_ == 0;
  ++pointsSeen_;
  if (first) {
    firstEast_ = east;
    firstNorth_ = north;
  }
  lastEast_ = east;
  lastNorth_ = north;

  for (int h = 0; h < kHeadings; ++h) {
    const float u = east * cos_[h] - north * sin_[h];
    const float v = east * sin_[h] + north * cos_[h];
    if (first) {
      minU_[h] = maxU_[h] = u;
      minV_[h] = maxV_[h] = v;
      continue;
    }
    if (u < minU_[h]) minU_[h] = u;
    if (u > maxU_[h]) maxU_[h] = u;
    if (v < minV_[h]) minV_[h] = v;
    if (v > maxV_[h]) maxV_[h] = v;
  }
}

bool MapRouteFit::finish(int screenWidth, int screenHeight, Result& out) const {
  out = Result{};
  if (pointsSeen_ == 0) return false;

  // A route wider than the usable area at the coarsest rung is the "as much as
  // possible" case, not an error. One pixel is the floor so a degenerate screen
  // cannot divide by zero.
  const double usableW = screenWidth - 2.0 * kFitMarginPx > 1.0 ? screenWidth - 2.0 * kFitMarginPx : 1.0;
  const double usableH = screenHeight - 2.0 * kFitMarginPx > 1.0 ? screenHeight - 2.0 * kFitMarginPx : 1.0;

  // The heading that puts the route's direction of travel up the screen. A
  // closed loop, whose start and end are the same point, has no such direction;
  // then this is 0 and the fit falls back to preferring north up, which is the
  // one orientation a rider can always interpret without a reference.
  const double travelEast = static_cast<double>(lastEast_) - static_cast<double>(firstEast_);
  const double travelNorth = static_cast<double>(lastNorth_) - static_cast<double>(firstNorth_);
  int preferredHeading = 0;
  if (std::fabs(travelEast) > 1.0 || std::fabs(travelNorth) > 1.0) {
    // atan2(east, north) is the compass bearing: clockwise from north, which is
    // MapHeading's own convention rather than the maths one.
    const double bearing = std::atan2(travelEast, travelNorth) / kDegToRad;
    const int step = static_cast<int>(std::lround(bearing / kStepDegrees));
    preferredHeading = ((step % kHeadings) + kHeadings) % kHeadings;
  }

  int bestHeading = -1;
  int bestStep = MapViewport::kZoomStepCount - 1;
  int bestTravelDistance = 0;
  double bestRequiredMpp = 0.0;
  bool bestFits = false;

  for (int h = 0; h < kHeadings; ++h) {
    const double width = static_cast<double>(maxU_[h]) - static_cast<double>(minU_[h]);
    const double height = static_cast<double>(maxV_[h]) - static_cast<double>(minV_[h]);

    // Anchor for this heading: the middle of its own extent, rotated back into
    // Mercator. The rotation matrix is orthogonal, so the inverse is the
    // transpose -- the same identity MapProjection::screenToMerc uses.
    const double uMid = (static_cast<double>(minU_[h]) + static_cast<double>(maxU_[h])) / 2.0;
    const double vMid = (static_cast<double>(minV_[h]) + static_cast<double>(maxV_[h])) / 2.0;
    const double east = uMid * cos_[h] + vMid * sin_[h];
    const double north = -uMid * sin_[h] + vMid * cos_[h];
    const double anchorMercX = centreX_ + east;
    const double anchorMercY = centreY_ + north;

    double anchorLat = 0.0;
    double anchorLon = 0.0;
    MapProjection::mercToLonLat(anchorMercX, anchorMercY, anchorLat, anchorLon);

    // The extent is in Mercator metres and the ladder is in ground metres, so
    // the cosine has to be paid here -- the same conversion
    // MapViewport::mppMercFor makes in the other direction, per heading because
    // each heading has its own anchor and therefore its own latitude.
    const double requiredMppMerc = std::fmax(width / usableW, height / usableH);
    const double cosLat = std::cos(anchorLat * kDegToRad);
    const double requiredMpp = requiredMppMerc * (cosLat > 1e-6 ? cosLat : 1e-6);

    int step = -1;
    for (int s = 0; s < MapViewport::kZoomStepCount; ++s) {
      if (MapViewport::kZoomLadder[s].mpp >= requiredMpp) {
        step = s;
        break;
      }
    }
    const bool fits = step >= 0;
    const int effectiveStep = fits ? step : MapViewport::kZoomStepCount - 1;

    // Better means: fits when the incumbent does not; then a finer rung; then
    // closer to the route's own direction of travel.
    //
    // **The last one is not a cosmetic tie-break.** Zoom is a five-rung ladder,
    // so two headings that land on the same rung render at exactly the same
    // scale -- a smaller "required" mpp buys literally nothing once it is
    // rounded up to a rung. Sorting on it anyway produced a real bug: a
    // north-south route came out tilted 22.5 degrees, because a slight tilt
    // trades vertical extent the 744-pixel axis is short of for horizontal
    // extent the 424-pixel axis has spare. Optimal by arithmetic, wrong on the
    // panel -- the rider gets a map askew for no gain they can see.
    //
    // So among headings on the same rung, pick the one nearest the bearing from
    // the route's start to its end. The route then runs up the screen, which is
    // both what track-up means everywhere else on this device and what makes the
    // overview recognisable rather than something to re-read.
    const int travelDistance = headingDistance(h, preferredHeading);
    bool better = bestHeading < 0;
    if (!better && fits != bestFits) better = fits;
    if (!better && fits == bestFits && effectiveStep != bestStep) better = effectiveStep < bestStep;
    if (!better && fits == bestFits && effectiveStep == bestStep) better = travelDistance < bestTravelDistance;

    if (!better) continue;
    bestTravelDistance = travelDistance;
    bestHeading = h;
    bestStep = effectiveStep;
    bestFits = fits;
    bestRequiredMpp = requiredMpp;
    out.heading = static_cast<uint8_t>(h);
    out.anchorMercX = anchorMercX;
    out.anchorMercY = anchorMercY;
    out.anchorLat = anchorLat;
    out.anchorLon = anchorLon;
    out.extentWidthM = width;
    out.extentHeightM = height;
  }

  out.valid = true;
  out.zoomStep = static_cast<uint8_t>(bestStep);
  out.fits = bestFits;
  out.requiredMpp = bestRequiredMpp;
  return true;
}
