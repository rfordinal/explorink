#pragma once

#include <cstdint>

// lat/lon -> Web Mercator -> screen, per docs/map-data-spec.md "Coordinate
// frame" and "A tile is a storage unit, not a render unit". One cosine/sine
// pair is computed in reset(), on viewport reset only; every per-point call
// after that is plain arithmetic, no trig.
//
// Screen-space rotation: heading is "up" on screen (track-up viewport), so a
// point's Mercator offset from the anchor is rotated by -heading before
// scaling to pixels. With theta = heading * 22.5 degrees (clockwise from
// north) and (east, north) = point-minus-anchor in Mercator metres:
//
//   sx = anchorScreenX + (east*cosTheta - north*sinTheta) / mppMerc
//   sy = anchorScreenY - (east*sinTheta + north*cosTheta) / mppMerc
//
// Check theta=0 (heading N): sx grows with east, sy shrinks with north --
// north is up, east is right, as expected with no rotation. Check theta=90
// (heading E): a point due east lands above the anchor (east becomes
// "up") and a point due north lands to the left -- correct for a rider
// facing east, for whom true north is off their left shoulder.
class MapProjection {
 public:
  // EPSG:3857. Matches mapbuilder/tiles.py::lonlat_to_merc.
  static void lonLatToMerc(double lat, double lon, double& outMercX, double& outMercY);

  // The inverse, matching mapbuilder/tiles.py::merc_to_lonlat. Needed because
  // the route overview picks its anchor in Mercator metres (MapRouteFit) and
  // reset() takes lat/lon -- everything else on the map path already has a
  // lat/lon to start from and never needs this direction.
  static void mercToLonLat(double mercX, double mercY, double& outLat, double& outLon);

  // anchorScreenX/Y: the screen pixel the anchor geographic point maps to.
  // headingStep: 0-15, 22.5 degrees per step, clockwise from north.
  // mppMerc: Mercator metres per pixel (mpp / cos(anchorLat), already
  // computed by the caller -- see docs/map-data-spec.md).
  void reset(double anchorLat, double anchorLon, int16_t anchorScreenX, int16_t anchorScreenY, uint8_t headingStep,
             double mppMerc);

  void projectMerc(double mercX, double mercY, int16_t& outScreenX, int16_t& outScreenY) const;

  // Same projection into int32_t. Tile geometry always lands within a few
  // thousand pixels of the anchor -- the tile range is only ever 3x3 around it
  // -- so int16_t holds it. A route does not: it is one object that can be 200
  // km long, and at 1 m/px its far end is 2e8 pixels away, which wraps an
  // int16_t into a line drawn straight across the middle of the map. The canvas
  // clips per segment (GfxRendererCanvas::drawLine), so a coordinate far off
  // screen costs nothing as long as it is still the *right* coordinate.
  //
  // Clamped to +/-kMaxWidePx rather than left to wrap: converting a double past
  // int32_t's range is undefined behaviour, and a route point at the far side of
  // the world is a corrupt file, not a drawing problem.
  void projectMercWide(double mercX, double mercY, int32_t& outScreenX, int32_t& outScreenY) const;

  static constexpr int32_t kMaxWidePx = 100000000;  // 1e8, past any real route

  // Tile-local point (as stored in a .tib way/place/junction record) plus
  // the tile's own origin (from its header) -> screen. origin_x/origin_y are
  // the Mercator NW corner; local x grows east, local y grows south (see
  // mapbuilder/tiles.py and mapbuilder/render_from_tiles.py's
  // _global_points, which undoes the same offset).
  void projectTileLocal(int32_t originX, int32_t originY, int16_t localX, int16_t localY, int16_t& outScreenX,
                        int16_t& outScreenY) const;

  // Inverse of projectMerc -- screen pixel back to Mercator metres. Used to
  // find which tiles a viewport needs (docs/map-data-spec.md, "Which tiles
  // to load"): inverse-project the viewport's corners, take the bbox.
  void screenToMerc(int16_t screenX, int16_t screenY, double& outMercX, double& outMercY) const;

  double mppMerc() const { return mppMerc_; }

 private:
  double anchorMercX_ = 0.0;
  double anchorMercY_ = 0.0;
  double cosTheta_ = 1.0;
  double sinTheta_ = 0.0;
  double mppMerc_ = 1.0;
  int16_t anchorScreenX_ = 0;
  int16_t anchorScreenY_ = 0;
};
