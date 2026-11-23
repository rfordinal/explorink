add_executable(OtaVersionCompareTest
  OtaVersionCompareTest.cpp
  ${REPO_ROOT}/src/network/OtaVersionCompare.cpp
)

target_include_directories(OtaVersionCompareTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(OtaVersionCompareTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaVersionCompareTest)
