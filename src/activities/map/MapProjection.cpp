#include "MapProjection.h"

#include <cmath>

namespace {
constexpr double kMercRadius = 6378137.0;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
}  // namespace

void MapProjection::lonLatToMerc(double lat, double lon, double& outMercX, double& outMercY) {
  outMercX = lon * kDegToRad * kMercRadius;
  outMercY = kMercRadius * std::log(std::tan(3.14159265358979323846 / 4.0 + lat * kDegToRad / 2.0));
}

void MapProjection::mercToLonLat(double mercX, double mercY, double& outLat, double& outLon) {
  outLon = mercX / kMercRadius / kDegToRad;
  outLat = (2.0 * std::atan(std::exp(mercY / kMercRadius)) - 3.14159265358979323846 / 2.0) / kDegToRad;
}

void MapProjection::reset(double anchorLat, double anchorLon, int16_t anchorScreenX, int16_t anchorScreenY,
                          uint8_t headingStep, double mppMerc) {
  lonLatToMerc(anchorLat, anchorLon, anchorMercX_, anchorMercY_);
  anchorScreenX_ = anchorScreenX;
  anchorScreenY_ = anchorScreenY;
  mppMerc_ = mppMerc;

  const double theta = (headingStep % 16) * 22.5 * kDegToRad;
  cosTheta_ = std::cos(theta);
  sinTheta_ = std::sin(theta);
}

void MapProjection::projectMerc(double mercX, double mercY, int16_t& outScreenX, int16_t& outScreenY) const {
  const double east = mercX - anchorMercX_;
  const double north = mercY - anchorMercY_;
  const double sxOff = (east * cosTheta_ - north * sinTheta_) / mppMerc_;
  const double syOff = -(east * sinTheta_ + north * cosTheta_) / mppMerc_;
  outScreenX = static_cast<int16_t>(anchorScreenX_ + std::lround(sxOff));
  outScreenY = static_cast<int16_t>(anchorScreenY_ + std::lround(syOff));
}

void MapProjection::projectMercWide(double mercX, double mercY, int32_t& outScreenX, int32_t& outScreenY) const {
  const double east = mercX - anchorMercX_;
  const double north = mercY - anchorMercY_;
  const double sx = anchorScreenX_ + (east * cosTheta_ - north * sinTheta_) / mppMerc_;
  const double sy = anchorScreenY_ - (east * sinTheta_ + north * cosTheta_) / mppMerc_;
  const double limit = static_cast<double>(kMaxWidePx);
  outScreenX = static_cast<int32_t>(std::lround(std::fmax(-limit, std::fmin(limit, sx))));
  outScreenY = static_cast<int32_t>(std::lround(std::fmax(-limit, std::fmin(limit, sy))));
}

void MapProjection::projectTileLocal(int32_t originX, int32_t originY, int16_t localX, int16_t localY,
                                     int16_t& outScreenX, int16_t& outScreenY, const uint8_t coordShift) const {
  // Widen before shifting: `localX` is int16 and a shift of 1 on a coordinate
  // near the tile edge overflows it. Shift 0 leaves both arithmetic and result
  // exactly what they were before the byte existed.
  const int32_t offX = static_cast<int32_t>(localX) << coordShift;
  const int32_t offY = static_cast<int32_t>(localY) << coordShift;
  const double mercX = static_cast<double>(originX) + static_cast<double>(offX);
  const double mercY = static_cast<double>(originY) - static_cast<double>(offY);
  projectMerc(mercX, mercY, outScreenX, outScreenY);
}

void MapProjection::screenToMerc(int16_t screenX, int16_t screenY, double& outMercX, double& outMercY) const {
  const double sxOff = static_cast<double>(screenX - anchorScreenX_);
  const double syOff = static_cast<double>(screenY - anchorScreenY_);
  // Inverse of the rotation in projectMerc -- the matrix is orthogonal, so
  // its inverse is its transpose.
  const double east = mppMerc_ * (sxOff * cosTheta_ - syOff * sinTheta_);
  const double north = -mppMerc_ * (sxOff * sinTheta_ + syOff * cosTheta_);
  outMercX = anchorMercX_ + east;
  outMercY = anchorMercY_ + north;
}
