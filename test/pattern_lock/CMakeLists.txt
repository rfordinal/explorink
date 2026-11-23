add_executable(PatternLockTest
  PatternLockTest.cpp
)

target_include_directories(PatternLockTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(PatternLockTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(PatternLockTest)
