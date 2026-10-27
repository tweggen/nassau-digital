# A minimal consumer of the canonical locator, for the locator tests
# (Tests/CMakeLists.txt, "the locator"). It includes
# cmake/bootstrap/NassauDigital.cmake exactly as a consumer's verbatim copy
# would be included, then checks what a consumer relies on.
cmake_minimum_required(VERSION 3.15)
project(NdLocatorConsumer LANGUAGES CXX)

set(NASSAU_DIGITAL_GIT_REF "${ND_TEST_GIT_REF}")
include("${ND_LOCATOR}")

if(NOT TARGET nassau::digital)
    message(FATAL_ERROR "locator consumer: no nassau::digital target")
endif()
# As a subproject the core adds no tests to its consumer's build.
if(TARGET scaffold_tests OR TARGET program_tests)
    message(FATAL_ERROR "locator consumer: the core's tests leaked into the consumer")
endif()
message(STATUS "locator consumer: OK, NASSAU_DIGITAL_DIR=${NASSAU_DIGITAL_DIR}")
