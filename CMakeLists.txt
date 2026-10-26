cmake_minimum_required(VERSION 3.15)
project(NassauDigital VERSION 0.1.0 LANGUAGES CXX)

# The dependency-free core of NassauDigital (docs/DESIGN.md): no Qt, no IPlug2,
# no SDK (R2). Unlike the plugin repos this project does NOT include the
# Nassau SDK's project prologue: the core must build from its own checkout,
# because qbx takes it as a submodule (DESIGN §9.2).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ISO C++ rather than gnu++17. Besides portability this keeps GCC's
# floating-point contraction at its ISO default (off) instead of the GNU
# mode's "fast", so an a*b+c is not silently fused differently from one
# build to the next (R8).
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
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

# The core, plus the R2 header check that guards it (C0.2).
add_subdirectory(Source/Core)

# Tests: the hand-rolled harness (Tests/test_util.h), always built.
option(ND_BUILD_TESTS "Build the test suite" ON)
if(ND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()
