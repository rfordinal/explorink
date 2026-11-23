add_executable(GnssSkyViewTest
  GnssSkyViewTest.cpp
)

target_include_directories(GnssSkyViewTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/src
)

target_link_libraries(GnssSkyViewTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GnssSkyViewTest)
