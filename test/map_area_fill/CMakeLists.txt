add_executable(MapAreaFillTest
  MapAreaFillTest.cpp
  ${REPO_ROOT}/src/activities/map/MapAreaFill.cpp
  ${REPO_ROOT}/test/map_preview/PpmCanvas.cpp
  ${REPO_ROOT}/test/map_preview/PreviewFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(MapAreaFillTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/test/map_preview
)

target_link_libraries(MapAreaFillTest PRIVATE
  ZLIB::ZLIB
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(MapAreaFillTest map_generated_headers)

gtest_discover_tests(MapAreaFillTest)
