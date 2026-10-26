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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

// ---- Test signals and measurements (R6: measured on real audio) ------------

constexpr double kPi = 3.14159265358979323846;

/// A sine of `freq` Hz at `rate`, `seconds` long, amplitude `amp`.
inline std::vector<float> sine(double freq, double rate, double seconds, double amp = 0.5) {
    const size_t n = static_cast<size_t>(std::llround(seconds * rate));
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return v;
}

/// Deterministic white noise in [-amp, amp).
inline std::vector<float> noise(size_t n, uint32_t seed, double amp = 0.5) {
    TestXorshift32 r(seed);
    std::vector<float> v(n);
    for (auto& x : v) x = static_cast<float>(amp) * r.bipolar();
    return v;
}

/// RMS of v[from, to).
inline double rms(const std::vector<float>& v, size_t from, size_t to) {
    to = std::min(to, v.size());
    if (to <= from) return 0.0;
    double acc = 0.0;
    for (size_t i = from; i < to; ++i) acc += static_cast<double>(v[i]) * v[i];
    return std::sqrt(acc / static_cast<double>(to - from));
}

inline double db(double gain) { return 20.0 * std::log10(std::max(gain, 1e-300)); }

/// Largest absolute value in v[from, to).
inline double peak(const std::vector<float>& v, size_t from, size_t to) {
    double m = 0.0;
    for (size_t i = from; i < std::min(to, v.size()); ++i) m = std::max(m, std::fabs(static_cast<double>(v[i])));
    return m;
}

/// Period in samples of v[from, to) by autocorrelation: the normalised
/// autocorrelation's highest peak for a lag in [minLag, maxLag], refined by
/// a parabola through it and its neighbours.
inline double autocorrPeriod(const std::vector<float>& v, size_t from, size_t to, int minLag, int maxLag) {
    to = std::min(to, v.size());
    const size_t n = to - from;
    auto corr = [&](int lag) {
        double acc = 0.0, e0 = 0.0, e1 = 0.0;
        for (size_t i = from; i + static_cast<size_t>(lag) < from + n; ++i) {
            const double a = v[i], b = v[i + static_cast<size_t>(lag)];
            acc += a * b;
            e0 += a * a;
            e1 += b * b;
        }
        return acc / std::sqrt(std::max(e0 * e1, 1e-300));
    };
    int best = minLag;
    double bestC = -2.0;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        const double c = corr(lag);
        if (c > bestC) {
            bestC = c;
            best = lag;
        }
    }
    const double cm = corr(best - 1), c0 = bestC, cp = corr(best + 1);
    const double den = cm - 2.0 * c0 + cp;
    const double off = (den != 0.0) ? 0.5 * (cm - cp) / den : 0.0;
    return best + std::max(-0.5, std::min(0.5, off));
}

/// Fundamental frequency by rising zero crossings (linearly interpolated),
/// first to last crossing over the whole number of periods between them.
inline double zeroCrossingF0(const std::vector<float>& v, size_t from, size_t to, double rate) {
    to = std::min(to, v.size());
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = from + 1; i < to; ++i) {
        const double y0 = v[i - 1], y1 = v[i];
        if (y0 < 0.0 && y1 >= 0.0) {
            const double t = static_cast<double>(i - 1) + (-y0) / (y1 - y0);
            if (first < 0.0) first = t;
            last = t;
            ++count;
        }
    }
    if (count < 2) return 0.0;
    return (count - 1) * rate / (last - first);
}

}  // namespace ndtest
