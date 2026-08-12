add_executable(MapLabelsTest
  MapLabelsTest.cpp
  ${REPO_ROOT}/src/activities/map/MapLabels.cpp
)

target_include_directories(MapLabelsTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapLabelsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# MapStyleDefaults.h is generated from data/mapstyle.json (test/CMakeLists.txt);
# the tests start from the compiled style and override the fields under test.
add_dependencies(MapLabelsTest map_generated_headers)

gtest_discover_tests(MapLabelsTest)
