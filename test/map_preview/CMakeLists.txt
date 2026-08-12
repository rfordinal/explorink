add_executable(map_preview
  preview_main.cpp
  MapPreviewPipeline.cpp
  HeapProbe.cpp
  PpmCanvas.cpp
  PreviewFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/activities/map/MapRenderer.cpp
  ${REPO_ROOT}/src/activities/map/MapAreaFill.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteReader.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteFit.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteSource.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
  ${REPO_ROOT}/src/activities/map/MapTileReader.cpp
  ${REPO_ROOT}/src/activities/map/MapTileSource.cpp
  ${REPO_ROOT}/src/activities/map/MapViewport.cpp
  ${REPO_ROOT}/src/activities/map/MapHatch.cpp
  ${REPO_ROOT}/src/activities/map/MapLabels.cpp
  ${REPO_ROOT}/src/activities/map/MapModeMask.cpp
)

target_include_directories(map_preview PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# zlib decompresses the built-in fonts' glyph groups (PreviewFont.cpp). The
# device uses uzlib for the same DEFLATE streams; on the host zlib is already
# there and needs no vendoring.
find_package(ZLIB REQUIRED)
target_link_libraries(map_preview PRIVATE crosspoint_test_common ZLIB::ZLIB)

# MapStyleDefaults.h / MapModeMaskDefaults.h are generated from
# data/mapstyle.json (test/CMakeLists.txt), so a style edit reaches this
# preview on the next build without a firmware build in between.
add_dependencies(map_preview map_generated_headers)

# Same real MapRenderer::drawMarker() glyph, stamped onto an already-rendered
# PPM instead of drawn as part of a fresh render -- for tools/render_ride_video.py
# --track (parent repo), which needs the marker moved and rotated on every
# packet without re-running the tile/geometry pass 339 times over. Mirrors
# map_preview's own source list: MapRenderer.cpp is one translation unit with
# every other MapRenderer method in it too, so the same full set has to link
# even though drawMarker() itself only touches the canvas and the style.
add_executable(marker_stamp
  marker_stamp_main.cpp
  PpmCanvas.cpp
  PreviewFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/activities/map/MapRenderer.cpp
  ${REPO_ROOT}/src/activities/map/MapAreaFill.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteReader.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteFit.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteSource.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
  ${REPO_ROOT}/src/activities/map/MapTileReader.cpp
  ${REPO_ROOT}/src/activities/map/MapTileSource.cpp
  ${REPO_ROOT}/src/activities/map/MapViewport.cpp
  ${REPO_ROOT}/src/activities/map/MapHatch.cpp
  ${REPO_ROOT}/src/activities/map/MapLabels.cpp
  ${REPO_ROOT}/src/activities/map/MapModeMask.cpp
)
target_include_directories(marker_stamp PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(marker_stamp PRIVATE crosspoint_test_common ZLIB::ZLIB)
add_dependencies(marker_stamp map_generated_headers)

