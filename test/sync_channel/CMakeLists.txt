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
