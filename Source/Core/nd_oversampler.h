#pragma once

// nd_oversampler.h - the oversampling of a voice's drive -> filter stage and
// of the kit bus (DESIGN §3, "Oversampling").
//
// Linear-phase halfband FIR stages: 2x is one stage, 4x two (the second at
// the 2x rate). A halfband's every second tap is zero and its centre tap is
// 1/2, so one of the two output phases of an upsampler is a plain delay and
// a downsampler needs only the odd taps: K multiplies per stage per channel
// per input (2x) or output sample.
//
// Delay. One stage up and down delays the signal by c = 2K - 1 samples at the
// stage's high rate, twice, which is 2K - 1 samples at its low rate: an
// integer. The second stage of 4x is 2K2 - 1 samples at the 2x rate, half an
// odd number of base samples, so the 4x chain adds one sample of delay at the
// 2x rate to make the whole a whole number of base frames (latency()). An
// integer latency is what lets a voice compensate it exactly (it runs its
// front D frames ahead, nd_voice_fx.h), so an oversampled voice lines up
// with one that is not.
//
// The taps are computed once at prepare (Kaiser-windowed sinc); the per-
// sample work is multiplies and adds only (R5). A Resampler holds only state
// and a pointer to the shared taps, allocates nothing (it lives inside the
// voice pool) and is reset at note-on.

#include <cmath>

namespace nd {

/// One halfband design: the odd taps h[c +- (2i - 1)], i = 1..K, of a
/// 4K - 1 tap halfband whose centre tap is 1/2. K is a compile-time constant
/// so the per-sample loops below unroll and vectorise.
template <int K>
struct HalfbandTaps {
    static constexpr int kK = K;
    double odd[K] = {};  ///< h[c + 2i - 1] for i = 1..K (index i - 1)

    /// Kaiser-windowed sinc, cutoff at a quarter of the high rate, window
    /// `beta`. Host thread (transcendentals).
    void design(double beta) {
        constexpr int c = 2 * K - 1;  // centre index of the 4K - 1 taps
        const double i0b = besselI0(beta);
        for (int i = 1; i <= K; ++i) {
            const int d = 2 * i - 1;  // odd offset from the centre
            const double x = 0.5 * d;
            const double sinc = std::sin(kPi * x) / (kPi * x);
            const double r = static_cast<double>(d) / static_cast<double>(c);
            const double w = besselI0(beta * std::sqrt(std::fmax(0.0, 1.0 - r * r))) / i0b;
            odd[i - 1] = 0.5 * sinc * w;
        }
        // Normalise so the DC gain is exactly 1: 1/2 + 2 * sum(odd) = 1.
        double s = 0.0;
        for (int i = 0; i < K; ++i) s += odd[i];
        const double scale = 0.25 / s;
        for (int i = 0; i < K; ++i) odd[i] *= scale;
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        const double q = x * x / 4.0;
        for (int k = 1; k < 64; ++k) {
            term *= q / (static_cast<double>(k) * static_cast<double>(k));
            sum += term;
            if (term < sum * 1e-17) break;
        }
        return sum;
    }
};

/// A window of the last N values, always contiguous: each value is written
/// twice, N apart, into a buffer of 2N, so buf[pos .. pos + N) is the window
/// oldest first, without ever moving it.
template <int N>
struct Window {
    double buf[2 * N] = {};
    int pos = 0;  // index of the oldest value

    void reset() {
        pos = 0;
        for (double& v : buf) v = 0.0;
    }
    inline void push(double x) {
        buf[pos] = x;
        buf[pos + N] = x;
        if (++pos == N) pos = 0;
    }
    /// w[0] = the oldest of the last N values, w[N - 1] = the newest.
    inline const double* window() const { return buf + pos; }
};

/// One channel of one 2x halfband stage: up (1 in, 2 out, gain 2 on the
/// zero-stuffed signal) and down (2 in, 1 out).
template <int K>
class HalfbandStage {
public:
    void reset() {
        up_.reset();
        downEven_.reset();
        downOdd_.reset();
    }

    /// x[k] in, y[2k] and y[2k + 1] out. The window is x[k - 2K + 1 .. k]:
    ///   y[2k]     = sum_i 2 h_i (x[k - K + 1 - i] + x[k - K + i])
    ///   y[2k + 1] = x[k - K + 1]
    inline void up(const HalfbandTaps<K>& t, double x, double& y0, double& y1) {
        up_.push(x);
        const double* w = up_.window();  // w[0] = x[k - 2K + 1] ... w[2K - 1] = x[k]
        y0 = 2.0 * dot(t, w);
        y1 = w[K];
    }

