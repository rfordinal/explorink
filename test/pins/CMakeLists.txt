add_executable(PinsTest
  PinsTest.cpp
  ${REPO_ROOT}/src/activities/map/PinRecord.cpp
  ${REPO_ROOT}/src/activities/map/PinStore.cpp
  ${REPO_ROOT}/src/activities/map/PinLogScanner.cpp
  ${REPO_ROOT}/src/activities/map/PinGeo.cpp
  ${REPO_ROOT}/src/activities/map/MapCommandParser.cpp
  ${REPO_ROOT}/src/activities/map/MapCommandConsole.cpp
  ${REPO_ROOT}/src/activities/map/HeldTilesStore.cpp
  ${REPO_ROOT}/src/activities/map/MapModeMask.cpp
)

target_include_directories(PinsTest PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/lib/BlePositionServer/include
)

target_link_libraries(PinsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(PinsTest map_generated_headers)

gtest_discover_tests(PinsTest)
