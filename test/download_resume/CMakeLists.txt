add_executable(DownloadResumeTest
  DownloadResumeTest.cpp
)

target_include_directories(DownloadResumeTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(DownloadResumeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DownloadResumeTest)
