add_executable(ParkedLoopPolicyTest
  ParkedLoopPolicyTest.cpp
)

target_include_directories(ParkedLoopPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ParkedLoopPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ParkedLoopPolicyTest)
