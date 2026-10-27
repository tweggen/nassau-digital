# NassauDigital.cmake -- the nassau-digital locator (CANONICAL COPY)
# =============================================================================
# THIS FILE IS COPIED VERBATIM INTO EACH CONSUMER (qbx keeps it as
# smaragd/cmake/NassauDigital.cmake). The copy in nassau-digital/cmake/bootstrap/
# is the source of truth: change the order here and re-copy, never edit a
# consumer's copy in place. A consumer cannot include() a file from a directory
# it has not found yet, so the finding has to live in the consumer; that is the
# whole reason for a copy, as with nassau-dsp's NassauDSP.cmake and
# nassau-plugin-sdk's NassauSDK.cmake, which this file follows.
#
# -----------------------------------------------------------------------------
# ONE PIN PER REPO (the author's decision of 2026-10-26, DESIGN §9.2).
# nassau-digital is pinned exactly once for a release: as a top-level submodule
# of nassau-suite, which passes -DNASSAU_DIGITAL_DIR to every consumer and whose
# ci/release.sh therefore tags it. No consumer carries it as a nested
# submodule: a second pin is a second answer to which core shipped.
#
# RESOLUTION ORDER (one order, no per-repo variation):
#
#   1. An explicit -DNASSAU_DIGITAL_DIR=... (or one already in the cache). This
#      is what the nassau-suite superbuild passes. An explicit directory that
#      holds no nassau-digital checkout is FATAL, never a reason to fall back:
#      the suite asked for THAT checkout.
#   2. A sibling checkout at <NASSAU_DIGITAL_SIBLING_ROOT>/../nassau-digital,
#      for multi-repo development (and what an IDE opening the workspace sees).
#   3. FetchContent from NASSAU_DIGITAL_GIT_REPOSITORY at NASSAU_DIGITAL_GIT_REF,
#      so a bare clone of one consumer (and its CI) still builds. The repo is
#      public; no token.
#
# Finding nothing IS an error: a consumer that compiles the core has no smaller
# build to fall back to. Turn the fetch off with
# -DNASSAU_DIGITAL_ALLOW_FETCH=OFF to keep a build offline.
#
# The consumer sets, before including this file:
#
#   NASSAU_DIGITAL_GIT_REF        required: a commit sha or a tag, NEVER a
#                                 branch (a branch would make a bare clone
#                                 build different code on different days).
#   NASSAU_DIGITAL_SIBLING_ROOT   optional: the root of the consumer's own
#                                 checkout, beside which the sibling is looked
#                                 for. Default ${CMAKE_CURRENT_SOURCE_DIR},
#                                 right when the including CMakeLists.txt sits
#                                 at the checkout's root; a consumer whose
#                                 CMakeLists.txt sits deeper (qbx: smaragd/
#                                 tw303a/) names its root.
#
# Result: the core added with add_subdirectory(... EXCLUDE_FROM_ALL) (its
# tests are built only when it is the top-level project), the target
# nassau::digital, and NASSAU_DIGITAL_DIR set in the including scope (in the
# cache only when it was passed explicitly). Every configure logs
# the route taken and the commit built:
#
#   -- nassau-digital: <route>, <dir> at <commit>
# =============================================================================

set(NASSAU_DIGITAL_GIT_REPOSITORY "https://github.com/tweggen/nassau-digital.git"
    CACHE STRING "nassau-digital git remote, for the FetchContent fallback")
if(NOT NASSAU_DIGITAL_GIT_REF)
    message(FATAL_ERROR
        "NassauDigital.cmake: set NASSAU_DIGITAL_GIT_REF (a commit sha or tag) "
        "before including it.")
