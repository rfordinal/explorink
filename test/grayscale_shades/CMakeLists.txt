add_executable(GrayShadeTest
  GrayShadeTest.cpp
)

target_include_directories(GrayShadeTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(GrayShadeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GrayShadeTest)
