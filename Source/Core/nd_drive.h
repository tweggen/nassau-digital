#pragma once

// nd_drive.h - the drive waveshaper (DESIGN §3, §4.3): input gain, a curve,
// output compensation and a dry/wet mix. Used per voice (before the filter)
// and on the kit bus (after the sum of the voices).
//
// The curves (DESIGN §3 names them: soft, tube, hard, fold), each with unity
// slope at 0 and bounded by 1:
//
//   soft   x / sqrt(1 + x^2)                  smooth everywhere
//   tube   x / (1 + |x|)                      nassau-dsp's shapeTriodeK, knee 1
//   hard   clamp(x, -1, 1)
//   fold   a triangle fold: x inside [-1, 1], reflected back at +-1
//
// The amount a (the zone's `drive`, 0..1) sets
//
//   input gain   G(a) = 10^(kMaxGainDb * a / 20)        0 .. +20 dB
//   wet share    w(a) = mix * min(1, a / kFadeIn)       so a = 0 is exactly dry
//   compensation C(a) = RMS(sine) / RMS(curve(G sine))  for a 1 kHz sine at
//                       kRefDb, counting its harmonics up to 20 kHz (what
//                       the voice's band keeps; the oversampler removes
//                       the rest, and counting it would leave a loud,
//                       bright curve such as fold too quiet)
//
// and the stage computes  y = x + w * (C * curve(G x) - x). The compensation
// makes a sine at the reference level ("moderate input", C5.1) come out at the
// level it went in at, whatever the amount: turning drive up adds grit, not
// level (DESIGN §3). C(a) is tabulated per curve at prepare (DriveTables) and
// interpolated per control block; per sample the stage is multiplies, adds, a
// compare or two, and for `soft` one square root (an IEEE basic operation,
// not a transcendental, R5).
//
// a = 0 is an exact bypass (w = 0): the voice's filter then sees its input
// bit for bit, which is what C5.2 compares against nassau-dsp's golden.
//
// Antiderivative antialiasing (first order). Oversampling alone leaves the
// harmonics of a hard edge (hard, fold) loud enough above the oversampled
// Nyquist to fold back into the audio band (docs/GATES.md C5.3 measures it),
// so where a curve has an antiderivative F in closed form without a
// transcendental (soft, hard, fold), the stage outputs the curve's mean over
// the step instead of its value: (F(u1) - F(u0)) / (u1 - u0), or f at the
// midpoint when the step is too small for that quotient to be accurate. It
// costs a division per sample and acts as a two-sample average on a linear
// stretch (a half-sample delay at the oversampled rate: a fraction of a frame
// that the voice's latency compensation does not count). `tube` has none
// without a logarithm (F = |u| - ln(1 + |u|)) and runs plain.
//
// The constants (20 dB, the -12 dBFS reference, the 0.05 fade-in, the curve
// formulas) are this gate's proposal; DESIGN §3 names the curves and asks for
// input gain and output compensation, not these values.

#include <cmath>
#include <vector>

#include "nd_program.h"
#include "synth_dsp.h"

namespace nd {

struct DriveConst {
    /// Input gain at drive 1. Measured (docs/GATES.md C5.3): +24 dB leaves
    /// the 10 kHz test tone's aliases at -77.7 dB (tube) and -79.4 dB (fold)
    /// above 20 kHz at 44.1 kHz, 4x; +20 dB keeps every curve below -84 dB.
    static constexpr double kMaxGainDb = 20.0;
    static constexpr double kRefDb = -12.0;     ///< "moderate input": a sine at this RMS (dBFS)
    static constexpr double kFadeIn = 0.05;     ///< drive below this crossfades from dry
    static constexpr int kSteps = 64;           ///< compensation table resolution over 0..1
    static constexpr int kNumCurves = 4;
};

/// One curve, one sample. f(0) = 0, f'(0) = 1, |f| <= 1.
inline double driveCurve(DriveCurve c, double x) {
    switch (c) {
        case DriveCurve::Soft: return x / std::sqrt(1.0 + x * x);
        case DriveCurve::Tube: return shapeTriodeK(x, 1.0);
        case DriveCurve::Hard: return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x);
        case DriveCurve::Fold: {
            // Period 4: t = (x + 1) / 4 in [0, 1), f = 1 - 4 |t - 1/2|.
            double t = (x + 1.0) * 0.25;
            t -= std::floor(t);
            const double d = t - 0.5;
            return 1.0 - 4.0 * (d < 0.0 ? -d : d);
        }
    }
    return x;
}

/// The curve's antiderivative, F(0) = 0, where it is algebraic (see the top).
inline double driveAntiderivative(DriveCurve c, double u) {
    switch (c) {
        case DriveCurve::Soft: return std::sqrt(1.0 + u * u) - 1.0;
        case DriveCurve::Hard: {
            const double a = u < 0.0 ? -u : u;
            return a <= 1.0 ? 0.5 * u * u : a - 0.5;
        }
        case DriveCurve::Fold: {
            // s = u folded into the base period [-1, 3): F = s^2/2 on [-1, 1],
            // 2s - s^2/2 - 1 on [1, 3]; F is periodic (the fold has zero mean).
            const double s = u - 4.0 * std::floor((u + 1.0) * 0.25);
            return s <= 1.0 ? 0.5 * s * s : 2.0 * s - 0.5 * s * s - 1.0;
        }
        case DriveCurve::Tube: break;
    }
    return 0.0;
}

