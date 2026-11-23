add_executable(MapChromeTest
  MapChromeTest.cpp
  ${REPO_ROOT}/src/activities/map/MapLabels.cpp
  ${REPO_ROOT}/src/activities/map/MapPointMarks.cpp
)

target_include_directories(MapChromeTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  # MapPointMarks.cpp draws the 24px POI icon set, whose freeink::Icon lives in
  # the SDK -- same include the preview and the map window already add.
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
)

target_link_libraries(MapChromeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# MapStyleDefaults.h is generated from data/mapstyle.json (test/CMakeLists.txt).
add_dependencies(MapChromeTest map_generated_headers)

gtest_discover_tests(MapChromeTest)
