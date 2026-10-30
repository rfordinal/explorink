add_executable(LoraPeerTableTest
  LoraPeerTableTest.cpp
  ${REPO_ROOT}/src/lora/LoraPeerTable.cpp
)

# The table is build-flag gated so a board with no radio leaves it out
# entirely (platformio.ini, ENABLE_LORA_MESH). The tests are the one place it is
# always on: a gated-out feature that nobody compiles rots. Same reasoning as
# test/group.
target_compile_definitions(LoraPeerTableTest PRIVATE ENABLE_LORA_MESH=1)

target_include_directories(LoraPeerTableTest PRIVATE ${REPO_ROOT}/src/lora)

target_link_libraries(LoraPeerTableTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LoraPeerTableTest)
