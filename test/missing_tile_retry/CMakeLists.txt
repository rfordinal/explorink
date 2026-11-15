add_executable(MissingTileRetryTest
  MissingTileRetryTest.cpp
)

target_include_directories(MissingTileRetryTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(MissingTileRetryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MissingTileRetryTest)
