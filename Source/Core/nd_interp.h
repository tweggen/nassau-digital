#pragma once

// nd_interp.h - the sample-reading interpolators (DESIGN §3: "one
// interpolator for every pitch (a windowed sinc or a 4-point Hermite: a
// measured choice)").
//
// Both are here because the choice is measured (Tests/interp_tests.cpp, and
// the numbers in docs/GATES.md C2): the instrument uses one of them for every
// layer and every pitch, chosen by Instrument::setInterpolator(), whose
// default is the measured choice.
//
// A reader is handed a function that yields frame i (the taps), so the same
// code serves the fast path (straight pointer arithmetic inside the sample)
// and the slow path near a loop seam or a play boundary. All arithmetic is
// plain multiply-add on precomputed coefficients: nothing transcendental per
// sample (R5). The sinc table is computed once, at prepare time.

#include <cmath>
#include <cstdint>
#include <vector>

namespace nd {

enum class Interpolator : int { Hermite4 = 0, Sinc = 1 };

/// 4-point, 3rd-order Hermite (Catmull-Rom). Taps x[-1], x[0], x[1], x[2]
/// around the read position x[0] + t, t in [0, 1).
inline float hermite4(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

/// A Kaiser-windowed sinc as a polyphase table, linearly interpolated between
/// phases. Taps x[-(kTaps/2 - 1)] .. x[kTaps/2] around x[0] + t.
class SincTable {
public:
    static constexpr int kTaps = 16;
    static constexpr int kHalf = kTaps / 2;      ///< taps right of x[0], inclusive of x[1]..x[kHalf]
    static constexpr int kLeft = kHalf - 1;      ///< taps left of x[0]
    static constexpr int kPhases = 512;

    /// Builds the table: cutoff as a fraction of the sample rate (0.5 is the
    /// Nyquist frequency), Kaiser beta. Allocates; prepare time only.
    void build(double cutoff = 0.45, double beta = 7.0) {
        coeffs_.assign(static_cast<size_t>((kPhases + 1) * kTaps), 0.0f);
        const double i0b = besselI0(beta);
        for (int p = 0; p <= kPhases; ++p) {
            const double t = static_cast<double>(p) / kPhases;
            double c[kTaps];
            double sum = 0.0;
            for (int j = 0; j < kTaps; ++j) {
                const double d = static_cast<double>(j - kLeft) - t;  // tap distance from the read point
                const double x = 2.0 * cutoff * d;
                const double sinc = (x == 0.0) ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double r = d / kHalf;
                const double w = (r * r < 1.0) ? besselI0(beta * std::sqrt(1.0 - r * r)) / i0b : 0.0;
                c[j] = 2.0 * cutoff * sinc * w;
                sum += c[j];
            }
            for (int j = 0; j < kTaps; ++j)  // unity gain at DC for every phase
                coeffs_[static_cast<size_t>(p * kTaps + j)] = static_cast<float>(c[j] / sum);
        }
    }

    bool built() const { return !coeffs_.empty(); }

    /// The kTaps coefficients for fraction t in [0, 1), into out[].
    void coefficients(float t, float* out) const {
        const float tp = t * static_cast<float>(kPhases);
        int p = static_cast<int>(tp);
        if (p >= kPhases) p = kPhases - 1;
        const float f = tp - static_cast<float>(p);
        const float* a = &coeffs_[static_cast<size_t>(p * kTaps)];
        const float* b = a + kTaps;
        for (int j = 0; j < kTaps; ++j) out[j] = a[j] + f * (b[j] - a[j]);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        const double q = x * x / 4.0;
        for (int k = 1; k < 64; ++k) {
            term *= q / (static_cast<double>(k) * k);
            sum += term;
            if (term < sum * 1e-17) break;
        }
        return sum;
    }
    std::vector<float> coeffs_;
};

}  // namespace nd
