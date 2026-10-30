add_executable(GroupMigrationTest
  GroupMigrationTest.cpp
)

target_compile_definitions(GroupMigrationTest PRIVATE ENABLE_GROUP_MARKERS=1)

target_include_directories(GroupMigrationTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(GroupMigrationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(GroupMigrationTest)
