# The test suite: one executable per gate, each a plain int main() with soft
# checks (Tests/test_util.h), registered with ctest.

function(nd_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE nassau_digital_core)
    target_compile_options(${name} PRIVATE ${ND_WARNING_FLAGS})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---- C0.1: the scaffold ----------------------------------------------------
nd_add_test(scaffold_tests)

# ---- C1: program model and XML -----------------------------------------------
nd_add_test(program_tests)
target_compile_definitions(program_tests PRIVATE
    ND_FIXTURE_PROGRAM_FULL="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/program_full.xml")

# ---- C2: one voice ------------------------------------------------------------
nd_add_test(voice_tests)
nd_add_test(interp_tests)

# ---- C3: layers, velocity, round-robin, voices ---------------------------------
nd_add_test(layers_tests)

# ---- C4: envelopes and modulation ----------------------------------------------
nd_add_test(mod_tests)

# ---- C8: libraries, base and overlay -----------------------------------------------
nd_add_test(overlay_tests)

# ---- C0.2: the R2 header check, on the real core and on fixtures ------------
# The build already runs the check on Source/Core (Source/Core/CMakeLists.txt);
# this repeats it as a test so a ctest run reports it by name, and proves the
# checker itself bites on each kind of violation. A failing fixture test is
# matched on the checker's own failure text, not on a bare non-zero exit, so a
# checker that fails for an unrelated reason (a missing directory, a syntax
# error) does not count as catching the violation.
set(ND_HC_SCRIPT ${PROJECT_SOURCE_DIR}/cmake/CheckCoreHeaders.cmake)
set(ND_HC_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/header_check)

add_test(NAME header_check_core
    COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${PROJECT_SOURCE_DIR}/Source/Core -P ${ND_HC_SCRIPT})
set_tests_properties(header_check_core PROPERTIES
    PASS_REGULAR_EXPRESSION "R2 header check: [0-9]+ include\\(s\\).*all allowed")

add_test(NAME header_check_fixture_good
    COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${ND_HC_FIXTURES}/good -P ${ND_HC_SCRIPT})
set_tests_properties(header_check_fixture_good PROPERTIES
    PASS_REGULAR_EXPRESSION "R2 header check: 7 include\\(s\\) in 3 file\\(s\\).*all allowed")

foreach(kind thirdparty cheader escape missing macro)
    add_test(NAME header_check_fixture_bad_${kind}
        COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${ND_HC_FIXTURES}/bad_${kind} -P ${ND_HC_SCRIPT})
    set_tests_properties(header_check_fixture_bad_${kind} PROPERTIES
        PASS_REGULAR_EXPRESSION "R2 header check FAILED: 1 include")
endforeach()
