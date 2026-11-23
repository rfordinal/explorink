add_executable(MapGnssHeadingTest
  MapGnssHeadingTest.cpp
  ${REPO_ROOT}/src/activities/map/MapGnssHeading.cpp
)

target_include_directories(MapGnssHeadingTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapGnssHeadingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapGnssHeadingTest)
