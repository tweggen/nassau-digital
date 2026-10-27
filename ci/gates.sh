#!/usr/bin/env bash
#
# ci/gates.sh - this repository's gate, in one command (R1).
#
#     ./ci/gates.sh
#
# Configures its own build-ci/ tree, builds the core and the whole suite
# (the build runs the R2 header check, C0.2), and runs every test. CI runs
# exactly this script on Linux, macOS and Windows, so "it passed here" and "it
# passed in CI" mean the same thing. The logic lives here, not in the workflow,
# so a move off GitHub rewrites the YAML that calls it, not the gate.
#
# The core needs no SDK, no submodules and no credentials. It needs
# nassau-dsp, which cmake/NassauDSP.cmake finds as -DNASSAU_DSP_DIR, a sibling
# ../nassau-dsp, or (CI, a bare clone) a fetch of the pinned public commit;
# the configure log says which ("nassau-dsp: ...").
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_DIR="$REPO_DIR/build-ci"

if [ $# -gt 0 ]; then
    echo "usage: ci/gates.sh   (no arguments; BUILD_TYPE=Debug to change the build type)" >&2
    exit 2
fi

say() { printf '\n=== %s ===\n' "$*"; }

say "Gate: NassauDigital core + tests ($BUILD_TYPE)"

# No -G: CMake honours the CMAKE_GENERATOR environment variable, so a runner
# can pick Ninja without this script caring, and a bare box still works.
say "Configure"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

say "Build"
# --config and -C as well as CMAKE_BUILD_TYPE: a single-config generator
# (Ninja, Makefiles) reads CMAKE_BUILD_TYPE, a multi-config one (Visual
# Studio, the Windows default) ignores it and reads --config/-C. Without them
# Visual Studio silently builds Debug and ctest finds no tests.
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j

say "Test"
ctest --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" --output-on-failure --no-tests=error

say "Pass"
