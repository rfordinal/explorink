#pragma once

#include <cstdint>

#include "IMapSource.h"
#include "MapRenderer.h"

// Hardcoded/mock source used by MapActivity until on-device tile loading
// lands (docs/prototype-plan.md, P4 -- MapTileSource plus HalFileSource).
// It is an IMapSource like the real thing, so MapActivity's render call is
// already the final one and only the source behind it changes.
//
// Proportional to screen size so it stays correct across all 4
// orientations (never assume 480x800). Allocates nothing.
class MockMapSource : public IMapSource {
 public:
  MockMapSource(int screenWidth, int screenHeight) {
    const auto x = [screenWidth](float frac) { return static_cast<int16_t>(screenWidth * frac); };
    const auto y = [screenHeight](float frac) { return static_cast<int16_t>(screenHeight * frac); };

    const float fx[kRoadPoints] = {0.50f, 0.48f, 0.54f, 0.42f, 0.38f, 0.46f};
    const float fy[kRoadPoints] = {0.98f, 0.75f, 0.57f, 0.40f, 0.20f, 0.05f};
    for (int i = 0; i < kRoadPoints; ++i) {
      xs_[i] = x(fx[i]);
      ys_[i] = y(fy[i]);
    }

    placeXs_[0] = x(0.48f);
    placeYs_[0] = y(0.75f);
    placeXs_[1] = x(0.38f);
    placeYs_[1] = y(0.20f);
  }

  bool beginWays() override {
    wayServed_ = false;
    return true;
  }

  bool nextWay(MapWayRef& out) override {
    if (wayServed_) return false;
    wayServed_ = true;
    out.classId = 0;
    out.roughness = 0;
    out.flags = 0;
    out.pointCount = kRoadPoints;
    out.xs = xs_;
    out.ys = ys_;
    return true;
  }

  bool beginPlaces() override {
    placeIndex_ = 0;
    return true;
  }

  bool nextPlace(MapPlaceRef& out) override {
    if (placeIndex_ >= kPlaceCount) return false;
    out.x = placeXs_[placeIndex_];
    out.y = placeYs_[placeIndex_];
    out.rank = 0;
    out.name = "";
    ++placeIndex_;
    return true;
  }

 private:
  static constexpr int kRoadPoints = 6;
  static constexpr int kPlaceCount = 2;

  int16_t xs_[kRoadPoints] = {};
  int16_t ys_[kRoadPoints] = {};
  int16_t placeXs_[kPlaceCount] = {};
  int16_t placeYs_[kPlaceCount] = {};
  bool wayServed_ = false;
  int placeIndex_ = 0;
};

// Marker anchored near the bottom edge, per the viewport model in
// docs/architecture-plan.md: track-up rotation always points travel
// direction up, so keeping the marker low maximizes look-ahead room.
inline MapViewState buildMockMapViewState(int screenWidth, int screenHeight) {
  MapViewState state;
  state.markerX = static_cast<int16_t>(screenWidth * 0.49f);
  state.markerY = static_cast<int16_t>(screenHeight * 0.88f);
  state.heading = MapHeading::N;
  return state;
}
