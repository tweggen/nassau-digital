#!/usr/bin/env bash
#
# ci/build.sh - build the core and its tests for development, into build/.
#
#     ./ci/build.sh            # Release build of the core and the suite
#     ./ci/build.sh --debug    # CMAKE_BUILD_TYPE=Debug
#     ./ci/build.sh --clean    # drop build/ first
#     ./ci/build.sh --test     # ...and run ctest afterwards
#     ./ci/build.sh -- -DFOO=bar   # extra arguments for the configure step
#
# The developer's build. The gate (ci/gates.sh) uses its own build-ci/ tree,
# so running one never invalidates the other. Nothing is installed: this repo
# is a library that qbx builds from source through its locator (DESIGN §9.2).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build"
BUILD_TYPE="Release"
CLEAN=""
TEST=""
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --debug) BUILD_TYPE="Debug"; shift ;;
        --clean) CLEAN="yes"; shift ;;
        --test)  TEST="yes"; shift ;;
        --)      shift; EXTRA=("$@"); break ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "usage: ci/build.sh [--debug] [--clean] [--test] [-- <cmake args>]" >&2; exit 2 ;;
    esac
done

say() { printf '\n=== %s ===\n' "$*"; }

if [ -n "$CLEAN" ] && [ -e "$BUILD_DIR" ]; then
    say "Removing build/"
    rm -rf -- "$BUILD_DIR"
fi

say "Configure ($BUILD_TYPE)"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${EXTRA[@]+"${EXTRA[@]}"}"

say "Build"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j

if [ -n "$TEST" ]; then
    say "Test"
    ctest --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" --output-on-failure
fi

say "Done"
