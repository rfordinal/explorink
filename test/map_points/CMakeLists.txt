add_executable(MapPointTest
  MapPointTest.cpp
  ${REPO_ROOT}/src/activities/map/MapPointReader.cpp
  ${REPO_ROOT}/src/activities/map/MapPointQuery.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/PinGeo.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
)

target_include_directories(MapPointTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/test/map_preview
)

target_compile_definitions(MapPointTest PRIVATE
  MAP_POINTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  MAP_POINTS_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

# MapPointTypes.h is generated from point_spec.py in the tile generator repo and
# is committed here, so nothing to depend on -- unlike MapStyleDefaults.h, which
# is generated at build time. Regenerate with
# mapbuilder/tilegen/gen_point_enum.py --cpp-out.
target_link_libraries(MapPointTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapPointTest)
