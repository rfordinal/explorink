# The device window: the real MapFollow decisions and the real MapRenderer, in
# one SDL2 window at 480x800, while a recorded ride plays. See window_main.cpp.
#
# Optional target. SDL2 is a system package (`apt install libsdl2-dev`) and
# nothing else in this suite needs it, so when it is missing this one target is
# skipped with a note instead of failing configure for everyone.
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  message(STATUS "map_window: SDL2 not found -- target skipped (apt install libsdl2-dev)")
  return()
endif()

set(PREVIEW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../map_preview")
set(REPLAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../map_replay")

add_executable(map_window
  window_main.cpp
  # The decision half, linked from map_replay -- the same Stepper that
  # `map_replay --check` gates against three real X4 rides.
  ${REPLAY_DIR}/ReplayEngine.cpp
  ${REPLAY_DIR}/RideLog.cpp
  # The drawing half, linked from map_preview -- the firmware's own renderer
  # and its own font tables.
  ${PREVIEW_DIR}/MapPreviewPipeline.cpp
  ${PREVIEW_DIR}/HeapProbe.cpp
  ${PREVIEW_DIR}/PpmCanvas.cpp
  ${PREVIEW_DIR}/PreviewFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/src/activities/map/MapAreaFill.cpp
  ${REPO_ROOT}/src/activities/map/MapFollow.cpp
  ${REPO_ROOT}/src/activities/map/MapHatch.cpp
  ${REPO_ROOT}/src/activities/map/MapLabels.cpp
  ${REPO_ROOT}/src/activities/map/MapModeMask.cpp
  ${REPO_ROOT}/src/activities/map/MapPointMarks.cpp
  ${REPO_ROOT}/src/activities/map/MapPointReader.cpp
  ${REPO_ROOT}/src/activities/map/MapPointSource.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/MapRenderer.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteFit.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteReader.cpp
  ${REPO_ROOT}/src/activities/map/MapRouteSource.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
  ${REPO_ROOT}/src/activities/map/MapTileReader.cpp
  ${REPO_ROOT}/src/activities/map/MapTileSource.cpp
  ${REPO_ROOT}/src/activities/map/MapViewport.cpp
)

target_include_directories(map_window PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include
  ${PREVIEW_DIR}
  ${REPLAY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# zlib decompresses the built-in fonts' glyph groups, same as map_preview.
find_package(ZLIB REQUIRED)
target_link_libraries(map_window PRIVATE crosspoint_test_common ZLIB::ZLIB)

if(TARGET SDL2::SDL2)
  target_link_libraries(map_window PRIVATE SDL2::SDL2)
else()
  target_include_directories(map_window PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(map_window PRIVATE ${SDL2_LIBRARIES})
endif()

# MapStyleDefaults.h / MapModeMaskDefaults.h come out of data/mapstyle.json, so
# a style edit reaches this window on the next build -- same as map_preview.
add_dependencies(map_window map_generated_headers)