    /// w[2k] and w[2k + 1] in (the latter the newer), z[k] out:
    ///   z[k] = w[2k - c] / 2 + sum_i h_i (w[2k - 2K + 2 - 2i] + w[2k - 2K + 2i])
    /// with c = 2K - 1, so the odd taps read even-indexed inputs and the
    /// centre an odd-indexed one.
    inline double down(const HalfbandTaps<K>& t, double wEven, double wOdd) {
        downEven_.push(wEven);
        downOdd_.push(wOdd);
        const double* e = downEven_.window();  // e[j] = w[2(k - 2K + 1 + j)]
        const double* o = downOdd_.window();   // o[j] = w[2(k - K + j) + 1]
        // w[2k - 2K + 2 - 2i] = e[K - i], w[2k - 2K + 2i] = e[K - 1 + i];
        // w[2k - 2K + 1] = w[2(k - K) + 1] = o[0].
        return dot(t, e) + 0.5 * o[0];
    }

private:
    static_assert(K % 4 == 0, "four partial sums");
    // sum_i h_i (w[K - i] + w[K - 1 + i]), i = 1..K, in four partial sums
    // (i mod 4) added at the end: without -ffast-math (R7) the compiler may
    // not reorder a floating-point sum itself, and one chain of K dependent
    // adds is what this would cost otherwise. The order is fixed, so the
    // result is the same on every run and every block size (R8).
    static inline double dot(const HalfbandTaps<K>& t, const double* w) {
        double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        for (int i = 1; i <= K; i += 4) {
            a0 += t.odd[i - 1] * (w[K - i] + w[K - 1 + i]);
            a1 += t.odd[i] * (w[K - i - 1] + w[K + i]);
            a2 += t.odd[i + 1] * (w[K - i - 2] + w[K + i + 1]);
            a3 += t.odd[i + 2] * (w[K - i - 3] + w[K + i + 2]);
        }
        return (a0 + a1) + (a2 + a3);
    }

    Window<2 * K> up_, downEven_;
    Window<K + 1> downOdd_;
};

/// The shared taps of the oversampling chains (built once at prepare).
struct OversamplerTaps {
    // Stage 1 (base <-> 2x) passes 0 .. 0.4167 fs (20 kHz at 48 kHz) and
    // stops from 0.5833 fs (28 kHz at 48 kHz): 79 taps, Kaiser beta 10,
    // about -100 dB. Stage 2 (2x <-> 4x) only has to keep stage 1's band (to
    // 0.5833 fs) from its images at 4x, from 1.4167 fs: a far wider
    // transition, 31 taps.
    static constexpr int kK1 = 20;
    static constexpr int kK2 = 8;
    HalfbandTaps<kK1> stage1;
    HalfbandTaps<kK2> stage2;
    void build() {
        stage1.design(10.0);
        stage2.design(10.0);
    }
};

/// One channel of the whole up -> (process at F x) -> down chain.
class Resampler {
public:
    static constexpr int kMaxFactor = 4;

    void reset(const OversamplerTaps* taps, int factor) {
        taps_ = taps;
        factor_ = factor;
        s1_.reset();
        s2_.reset();
        delay2x_ = 0.0;
    }

    int factor() const { return factor_; }

    /// The chain's delay in base-rate frames (an integer, see the top).
    static constexpr int latency(int factor) {
        return factor == 2 ? 2 * OversamplerTaps::kK1 - 1
                           : (factor == 4 ? 2 * OversamplerTaps::kK1 - 1 + OversamplerTaps::kK2 : 0);
    }

    /// One base sample up to `factor` samples (out[0 .. factor)).
    inline void up(double x, double* out) {
        if (factor_ == 2) {
            s1_.up(taps_->stage1, x, out[0], out[1]);
        } else {
            double a, b;
            s1_.up(taps_->stage1, x, a, b);
            // One sample of delay at the 2x rate (see the top): the 4x chain's
            // total delay is then a whole number of base frames.
            const double d = delay2x_;
            delay2x_ = b;
            s2_.up(taps_->stage2, d, out[0], out[1]);
            s2_.up(taps_->stage2, a, out[2], out[3]);
        }
    }

    /// `factor` samples down to one base sample.
    inline double down(const double* in) {
        if (factor_ == 2) return s1_.down(taps_->stage1, in[0], in[1]);
        const double a = s2_.down(taps_->stage2, in[0], in[1]);
        const double b = s2_.down(taps_->stage2, in[2], in[3]);
        return s1_.down(taps_->stage1, a, b);
    }

private:
    const OversamplerTaps* taps_ = nullptr;
    int factor_ = 1;
    HalfbandStage<OversamplerTaps::kK1> s1_;
    HalfbandStage<OversamplerTaps::kK2> s2_;
    double delay2x_ = 0.0;
};

}  // namespace nd
