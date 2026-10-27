# run_locator.cmake - configure the locator consumer in a fresh tree and echo
# its output, for one route of the locator (Tests/CMakeLists.txt).
#
#   cmake -DBUILD_DIR=<dir> -DLOCATOR=<file> -DCONSUMER=<dir> [-DARGS=a,b,...]
#         [-DFAKE_SIBLING=<dir>] -P run_locator.cmake
#
# ARGS is comma-separated (a ; would split the add_test argument). With
# FAKE_SIBLING, a stand-in checkout (the two files the locator tests for, and a
# CMakeLists.txt defining nassau::digital) is written at <FAKE_SIBLING> first.
# The test's PASS_REGULAR_EXPRESSION judges the output, so a configure that
# fails for a reason other than the one under test does not pass.
file(REMOVE_RECURSE "${BUILD_DIR}")
file(MAKE_DIRECTORY "${BUILD_DIR}")
if(FAKE_SIBLING)
    file(REMOVE_RECURSE "${FAKE_SIBLING}")
    file(WRITE "${FAKE_SIBLING}/Source/Core/nd_instrument.h" "// stand-in\n")
    file(WRITE "${FAKE_SIBLING}/CMakeLists.txt"
        "add_library(nd_fake_core INTERFACE)\nadd_library(nassau::digital ALIAS nd_fake_core)\n")
endif()
string(REPLACE "," ";" _args "${ARGS}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER}" -B "${BUILD_DIR}"
            "-DND_LOCATOR=${LOCATOR}" ${_args}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
message("${_out}${_err}")
message("run_locator: configure exit ${_rc}")
