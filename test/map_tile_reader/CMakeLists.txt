add_executable(MapTileReaderGoldenTest
  MapTileReaderGoldenTest.cpp
  ${REPO_ROOT}/test/map_preview/MapPreviewPipeline.cpp
  ${REPO_ROOT}/test/map_preview/HeapProbe.cpp
  ${REPO_ROOT}/test/map_preview/PpmCanvas.cpp
  ${REPO_ROOT}/test/map_preview/PreviewFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/activities/map/MapRenderer.cpp
  ${REPO_ROOT}/src/activities/map/MapLabels.cpp
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
  ${REPO_ROOT}/src/activities/map/MapModeMask.cpp
  # The point layer: MapRenderer and MapPreviewPipeline both call into it since
  # the Nearby work landed, so this target does not link without them. Same
  # three files map_preview/CMakeLists.txt lists.
  ${REPO_ROOT}/src/activities/map/MapPointReader.cpp
  ${REPO_ROOT}/src/activities/map/MapPointSource.cpp
  ${REPO_ROOT}/src/activities/map/MapPointMarks.cpp
)

target_include_directories(MapTileReaderGoldenTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/test/map_preview
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
)

target_compile_definitions(MapTileReaderGoldenTest PRIVATE
  MAP_TILE_READER_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(MapTileReaderGoldenTest PRIVATE
  ZLIB::ZLIB
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(MapTileReaderGoldenTest map_generated_headers)

gtest_discover_tests(MapTileReaderGoldenTest)
