add_executable(BeaconPolicyTest
  BeaconPolicyTest.cpp
  ${REPO_ROOT}/src/lora/BeaconPolicy.cpp
)

target_include_directories(BeaconPolicyTest PRIVATE
  ${REPO_ROOT}/src/lora
)

target_link_libraries(BeaconPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BeaconPolicyTest)
