add_executable(WalletTest
  WalletTest.cpp
  ${REPO_ROOT}/src/activities/wallet/WalletManifestParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  # The hint-label width check measures with the firmware's own font table
  # (WalletCodeHints.*). EpdFont needs no Arduino and no zlib for metrics -- only
  # glyph decompression would, and nothing here draws.
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(WalletTest PRIVATE
  ${REPO_ROOT}/src/activities/wallet
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/wallet_preview
)

target_compile_definitions(WalletTest PRIVATE
  WALLET_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  # Read at test time, so a label edited in the yaml is measured as edited -- a
  # copy of the string in the test could not catch a translator making it longer.
  WALLET_I18N_DIR="${REPO_ROOT}/lib/I18n/translations"
)

target_link_libraries(WalletTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WalletTest)
