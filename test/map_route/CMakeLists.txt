add_executable(MapRouteTest
  MapRouteTest.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteReader.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteFit.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/MapViewport.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
)

target_include_directories(MapRouteTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/test/map_preview
)

target_compile_definitions(MapRouteTest PRIVATE
  MAP_ROUTE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  MAP_ROUTE_TEMP_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

# MapViewport.h reads the compiled style for the marker anchor, so the generated
# header has to exist before this compiles -- same dependency the other map tests
# take (test/CMakeLists.txt, map_generated_headers).
add_dependencies(MapRouteTest map_generated_headers)

target_link_libraries(MapRouteTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
