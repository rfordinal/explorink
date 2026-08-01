"""
PlatformIO post-load script: register a `map-preview` custom target so
`pio run -t map-preview` builds and runs the native MapRenderer preview
under test/map_preview/ (a plain executable, not a gtest suite -- reuses the
same CMake/CTest host-build infra as `unit-tests`, see
register_unit_tests_target.py).

This is a host build (plain g++ via CMake), not a firmware build -- no
PlatformIO/ESP-IDF toolchain involved. See
docs/firmware-implementation-plan.md Phase 1 in the parent xteink repo.
"""

import os

Import("env")  # noqa: F821  -- provided by PlatformIO at script load

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
BUILD_DIR = os.path.join(PROJECT_DIR, "build", "test")
TEST_SRC_DIR = os.path.join(PROJECT_DIR, "test")
OUTPUT_PPM = os.path.join(BUILD_DIR, "map_preview", "map_preview.ppm")

env.AddCustomTarget(  # noqa: F821
    name="map-preview",
    dependencies=None,
    actions=[
        f'cmake -S "{TEST_SRC_DIR}" -B "{BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release',
        f'cmake --build "{BUILD_DIR}" --target map_preview',
        f'"{BUILD_DIR}/map_preview/map_preview" "{OUTPUT_PPM}"',
    ],
    title="Map preview (native)",
    description="Render MapRenderer's mock view to a PPM image, no flashing/ESP32 toolchain",
)
