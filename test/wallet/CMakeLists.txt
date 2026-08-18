add_executable(WalletTest
  WalletTest.cpp
  ${REPO_ROOT}/src/activities/wallet/WalletManifestParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(WalletTest PRIVATE
  ${REPO_ROOT}/src/activities/wallet
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/test/wallet_preview
)

target_compile_definitions(WalletTest PRIVATE
  WALLET_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(WalletTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WalletTest)
