// scaffold_tests.cpp - C0: the suite exists, links the core and reports.
//
// C0.1 asks for an empty suite that builds and runs on all three CI hosts;
// "empty" here means it proves the harness and the link, and nothing about
// sound yet.

#include "nd_version.h"
#include "test_util.h"

#include <cstring>

using ndtest::check;

int main() {
    std::printf("=== NassauDigital core: scaffold (C0) ===\n");
    ndtest::group("C0.1 scaffold: the harness runs and the core links");
    check("the core reports a version", nd::coreVersion() != nullptr && std::strlen(nd::coreVersion()) > 0);
    check("the program format version is 1 (DESIGN §12)", nd::kProgramFormatVersion == 1);
    return ndtest::summary("scaffold_tests");
}
