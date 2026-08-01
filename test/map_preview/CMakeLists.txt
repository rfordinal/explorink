add_executable(map_preview
  preview_main.cpp
  PpmCanvas.cpp
  ${REPO_ROOT}/src/activities/map/MapRenderer.cpp
)

target_include_directories(map_preview PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(map_preview PRIVATE crosspoint_test_common)
