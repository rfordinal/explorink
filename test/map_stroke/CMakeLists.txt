add_executable(MapStrokeTest
  MapStrokeTest.cpp
  ${REPO_ROOT}/test/map_preview/PpmCanvas.cpp
  ${REPO_ROOT}/test/map_preview/PreviewFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(MapStrokeTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/test/map_preview
)

target_link_libraries(MapStrokeTest PRIVATE
  ZLIB::ZLIB
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(MapStrokeTest map_generated_headers)

gtest_discover_tests(MapStrokeTest)
