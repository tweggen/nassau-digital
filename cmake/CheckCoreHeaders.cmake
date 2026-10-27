# CheckCoreHeaders.cmake - the R2 header check (docs/GATES.md C0.2).
#
#     cmake -DCORE_DIR=<dir> -P cmake/CheckCoreHeaders.cmake
#
# Fails (non-zero exit, FATAL_ERROR) when any source under CORE_DIR includes
# something outside R2's list. R2 (docs/GATES.md): the core stays
# framework-free, standard headers only. So an #include is allowed when it is
#
#   * <name>   and name is a C++17 standard library header (the list below), or
#   * "name"   and name resolves to an existing file INSIDE CORE_DIR, relative
#              to the including file's directory or to CORE_DIR itself, or
#   * "name"   and name is a header directly in DSP_INCLUDE_DIR, when that is
#              passed: nassau-dsp's Source/DSP, the one further root R2 allows
#              (DESIGN §11, gate C5). nassau-dsp checks its own headers against
#              the same rule (its tools/check_headers.py).
#
# Everything else fails: a third-party <QString>, a C header such as
# <math.h> (use <cmath>), a "../Plugin/x.h" that climbs out of the core, a
# quoted name that resolves nowhere, and an `#include MACRO` (which no check
# can see through).
#
# Written in CMake rather than a script language so it needs nothing a CMake
# build does not already have, on Linux, macOS and Windows alike.

if(NOT CORE_DIR)
    message(FATAL_ERROR "CheckCoreHeaders: pass -DCORE_DIR=<dir>")
endif()
get_filename_component(CORE_DIR "${CORE_DIR}" ABSOLUTE)
if(DSP_INCLUDE_DIR)
    get_filename_component(DSP_INCLUDE_DIR "${DSP_INCLUDE_DIR}" ABSOLUTE)
    if(NOT EXISTS "${DSP_INCLUDE_DIR}/synth_dsp.h")
        message(FATAL_ERROR "CheckCoreHeaders: DSP_INCLUDE_DIR=${DSP_INCLUDE_DIR} holds no nassau-dsp headers")
    endif()
endif()
if(NOT IS_DIRECTORY "${CORE_DIR}")
    message(FATAL_ERROR "CheckCoreHeaders: ${CORE_DIR} is not a directory")
endif()

set(STD_HEADERS
    algorithm any array atomic bitset cassert cctype cerrno cfenv cfloat
    charconv chrono cinttypes climits clocale cmath codecvt complex
    condition_variable csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib
    cstring ctime cuchar cwchar cwctype deque exception execution filesystem
    forward_list fstream functional future initializer_list iomanip ios iosfwd
    iostream istream iterator limits list locale map memory memory_resource
    mutex new numeric optional ostream queue random ratio regex
    scoped_allocator set shared_mutex sstream stack stdexcept streambuf string
    string_view system_error thread tuple type_traits typeindex typeinfo
    unordered_map unordered_set utility valarray variant vector)

file(GLOB_RECURSE SOURCES
    "${CORE_DIR}/*.h" "${CORE_DIR}/*.hpp" "${CORE_DIR}/*.cpp" "${CORE_DIR}/*.cc")
list(SORT SOURCES)

set(VIOLATIONS "")
set(NUM_INCLUDES 0)
foreach(SRC IN LISTS SOURCES)
    file(STRINGS "${SRC}" LINES REGEX "^[ \t]*#[ \t]*include")
    get_filename_component(SRC_DIR "${SRC}" DIRECTORY)
    file(RELATIVE_PATH SRC_REL "${CORE_DIR}" "${SRC}")
    foreach(LINE IN LISTS LINES)
        math(EXPR NUM_INCLUDES "${NUM_INCLUDES} + 1")
        if(LINE MATCHES "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>")
            set(NAME "${CMAKE_MATCH_1}")
            list(FIND STD_HEADERS "${NAME}" IDX)
            if(IDX EQUAL -1)
                list(APPEND VIOLATIONS "${SRC_REL}: <${NAME}> is not a C++17 standard header")
            endif()
        elseif(LINE MATCHES "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\"")
            set(NAME "${CMAKE_MATCH_1}")
            set(FOUND "")
            foreach(BASE IN ITEMS "${SRC_DIR}" "${CORE_DIR}")
                get_filename_component(CAND "${BASE}/${NAME}" ABSOLUTE)
                if(NOT FOUND AND EXISTS "${CAND}" AND NOT IS_DIRECTORY "${CAND}")
                    file(RELATIVE_PATH REL "${CORE_DIR}" "${CAND}")
                    if(NOT REL MATCHES "^\\.\\.")
                        set(FOUND "${CAND}")
                    endif()
                endif()
            endforeach()
            if(NOT FOUND AND DSP_INCLUDE_DIR AND NOT NAME MATCHES "[/\\\\]"
               AND EXISTS "${DSP_INCLUDE_DIR}/${NAME}" AND NOT IS_DIRECTORY "${DSP_INCLUDE_DIR}/${NAME}")
                set(FOUND "${DSP_INCLUDE_DIR}/${NAME}")
            endif()
            if(NOT FOUND)
                list(APPEND VIOLATIONS "${SRC_REL}: \"${NAME}\" is not a file of the core")
            endif()
        else()
            string(STRIP "${LINE}" LINE)
            list(APPEND VIOLATIONS "${SRC_REL}: unrecognised include `${LINE}`")
        endif()
    endforeach()
endforeach()

list(LENGTH SOURCES NUM_SOURCES)
if(VIOLATIONS)
    list(LENGTH VIOLATIONS NUM_VIOLATIONS)
    string(REPLACE ";" "\n  " TEXT "${VIOLATIONS}")
    message(FATAL_ERROR
        "R2 header check FAILED: ${NUM_VIOLATIONS} include(s) outside R2's list "
        "under ${CORE_DIR}:\n  ${TEXT}\n"
        "The core takes standard C++ headers and its own files only (docs/GATES.md R2).")
endif()
message(STATUS "R2 header check: ${NUM_INCLUDES} include(s) in ${NUM_SOURCES} file(s) under ${CORE_DIR}, all allowed")
