add_executable(TeamTest
  TeamTest.cpp
  ${REPO_ROOT}/src/activities/map/TeamMembers.cpp
  ${REPO_ROOT}/src/activities/map/TeamRoster.cpp
  ${REPO_ROOT}/src/activities/map/TeamFix.cpp
  ${REPO_ROOT}/src/activities/map/TeamStore.cpp
  ${REPO_ROOT}/src/activities/map/TeamRecord.cpp
  ${REPO_ROOT}/src/activities/map/MapCommandParser.cpp
  ${REPO_ROOT}/src/activities/map/MapCommandConsole.cpp
  ${REPO_ROOT}/src/activities/map/HeldTilesStore.cpp
  ${REPO_ROOT}/src/activities/map/MapModeMask.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
  ${REPO_ROOT}/src/activities/map/PinRecord.cpp
  ${REPO_ROOT}/src/activities/map/PinStore.cpp
  ${REPO_ROOT}/src/activities/map/PinLogScanner.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

# The feature is build-flag gated so a memory-tight device can leave it out
# entirely (platformio.ini, ENABLE_TEAM_MARKERS). The tests are the one place it
# is always on: a gated-out feature that nobody compiles rots.
target_compile_definitions(TeamTest PRIVATE
  ENABLE_TEAM_MARKERS=1
  ENABLE_TEAM_CMD=1
)

target_include_directories(TeamTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/BlePositionServer/include
)

target_link_libraries(TeamTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(TeamTest map_generated_headers)

gtest_discover_tests(TeamTest)
