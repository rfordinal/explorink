add_executable(DebugInputTest
  DebugInputTest.cpp
  ${REPO_ROOT}/src/DebugInput.cpp
)

target_include_directories(DebugInputTest PRIVATE
  ${REPO_ROOT}/src
)

# The injector only exists in devel builds, so the test has to build it the
# same way the firmware does.
target_compile_definitions(DebugInputTest PRIVATE ENABLE_BUTTON_CMD=1)

target_link_libraries(DebugInputTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DebugInputTest)
