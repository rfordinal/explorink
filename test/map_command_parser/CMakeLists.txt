add_executable(MapCommandParserTest
  MapCommandParserTest.cpp
  ${REPO_ROOT}/src/activities/map/MapCommandParser.cpp
  ${REPO_ROOT}/src/activities/map/MapCommandConsole.cpp
  ${REPO_ROOT}/src/activities/map/MapModeMask.cpp
)

target_include_directories(MapCommandParserTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapCommandParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapCommandParserTest)
