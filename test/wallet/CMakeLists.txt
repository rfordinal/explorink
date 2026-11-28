add_executable(WalletTest
  WalletTest.cpp
  # The device sealing an asset as it arrives (T-2095). Its own file: the offset
  # arithmetic it covers is a different subject from the read path, and
  # WalletTest.cpp is already long.
  WalletIngestTest.cpp
  # The device sealing the wallet's LIST as it arrives (T-2096). Separate from
  # the asset sealer above because the two are opposites -- one streams and
  # never holds the file, the other holds it whole because the read path cannot
  # stream (WalletManifestIngest.h).
  WalletManifestSealTest.cpp
  # Which screen Home > Wallet opens (T-2109). The screen-level branch is not
  # host testable; the predicate beside it is.
  WalletEntryTest.cpp
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
  # GrayShade.h only: the grey plane encoding, constexpr, no renderer and no panel
  # (lib/GfxRenderer/GrayShade.h). WalletAsset.h states the wallet's grey levels in
  # terms of it rather than repeating the table.
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/wallet_preview
  # components/lock/PatternLock.h only: the grid arithmetic the gate and the
  # wallet's provisioning both validate against. Header-only, no Arduino and no
  # renderer, which is the whole reason that check could move out of the screen.
  ${REPO_ROOT}/src
)

target_compile_definitions(WalletTest PRIVATE
  WALLET_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  # Read at test time, so a label edited in the yaml is measured as edited -- a
  # copy of the string in the test could not catch a translator making it longer.
  WALLET_I18N_DIR="${REPO_ROOT}/lib/I18n/translations"
)

# OpenSSL is the host's crypto for the P3 tests. The device uses mbedtls, which no
# host build here links -- the shared arithmetic is bound to one library on each
# side on purpose, so two independent primitives have to agree on one contract
# (test/wallet/WalletCryptoHost.h).
find_package(OpenSSL REQUIRED)

target_link_libraries(WalletTest PRIVATE
  crosspoint_test_common
  OpenSSL::Crypto
  GTest::gtest_main
)

gtest_discover_tests(WalletTest)
