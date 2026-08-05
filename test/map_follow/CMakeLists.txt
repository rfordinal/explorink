add_executable(MapFollowTest
  MapFollowTest.cpp
  ${REPO_ROOT}/src/activities/map/MapFollow.cpp
)

target_include_directories(MapFollowTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapFollowTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapFollowTest)
