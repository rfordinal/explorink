add_executable(FirmwareBoardTagTest
  FirmwareBoardTagTest.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)

target_include_directories(FirmwareBoardTagTest PRIVATE
  ${REPO_ROOT}/src/network
)

# FirmwareBoardTag.cpp picks its board name from exactly one FREEINK_DEVICE_*
# flag (same coherence rule BoardConfig.h enforces at the SDK level) -- the
# choice here is arbitrary, the tests just need a known, fixed TAG to probe.
target_compile_definitions(FirmwareBoardTagTest PRIVATE FREEINK_DEVICE_X4PRO=1)

target_link_libraries(FirmwareBoardTagTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareBoardTagTest)
