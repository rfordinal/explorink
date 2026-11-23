add_executable(MapFixTrustTest
  MapFixTrustTest.cpp
  ${REPO_ROOT}/src/activities/map/MapFixTrust.cpp
)

target_include_directories(MapFixTrustTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapFixTrustTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapFixTrustTest)
