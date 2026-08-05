add_executable(MapTilePathTest
  MapTilePathTest.cpp
)

target_include_directories(MapTilePathTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapTilePathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapTilePathTest)
