add_executable(MapFlagRulesTest
  MapFlagRulesTest.cpp
  ${REPO_ROOT}/src/activities/map/MapRenderer.cpp
  ${REPO_ROOT}/src/activities/map/MapAreaFill.cpp
  ${REPO_ROOT}/src/activities/map/MapLabels.cpp
  ${REPO_ROOT}/src/activities/map/MapPointMarks.cpp
  ${REPO_ROOT}/test/map_preview/PpmCanvas.cpp
  ${REPO_ROOT}/test/map_preview/PreviewFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(MapFlagRulesTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
  ${REPO_ROOT}/test/map_preview
)

target_link_libraries(MapFlagRulesTest PRIVATE
  ZLIB::ZLIB
  crosspoint_test_common
  GTest::gtest_main
)

# Reads kDefaultMapStyle, so the header generated from data/mapstyle.json must
# exist first -- that is the half of this test that proves the *shipped* style
# still carries no flag rule.
add_dependencies(MapFlagRulesTest map_generated_headers)

gtest_discover_tests(MapFlagRulesTest)
