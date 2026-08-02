add_executable(MapTileReaderGoldenTest
  MapTileReaderGoldenTest.cpp
  ${REPO_ROOT}/test/map_preview/MapPreviewPipeline.cpp
  ${REPO_ROOT}/test/map_preview/HeapProbe.cpp
  ${REPO_ROOT}/test/map_preview/PpmCanvas.cpp
  ${REPO_ROOT}/src/activities/map/MapRenderer.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
  ${REPO_ROOT}/src/activities/map/MapTileReader.cpp
  ${REPO_ROOT}/src/activities/map/MapTileSource.cpp
  ${REPO_ROOT}/src/activities/map/MapViewport.cpp
  ${REPO_ROOT}/src/activities/map/MapHatch.cpp
)

target_include_directories(MapTileReaderGoldenTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/test/map_preview
)

target_compile_definitions(MapTileReaderGoldenTest PRIVATE
  MAP_TILE_READER_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(MapTileReaderGoldenTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapTileReaderGoldenTest)
