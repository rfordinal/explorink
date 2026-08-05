add_executable(MissingTilePriorityTest
  MissingTilePriorityTest.cpp
)

target_include_directories(MissingTilePriorityTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(MissingTilePriorityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MissingTilePriorityTest)