endif()
if(NOT NASSAU_DIGITAL_SIBLING_ROOT)
    set(NASSAU_DIGITAL_SIBLING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

function(_nassau_digital_is_usable dir out)
    if(dir AND EXISTS "${dir}/CMakeLists.txt" AND EXISTS "${dir}/Source/Core/nd_instrument.h")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(_nassau_digital_found FALSE)
set(_nassau_digital_how "")

# --- 1. explicit ---------------------------------------------------------------
if(NASSAU_DIGITAL_DIR)
    _nassau_digital_is_usable("${NASSAU_DIGITAL_DIR}" _nassau_digital_found)
    if(NOT _nassau_digital_found)
        message(FATAL_ERROR
            "nassau-digital: NASSAU_DIGITAL_DIR=${NASSAU_DIGITAL_DIR} holds no "
            "nassau-digital checkout (no CMakeLists.txt and Source/Core/nd_instrument.h). "
            "An explicit directory is never a reason to fall back to a sibling or a fetch: "
            "fix the path, or drop -DNASSAU_DIGITAL_DIR (and its cache entry) to let the "
            "locator search.")
    endif()
    get_filename_component(NASSAU_DIGITAL_DIR "${NASSAU_DIGITAL_DIR}" ABSOLUTE)
    set(_nassau_digital_how "explicit NASSAU_DIGITAL_DIR")
endif()

# --- 2. sibling checkout -------------------------------------------------------
if(NOT _nassau_digital_found)
    get_filename_component(_nassau_digital_sibling
        "${NASSAU_DIGITAL_SIBLING_ROOT}/../nassau-digital" ABSOLUTE)
    _nassau_digital_is_usable("${_nassau_digital_sibling}" _nassau_digital_found)
    if(_nassau_digital_found)
        set(NASSAU_DIGITAL_DIR "${_nassau_digital_sibling}")
        set(_nassau_digital_how "sibling checkout")
    endif()
endif()

# --- 3. FetchContent -----------------------------------------------------------
option(NASSAU_DIGITAL_ALLOW_FETCH
    "Allow fetching nassau-digital from git when no checkout is found" ON)

if(NOT _nassau_digital_found AND NASSAU_DIGITAL_ALLOW_FETCH)
    include(FetchContent)
    # SOURCE_SUBDIR names a directory that does not exist, so MakeAvailable
    # only downloads: the add_subdirectory below is the one for every route,
    # with EXCLUDE_FROM_ALL and one binary directory.
    FetchContent_Declare(nassau_digital_src
        GIT_REPOSITORY "${NASSAU_DIGITAL_GIT_REPOSITORY}"
        GIT_TAG        "${NASSAU_DIGITAL_GIT_REF}"
        SOURCE_SUBDIR  "_nassau_digital_download_only")
    FetchContent_MakeAvailable(nassau_digital_src)
    _nassau_digital_is_usable("${nassau_digital_src_SOURCE_DIR}" _nassau_digital_found)
    if(_nassau_digital_found)
        set(NASSAU_DIGITAL_DIR "${nassau_digital_src_SOURCE_DIR}")
        set(_nassau_digital_how "fetched ${NASSAU_DIGITAL_GIT_REF}")
    endif()
endif()

if(NOT _nassau_digital_found)
    get_filename_component(_nassau_digital_clone_to
        "${NASSAU_DIGITAL_SIBLING_ROOT}/../nassau-digital" ABSOLUTE)
    if(NASSAU_DIGITAL_ALLOW_FETCH)
        set(_nassau_digital_fetch "failed")
    else()
        set(_nassau_digital_fetch "off")
    endif()
    message(FATAL_ERROR
        "nassau-digital not found (no NASSAU_DIGITAL_DIR, no sibling checkout at "
        "${_nassau_digital_clone_to}, fetch ${_nassau_digital_fetch}). "
        "Clone it beside this repo:\n"
        "    git clone ${NASSAU_DIGITAL_GIT_REPOSITORY} ${_nassau_digital_clone_to}\n"
        "or pass -DNASSAU_DIGITAL_DIR=<checkout>, or allow the fetch "
        "(-DNASSAU_DIGITAL_ALLOW_FETCH=ON).")
endif()

# Only an EXPLICIT directory lives in the cache. A sibling or fetched one is a
# plain variable of the including scope, so the next configure searches again
# and logs its own route: a cached sibling path would come back as "explicit"
# and hide a sibling that has since gone, or a fetch that should now be taken.

if(NOT TARGET nassau::digital)
    add_subdirectory("${NASSAU_DIGITAL_DIR}" "${CMAKE_BINARY_DIR}/_deps/nassau-digital-build"
                     EXCLUDE_FROM_ALL)
endif()

# Say which commit was built, every configure: with three routes the question
# "which core is in this build?" must be answered in the log, not guessed.
find_package(Git QUIET)
set(_nassau_digital_rev "unknown")
if(GIT_FOUND AND EXISTS "${NASSAU_DIGITAL_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${NASSAU_DIGITAL_DIR}" rev-parse --short=12 HEAD
                    OUTPUT_VARIABLE _nassau_digital_rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
message(STATUS "nassau-digital: ${_nassau_digital_how}, ${NASSAU_DIGITAL_DIR} at ${_nassau_digital_rev}")

# A checkout from before this locator existed builds the core but names no
# nassau::digital; a consumer linking it would fail at generate time, naming
# neither this file nor the pin. Stop here instead, saying which.
if(NOT TARGET nassau::digital)
    message(FATAL_ERROR
        "nassau-digital at ${NASSAU_DIGITAL_DIR} (${_nassau_digital_rev}) defines no "
        "nassau::digital target: it predates cmake/bootstrap/NassauDigital.cmake. Move "
        "that checkout, pin or NASSAU_DIGITAL_GIT_REF to a commit that has the locator.")
endif()
