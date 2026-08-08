add_executable(map_replay
  replay_main.cpp
  ReplayEngine.cpp
  RideLog.cpp
  # The real decision code, linked -- not re-implemented. This is the whole
  # point of the tool: mapbuilder/tools/route_follow_sim.py and
  # tools/rederive_heading.py (parent repo) both hand-copied these constants
  # into Python and both carry a docstring warning that they can drift.
  ${REPO_ROOT}/src/activities/map/MapFollow.cpp
  ${REPO_ROOT}/src/activities/map/MapProjection.cpp
  ${REPO_ROOT}/src/activities/map/MapViewport.cpp
  ${REPO_ROOT}/src/activities/map/MapTileGrid.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(map_replay PRIVATE
  ${REPO_ROOT}/src/activities/map
  ${REPO_ROOT}/lib/JsonParser
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(map_replay PRIVATE crosspoint_test_common)

# MapViewport.h reads the marker anchor out of MapStyleDefaults.h, generated
# from data/mapstyle.json (test/CMakeLists.txt) -- same dependency map_preview
# has, and for the same reason.
add_dependencies(map_replay map_generated_headers)
