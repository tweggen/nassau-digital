#pragma once

// test_util.h - the shared hand-rolled test harness (the house pattern of the
// Nassau DSP cores: nassau-analogue Tests/test_util.h). Plain soft checks, no
// framework: every check runs, failures are tallied, and summary() turns the
// tally into main()'s exit code. A soft check never aborts, so a Windows Debug
// build cannot hang a headless run on an assert dialog.
//
// Test-only code: not under Source/Core/, so R2's header restriction does not
// apply here. Determinism (R8): no rand(), no time(); test signals that need
// noise use TestXorshift32 with a fixed seed.

#include <cstdint>
#include <cstdio>
#include <string>

namespace ndtest {

inline int& checks() { static int n = 0; return n; }
inline int& failures() { static int n = 0; return n; }

/// One soft check: prints [PASS]/[FAIL] and the name, tallies the result.
inline bool check(const std::string& name, bool cond) {
    ++checks();
    if (!cond) ++failures();
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name.c_str());
    std::fflush(stdout);
    return cond;
}

/// A group heading, so a failing check can be found in the log.
inline void group(const std::string& name) {
    std::printf("\nGroup: %s\n", name.c_str());
}

/// A measured value, printed beside the checks (R6: numbers, not adjectives).
inline void note(const std::string& text) { std::printf("    %s\n", text.c_str()); }

/// The exit code for main(): 0 when every check passed.
inline int summary(const char* suite) {
    std::printf("\n=== %s: %d/%d checks passed", suite, checks() - failures(), checks());
    if (failures() > 0) {
        std::printf(", %d FAILED ===\n", failures());
        return 1;
    }
    std::printf(" ===\n");
    return 0;
}

/// Deterministic PRNG for test signals (R8).
struct TestXorshift32 {
    uint32_t s;
    explicit TestXorshift32(uint32_t seed) : s(seed ? seed : 0x1234567u) {}
    uint32_t next() {
        uint32_t x = s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = x;
        return x;
    }
    /// Uniform in [-1, 1).
    float bipolar() { return static_cast<float>(next() >> 8) / 8388608.0f - 1.0f; }
};

}  // namespace ndtest
