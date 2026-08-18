"""
PlatformIO post-load script: register a `wallet-preview` custom target so
`pio run -t wallet-preview` builds and runs the native wallet reader preview
under test/wallet_preview/ (a plain executable, not a gtest suite -- reuses the
same CMake/CTest host-build infra as `unit-tests`, see
register_unit_tests_target.py and register_map_preview_target.py).

It renders the committed generator fixture (test/wallet/fixtures) through the
firmware's own WalletAsset / ManifestParser code and writes two PNGs: the panel's
own 800x480 landscape frame, and the same bits read the way a rider holds the
device. See docs/wallet-viewer.md, "Read against real generator output".

Point it at a bigger tree by running the binary directly:
  build/test/wallet_preview/wallet_preview --tree DIR --level detail --col 1 \
      --row 0 --out /tmp/tile

`--code N` renders a machine-readable code instead of a document level and
reports its sha256 check, module box, centring and label band -- see
docs/wallet-viewer.md, "The code screen":
  build/test/wallet_preview/wallet_preview --tree test/wallet/fixtures/codes \
      --code 0 --out /tmp/code

This is a host build (plain g++ via CMake), not a firmware build -- no
PlatformIO/ESP-IDF toolchain involved.
"""

import os

Import("env")  # noqa: F821  -- provided by PlatformIO at script load

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
BUILD_DIR = os.path.join(PROJECT_DIR, "build", "test")
TEST_SRC_DIR = os.path.join(PROJECT_DIR, "test")
FIXTURE_TREE = os.path.join(TEST_SRC_DIR, "wallet", "fixtures")
OUTPUT_PREFIX = os.path.join(BUILD_DIR, "wallet_preview", "wallet_fit")

env.AddCustomTarget(  # noqa: F821
    name="wallet-preview",
    dependencies=None,
    actions=[
        f'cmake -S "{TEST_SRC_DIR}" -B "{BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release',
        f'cmake --build "{BUILD_DIR}" --target wallet_preview',
        # The committed fixture holds the FIT sidecar only, not the 48 KB .dat.
        # The tool falls back to the sidecar and rebuilds the same bytes, so this
        # runs with no extra input. Override --tree for a full generated tree.
        f'"{BUILD_DIR}/wallet_preview/wallet_preview" --tree "{FIXTURE_TREE}" '
        f'--level fit --out "{OUTPUT_PREFIX}"',
    ],
    title="Wallet preview (native)",
    description="Render a generated wallet asset to PNG, no flashing/ESP32 toolchain",
)
