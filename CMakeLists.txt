cmake_minimum_required(VERSION 3.15)
project(NassauDigital VERSION 0.1.0 LANGUAGES CXX)

# The dependency-free core of NassauDigital (docs/DESIGN.md): no Qt, no IPlug2,
# no SDK (R2). Unlike the plugin repos this project does NOT include the
# Nassau SDK's project prologue: the core must build from its own checkout,
# because qbx builds it from source (DESIGN §9.2).
#
# It builds as a SUBPROJECT too: a consumer finds this checkout with the
# locator cmake/bootstrap/NassauDigital.cmake (copied verbatim into it), which
# add_subdirectory()s it EXCLUDE_FROM_ALL and links nassau::digital. As a
# subproject it sets no global build type and adds no tests: the consumer's
# build gets the core target and nothing else.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ND_TOP_LEVEL ON)
else()
    set(ND_TOP_LEVEL OFF)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ISO C++ rather than gnu++17. Besides portability this keeps GCC's
# floating-point contraction at its ISO default (off) instead of the GNU
# mode's "fast", so an a*b+c is not silently fused differently from one
# build to the next (R8).
set(CMAKE_CXX_EXTENSIONS OFF)

if(ND_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Warnings for everything this project compiles. Deliberately no
# -ffast-math and no -march=native (R7): CMake's default Release flags
# (-O3 -DNDEBUG, MSVC /O2) are portable and keep IEEE semantics.
if(MSVC)
    set(ND_WARNING_FLAGS /W4 /permissive- /utf-8)
else()
    set(ND_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

# nassau-dsp (DESIGN §11, gate X1): the ladder, the SVF, the HP cascade and
# the shapers the voice's filter and drive are built from (gate C5), a
# header-only library shared with nassau-analogue. It is pinned ONCE for a
# release, as a top-level nassau-suite submodule that passes NASSAU_DSP_DIR,
# never as a submodule here (DESIGN §9.2). cmake/NassauDSP.cmake is a VERBATIM
# copy of nassau-dsp's cmake/bootstrap/NassauDSP.cmake: it finds an explicit
# NASSAU_DSP_DIR, then a sibling ../nassau-dsp, then fetches the ref below.
# NASSAU_DSP_GIT_REF is what a bare clone (and CI) builds: a commit, never a
# branch; move it in the same change that needs newer nassau-dsp code.
# NASSAU_DSP_FLOAT stays off: the core's filters run in double, which is
# what C5.2's byte comparison against nassau-dsp's golden assumes.
set(NASSAU_DSP_GIT_REF "76e38fb3207092fbc3139a588b2726e9ad1405cd")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/NassauDSP.cmake)

# The core, plus the R2 header check that guards it (C0.2).
add_subdirectory(Source/Core)

# Tests: the hand-rolled harness (Tests/test_util.h), built by default only
# when this is the top-level project.
option(ND_BUILD_TESTS "Build the test suite" ${ND_TOP_LEVEL})
if(ND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()
