add_executable(map_preview
  preview_main.cpp
  MapPreviewPipeline.cpp
  HeapProbe.cpp
  PpmCanvas.cpp
  ${REPO_ROOT}/src/activities/map/MapRenderer.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
  ${REPO_ROOT}/src/activities/map/MapTileReader.cpp
  ${REPO_ROOT}/src/activities/map/MapTileSource.cpp
  ${REPO_ROOT}/src/activities/map/MapViewport.cpp
  ${REPO_ROOT}/src/activities/map/MapHatch.cpp
)

target_include_directories(map_preview PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(map_preview PRIVATE crosspoint_test_common)
