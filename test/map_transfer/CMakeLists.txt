add_executable(MapTransferTest
  MapTransferDecideTest.cpp
)

# MapTransferDecide.h and MapTransferPath.h only. Both are header-only and pure
# -- no Arduino, no NimBLE, no HalStorage, no renderer -- which is the whole
# point of T-2106: the receiver's decisions had to leave the .cpp before
# anything on the host could ask them a question.
target_include_directories(MapTransferTest PRIVATE
  ${REPO_ROOT}/src/activities/map
)

target_link_libraries(MapTransferTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MapTransferTest)
