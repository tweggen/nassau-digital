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

# ---- C5: drive, filter, kit bus ---------------------------------------------------
# C5.2 compares the voice's filters byte for byte with nassau-dsp's own golden
# (Tests/fixtures/filter_golden.bin there), found through NASSAU_DSP_DIR in
# every route the locator takes (explicit, sibling or fetched).
nd_add_test(fx_tests)
target_compile_definitions(fx_tests PRIVATE
    ND_DSP_FILTER_GOLDEN="${NASSAU_DSP_DIR}/Tests/fixtures/filter_golden.bin")

# ---- C8: libraries, base and overlay -----------------------------------------------
nd_add_test(overlay_tests)
# ---- C7: tempo modes by resampling ------------------------------------------------
nd_add_test(tempo_tests)

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
    COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${PROJECT_SOURCE_DIR}/Source/Core
            -DDSP_INCLUDE_DIR=${NASSAU_DSP_DIR}/Source/DSP -P ${ND_HC_SCRIPT})
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

# ---- The locator (cmake/bootstrap/NassauDigital.cmake) -----------------------
# Each route a consumer can take, configured in a fresh tree with a minimal
# consumer (locator/consumer). A route that succeeds must log its route and
# reach the consumer's own OK line (configure exit 0); a route that must stop
# must stop with ITS message (CMake wraps an error message's lines, so the
# patterns allow a newline wherever the message has a space). Network-free: the fetch route is exercised only
# with -DND_TEST_LOCATOR_FETCH=ON (and by every consumer's CI, which fetches).
set(ND_LOC_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/locator/run_locator.cmake)
set(ND_LOC_CONSUMER ${CMAKE_CURRENT_SOURCE_DIR}/locator/consumer)
set(ND_LOC_FILE ${PROJECT_SOURCE_DIR}/cmake/bootstrap/NassauDigital.cmake)
set(ND_LOC_WS ${CMAKE_CURRENT_BINARY_DIR}/locator_ws)
function(nd_add_locator_test name regex)
    add_test(NAME locator_${name}
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${ND_LOC_WS}/build_${name}
                -DLOCATOR=${ND_LOC_FILE} -DCONSUMER=${ND_LOC_CONSUMER} ${ARGN}
                -P ${ND_LOC_SCRIPT})
    set_tests_properties(locator_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

# 1. explicit: this very checkout, as the suite passes it. Also proves the core
#    configures as a subproject without adding its tests.
nd_add_locator_test(explicit
    "nassau-digital: explicit NASSAU_DIGITAL_DIR, [^\n]* at [^\n]*\n.*locator consumer: OK.*configure exit 0"
    "-DARGS=-DND_TEST_GIT_REF=0000000000000000000000000000000000000000,-DNASSAU_DIGITAL_DIR=${PROJECT_SOURCE_DIR},-DNASSAU_DIGITAL_ALLOW_FETCH=OFF")
# An explicit directory holding no core stops; it never falls back, even with
#    a usable sibling present and the fetch allowed.
nd_add_locator_test(explicit_empty
    "NASSAU_DIGITAL_DIR=[^ \n]*empty_checkout[ \n]+holds[ \n]+no[ \n]+nassau-digital[ \n]+checkout.*configure exit 1"
    "-DFAKE_SIBLING=${ND_LOC_WS}/nassau-digital"
    "-DARGS=-DND_TEST_GIT_REF=0000000000000000000000000000000000000000,-DNASSAU_DIGITAL_DIR=${ND_LOC_WS}/empty_checkout,-DNASSAU_DIGITAL_SIBLING_ROOT=${ND_LOC_WS}/consumer_root")
# 2. sibling: a stand-in checkout beside the consumer's root.
nd_add_locator_test(sibling
    "nassau-digital: sibling checkout, [^\n]*locator_ws/nassau-digital at [^\n]*\n.*locator consumer: OK.*configure exit 0"
    "-DFAKE_SIBLING=${ND_LOC_WS}/nassau-digital"
    "-DARGS=-DND_TEST_GIT_REF=0000000000000000000000000000000000000000,-DNASSAU_DIGITAL_SIBLING_ROOT=${ND_LOC_WS}/consumer_root,-DNASSAU_DIGITAL_ALLOW_FETCH=OFF")
# No checkout anywhere and the fetch off: stop with the clone command.
nd_add_locator_test(none_fetch_off
    "nassau-digital not found.*fetch off\\)\\..*git clone https://github.com/tweggen/nassau-digital.git [^ \n]*nowhere/nassau-digital\n.*configure exit 1"
    "-DARGS=-DND_TEST_GIT_REF=0000000000000000000000000000000000000000,-DNASSAU_DIGITAL_SIBLING_ROOT=${ND_LOC_WS}/nowhere/root,-DNASSAU_DIGITAL_ALLOW_FETCH=OFF")
# A checkout from before the locator (no nassau::digital): stop, naming it,
#    rather than let the consumer's link fail at generate time.
nd_add_locator_test(predates
    "defines[ \n]+no[ \n]+nassau::digital[ \n]+target:[ \n]+it[ \n]+predates.*configure exit 1"
    "-DFAKE_OLD=${ND_LOC_WS}/old_checkout"
    "-DARGS=-DND_TEST_GIT_REF=0000000000000000000000000000000000000000,-DNASSAU_DIGITAL_DIR=${ND_LOC_WS}/old_checkout")
set_tests_properties(locator_explicit_empty locator_sibling PROPERTIES RESOURCE_LOCK nd_locator_fake_sibling)
# The ref these tests never fetch is a placeholder; the locator refuses an
# unset one, which is what a consumer forgetting its pin would hit.
nd_add_locator_test(no_ref
    "NassauDigital.cmake: set NASSAU_DIGITAL_GIT_REF.*configure exit 1"
    "-DARGS=-DNASSAU_DIGITAL_DIR=${PROJECT_SOURCE_DIR}")

# 3. fetch (network): the pinned commit, with no sibling to find.
option(ND_TEST_LOCATOR_FETCH "Also test the locator's fetch route (needs github.com)" OFF)
# The commit to fetch must already carry nassau::digital (this locator's
# change or later), so there is no default: name one.
set(ND_TEST_LOCATOR_FETCH_REF "" CACHE STRING "The commit the fetch-route test fetches")
if(ND_TEST_LOCATOR_FETCH)
    if(NOT ND_TEST_LOCATOR_FETCH_REF)
        message(FATAL_ERROR "ND_TEST_LOCATOR_FETCH=ON needs -DND_TEST_LOCATOR_FETCH_REF=<commit>")
    endif()
    nd_add_locator_test(fetch
        "nassau-digital: fetched ${ND_TEST_LOCATOR_FETCH_REF}, [^\n]* at [0-9a-f]+\n.*locator consumer: OK.*configure exit 0"
        "-DARGS=-DND_TEST_GIT_REF=${ND_TEST_LOCATOR_FETCH_REF},-DNASSAU_DIGITAL_SIBLING_ROOT=${ND_LOC_WS}/nowhere/root")
endif()
