# The encoder only, not the writer: LoraEventLog::append() needs HalStorage and
# a card, and the thing that rots is the format, not the file handling.
add_executable(LoraLogTest LoraEventLogTest.cpp ${REPO_ROOT}/src/lora/LoraEventLine.cpp ${REPO_ROOT}/src/DayFile.cpp)

# Same reason as test/lora_peers: a build-flag-gated feature that nobody
# compiles rots, and the tests are the one place this is always on.
target_compile_definitions(LoraLogTest PRIVATE ENABLE_LORA_MESH=1)
target_include_directories(LoraLogTest PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/src/lora)
target_link_libraries(LoraLogTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(LoraLogTest)
