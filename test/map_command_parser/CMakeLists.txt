add_executable(MapCommandParserTest
  MapCommandParserTest.cpp
  ${REPO_ROOT}/src/activities/map/MapCommandParser.cpp
  ${REPO_ROOT}/src/activities/map/MapCommandConsole.cpp
  ${REPO_ROOT}/src/activities/map/HeldTilesStore.cpp
  ${REPO_ROOT}/src/activities/map/MapModeMask.cpp
  # The console formats `pin log` records, so it needs the record codec. The pins
  # feature's own cases live in test/pins.
  ${REPO_ROOT}/src/activities/map/PinRecord.cpp
)

target_include_directories(MapCommandParserTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapCommandParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(MapCommandParserTest map_generated_headers)

gtest_discover_tests(MapCommandParserTest)
