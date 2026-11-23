add_executable(GroupTest
  GroupTest.cpp
  ${REPO_ROOT}/src/activities/map/GroupMembers.cpp
  ${REPO_ROOT}/src/activities/map/GroupRoster.cpp
  ${REPO_ROOT}/src/activities/map/GroupFix.cpp
  ${REPO_ROOT}/src/activities/map/GroupStore.cpp
  ${REPO_ROOT}/src/activities/map/GroupRecord.cpp
  # Day-file naming moved out of GroupRecord.cpp when the radio's event log
  # became the second caller (src/DayFile.h).
  ${REPO_ROOT}/src/DayFile.cpp
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
# entirely (platformio.ini, ENABLE_GROUP_MARKERS). The tests are the one place it
# is always on: a gated-out feature that nobody compiles rots.
target_compile_definitions(GroupTest PRIVATE
  ENABLE_GROUP_MARKERS=1
  ENABLE_GROUP_CMD=1
)

target_include_directories(GroupTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/BlePositionServer/include
)

target_link_libraries(GroupTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(GroupTest map_generated_headers)

gtest_discover_tests(GroupTest)
