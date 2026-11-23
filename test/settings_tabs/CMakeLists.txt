add_executable(SettingsTabsTest
  SettingsTabsTest.cpp
  ${REPO_ROOT}/test/map_preview/PreviewFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(SettingsTabsTest PRIVATE
  ${REPO_ROOT}/test/map_preview
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

# The built-in fonts' glyph groups are DEFLATE streams, same as the map preview
# (test/map_preview/PreviewFont.cpp).
find_package(ZLIB REQUIRED)

target_link_libraries(SettingsTabsTest PRIVATE
  crosspoint_test_common
  ZLIB::ZLIB
  GTest::gtest_main
)

gtest_discover_tests(SettingsTabsTest)