inline bool driveHasAntiderivative(DriveCurve c) { return c != DriveCurve::Tube; }

/// One channel's memory for the antiderivative method.
struct DriveChannel {
    double u = 0.0;  ///< the previous sample's curve input
    double F = 0.0;  ///< its antiderivative
};

inline double driveGain(double amount) {
    return std::pow(10.0, DriveConst::kMaxGainDb * amount / 20.0);
}

/// The compensation tables, one per curve over the amount (prepare time).
struct DriveTables {
    double comp[DriveConst::kNumCurves][DriveConst::kSteps + 1] = {};

    void build() {
        // RMS of a reference sine through each curve at each amount, from its
        // harmonics 1..kHarmonics (a DFT of one period sampled densely), so
        // the level is the level of what the band keeps (see the top).
        constexpr int kN = 4096;
        constexpr int kHarmonics = 20;  // a 1 kHz reference: up to 20 kHz
        constexpr double kPi = 3.14159265358979323846;
        const double amp = std::pow(10.0, DriveConst::kRefDb / 20.0) * std::sqrt(2.0);
        const double rmsIn = amp / std::sqrt(2.0);
        static_assert(kN % 4 == 0, "a quarter period is a whole number of samples (cos from sin)");
        std::vector<double> sinT(kN), y(kN);  // prepare time: allocation is fine here
        for (int i = 0; i < kN; ++i) sinT[static_cast<size_t>(i)] = std::sin(2.0 * kPi * i / kN);
        for (int c = 0; c < DriveConst::kNumCurves; ++c) {
            for (int s = 0; s <= DriveConst::kSteps; ++s) {
                const double g = driveGain(static_cast<double>(s) / DriveConst::kSteps);
                for (size_t i = 0; i < y.size(); ++i) y[i] = driveCurve(static_cast<DriveCurve>(c), g * amp * sinT[i]);
                double power = 0.0;
                for (int h = 1; h <= kHarmonics; ++h) {
                    double re = 0.0, im = 0.0;
                    for (int i = 0; i < kN; ++i) {
                        const int k = (h * i) % kN;
                        const double yi = y[static_cast<size_t>(i)];
                        re += yi * sinT[static_cast<size_t>((k + kN / 4) % kN)];  // cos
                        im += yi * sinT[static_cast<size_t>(k)];
                    }
                    re *= 2.0 / kN;
                    im *= 2.0 / kN;
                    power += 0.5 * (re * re + im * im);
                }
                const double rmsOut = std::sqrt(power);
                comp[c][s] = rmsOut > 0.0 ? rmsIn / rmsOut : 1.0;
            }
        }
    }

    /// C(a), linearly interpolated (control rate).
    double compensation(DriveCurve curve, double amount) const {
        const double a = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
        const double p = a * DriveConst::kSteps;
        int i = static_cast<int>(p);
        if (i >= DriveConst::kSteps) i = DriveConst::kSteps - 1;
        const double f = p - i;
        const double* t = comp[static_cast<int>(curve)];
        return t[i] + (t[i + 1] - t[i]) * f;
    }
};

/// What one control block's drive does: gain, compensation, wet share.
struct DriveSetting {
    double gain = 1.0;
    double comp = 1.0;
    double wet = 0.0;  ///< 0 = exact bypass

    static DriveSetting of(const DriveTables& t, DriveCurve curve, double amount, double mix) {
        DriveSetting s;
        const double a = amount < 0.0 ? 0.0 : (amount > 1.0 ? 1.0 : amount);
        const double m = mix < 0.0 ? 0.0 : (mix > 1.0 ? 1.0 : mix);
        if (a <= 0.0 || m <= 0.0) return s;
        s.gain = driveGain(a);
        s.comp = t.compensation(curve, a);
        s.wet = m * (a < DriveConst::kFadeIn ? a / DriveConst::kFadeIn : 1.0);
        return s;
    }
};

/// One sample through the stage: y = x + w (C f(G x) - x), with f the curve's
/// mean over the step when `adaa` and the curve has an antiderivative.
inline double driveShape(const DriveSetting& d, DriveCurve c, bool adaa, DriveChannel& ch, double x) {
    const double u = d.gain * x;
    double f;
    if (adaa && driveHasAntiderivative(c)) {
        const double F = driveAntiderivative(c, u);
        const double du = u - ch.u;
        // Below 1e-5 the quotient loses more than about 1e-10 to cancellation.
        f = (du > 1e-5 || du < -1e-5) ? (F - ch.F) / du : driveCurve(c, 0.5 * (u + ch.u));
        ch.u = u;
        ch.F = F;
    } else {
        f = driveCurve(c, u);
    }
    return x + d.wet * (d.comp * f - x);
}

}  // namespace nd
