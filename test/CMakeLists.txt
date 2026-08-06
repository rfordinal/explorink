cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The map headers generated from data/mapstyle.json. PlatformIO runs these two
# scripts as pre: steps, so a firmware build always has them; a host build had
# been reusing whatever a previous firmware build left in the source tree,
# which meant a style edit did not reach the native preview at all. Generating
# them here closes that gap and keeps one copy -- same paths PlatformIO writes,
# both gitignored.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MAPSTYLE_JSON "${REPO_ROOT}/data/mapstyle.json")
set(MAP_MODE_MASK_DEFAULTS "${REPO_ROOT}/src/activities/map/MapModeMaskDefaults.h")
set(MAP_STYLE_DEFAULTS "${REPO_ROOT}/src/activities/map/MapStyleDefaults.h")

add_custom_command(
  OUTPUT "${MAP_MODE_MASK_DEFAULTS}"
  COMMAND ${Python3_EXECUTABLE} "${REPO_ROOT}/scripts/gen_mode_masks.py" "${MAPSTYLE_JSON}" "${MAP_MODE_MASK_DEFAULTS}"
  DEPENDS "${MAPSTYLE_JSON}" "${REPO_ROOT}/scripts/gen_mode_masks.py"
  COMMENT "gen_mode_masks.py: mode class masks from mapstyle.json"
  VERBATIM
)

add_custom_command(
  OUTPUT "${MAP_STYLE_DEFAULTS}"
  COMMAND ${Python3_EXECUTABLE} "${REPO_ROOT}/scripts/gen_mapstyle.py" "${MAPSTYLE_JSON}" "${MAP_STYLE_DEFAULTS}"
  DEPENDS "${MAPSTYLE_JSON}" "${REPO_ROOT}/scripts/gen_mapstyle.py"
  COMMENT "gen_mapstyle.py: MapStyle from mapstyle.json"
  VERBATIM
)

add_custom_target(map_generated_headers DEPENDS "${MAP_MODE_MASK_DEFAULTS}" "${MAP_STYLE_DEFAULTS}")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(opds_filename)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(map_preview)
add_subdirectory(map_stroke)
add_subdirectory(map_area_fill)
add_subdirectory(map_tile_reader)
add_subdirectory(map_route)
add_subdirectory(map_command_parser)
add_subdirectory(missing_tile_priority)
add_subdirectory(map_tile_path)
add_subdirectory(map_follow)
add_subdirectory(grayscale_shades)
