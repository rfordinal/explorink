# Plain executable, not a gtest suite -- ctest does not run it (test/README).
add_executable(wallet_preview
  wallet_preview.cpp
  ${REPO_ROOT}/src/activities/wallet/WalletManifestParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(wallet_preview PRIVATE
  ${REPO_ROOT}/src/activities/wallet
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/test/wallet
)

# OpenSSL renders an encrypted tree (--key): the device's mbedtls is not linkable
# here, so the same shared arithmetic runs against a second primitive
# implementation (test/wallet/WalletCryptoHost.h).
find_package(OpenSSL REQUIRED)

target_link_libraries(wallet_preview PRIVATE
  crosspoint_test_common
  OpenSSL::Crypto
  ZLIB::ZLIB
)
