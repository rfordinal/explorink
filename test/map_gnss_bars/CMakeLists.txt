add_executable(MapGnssBarsTest
  MapGnssBarsTest.cpp
  ${REPO_ROOT}/src/activities/map/MapGnssBars.cpp
)

target_include_directories(MapGnssBarsTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapGnssBarsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapGnssBarsTest)
