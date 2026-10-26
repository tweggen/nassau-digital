# The NassauDigital core (docs/DESIGN.md §9.2): the program model, its XML
# format and the voice DSP. Standard headers only (R2), checked below.

add_library(nassau_digital_core STATIC
    nd_version.h
    nd_version.cpp
    nd_xml.h
    nd_xml.cpp
    nd_program.h
    nd_program.cpp
    nd_program_xml.h
    nd_program_xml.cpp
    nd_edit.h
    nd_edit.cpp
    nd_sample.h
    nd_interp.h
    nd_envelope.h
    nd_voice_fx.h
    nd_instrument.h
    nd_instrument.cpp
)
target_include_directories(nassau_digital_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(nassau_digital_core PRIVATE ${ND_WARNING_FLAGS})

# ---- C0.2: the R2 header check -------------------------------------------
# Runs on every build, before the core compiles: any #include under
# Source/Core/ that is neither a C++17 standard header nor a file of the core
# itself fails the build. The script is plain CMake so it needs nothing a CMake
# build does not already have, on any of the three CI hosts.
set(ND_HEADER_CHECK_SCRIPT ${PROJECT_SOURCE_DIR}/cmake/CheckCoreHeaders.cmake)
file(GLOB_RECURSE ND_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
set(ND_HEADER_CHECK_STAMP ${CMAKE_CURRENT_BINARY_DIR}/core_header_check.stamp)
add_custom_command(
    OUTPUT ${ND_HEADER_CHECK_STAMP}
    COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${ND_HEADER_CHECK_SCRIPT}
    COMMAND ${CMAKE_COMMAND} -E touch ${ND_HEADER_CHECK_STAMP}
    DEPENDS ${ND_CORE_SOURCES} ${ND_HEADER_CHECK_SCRIPT}
    COMMENT "R2 header check of Source/Core (C0.2)"
    VERBATIM)
add_custom_target(core_header_check DEPENDS ${ND_HEADER_CHECK_STAMP})
add_dependencies(nassau_digital_core core_header_check)
