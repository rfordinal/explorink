add_executable(SyncChannelTest
  SyncNoiseTest.cpp
  SyncChannelTest.cpp
)

# SyncNoise.h and SyncChannel.h only: header-only and pure, the primitives
# injected (T-2098). OpenSSL is the host's crypto, mbedtls the device's.
target_include_directories(SyncChannelTest PRIVATE
  ${REPO_ROOT}/src/activities/wallet
  ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(OpenSSL REQUIRED)

target_link_libraries(SyncChannelTest PRIVATE
  crosspoint_test_common
  OpenSSL::Crypto
  GTest::gtest_main
)

gtest_discover_tests(SyncChannelTest)

# The device half on stdin/stdout, for scripts/sync_channel_phone.py -- a phone
# on an independent Noise implementation. A host tool, not a ctest.
add_executable(sync_channel_host SyncChannelHost.cpp)
target_include_directories(sync_channel_host PRIVATE
  ${REPO_ROOT}/src/activities/wallet
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sync_channel_host PRIVATE OpenSSL::Crypto)
