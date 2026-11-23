add_executable(LoraIdentityTest IdentityLoadRuleTest.cpp)

# Same reason as test/lora_peers: a build-flag-gated feature that nobody
# compiles rots, and the tests are the one place this is always on.
target_compile_definitions(LoraIdentityTest PRIVATE ENABLE_LORA_MESH=1)
target_include_directories(LoraIdentityTest PRIVATE ${REPO_ROOT}/src/lora)
target_link_libraries(LoraIdentityTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(LoraIdentityTest)
