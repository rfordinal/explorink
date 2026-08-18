# Plain executable, not a gtest suite -- ctest does not run it (test/README).
add_executable(wallet_preview
  wallet_preview.cpp
  ${REPO_ROOT}/src/activities/wallet/WalletManifestParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(wallet_preview PRIVATE
  ${REPO_ROOT}/src/activities/wallet
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(wallet_preview PRIVATE
  crosspoint_test_common
  ZLIB::ZLIB
)
