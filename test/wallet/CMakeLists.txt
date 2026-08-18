add_executable(WalletTest
  WalletTest.cpp
  ${REPO_ROOT}/src/activities/wallet/WalletManifestParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(WalletTest PRIVATE
  ${REPO_ROOT}/src/activities/wallet
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(WalletTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WalletTest)
