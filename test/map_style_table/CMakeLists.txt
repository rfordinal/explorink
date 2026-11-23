add_executable(MapStyleTableTest
  MapStyleTableTest.cpp
)

target_include_directories(MapStyleTableTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapStyleTableTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# MapStyleDefaults.h and MapModeMaskDefaults.h are both generated from
# data/mapstyle.json (test/CMakeLists.txt). This suite is about what the
# generators produced, so it must not run against a stale pair.
add_dependencies(MapStyleTableTest map_generated_headers)

gtest_discover_tests(MapStyleTableTest)
