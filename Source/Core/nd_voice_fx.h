#pragma once

// nd_voice_fx.h - a voice's drive -> filter stage (DESIGN §3, §4.2, §4.3).
//
//   layers -> sum -> [ Drive -> Filter ] -> Amp -> Pan
//
// The filters are nassau-dsp's own (synth_filter.h, gate X1), driven exactly
// as nassau-analogue's SynthCore drives them, so a voice's filter is the same
// code with the same coefficient schedule as nassau-analogue's (C5.2):
//
//   * once per control block (nassau::kControlBlock = 32 output frames, the
//     voice's own grid from note-on): setControlRate(cutoff, rate, resonance),
//     which computes g = tan(pi fc / fs) and everything derived from it;
//   * per sample: g ramps linearly from the previous block's g to the new one
//     and advanceCoeff(g) re-derives the rest (adds, multiplies and a
//     division: no transcendental, R5). On a voice's first block the ramp
//     starts at the target. A block whose g did not move skips advanceCoeff,
//     which would only recompute the same values (bit-identical: the same
//     expressions on the same g).
//
// Filter types (DESIGN §3, v1):
//   lp24  LadderFilter   24 dB/oct ZDF ladder, resonance saturating in its loop
//   lp12  SvfFilter      12 dB/oct TPT state-variable, low-pass output
//   hp12  HpfCascade     2 one-pole high-passes (no resonance)
//   hp24  HpfCascade     4 one-pole high-passes (no resonance)
//   off   none           a true bypass, and the default (DESIGN §3, decided
//                        2026-10-27): no filter code runs and its state is
//                        never touched. The stage hands on what the drive
//                        made (through the oversampler's round trip if the
//                        voice is oversampled); at 1x with no drive it
//                        returns at once and the layers' sum passes bit for
//                        bit.
// resonance 0..1 is nassau-dsp's 0..100 %. (C5 had no "off" and made lp24 at
// 20 kHz the default, which coloured every voice.)
//
// Oversampling (DESIGN §3). start() decides it from the zone's drive at
// note-on: above zero, the drive and the filter run at `factor` times the
// rate between a halfband upsampler and downsampler (nd_oversampler.h), and
// the decision holds for the life of the voice, so automating drive through
// zero never switches filter state inside a note. The chain delays the signal
// by latency() whole frames; the voice runs everything before this stage
// that many frames ahead (nd_instrument.cpp), so the delay never reaches the
// output and an oversampled voice lines up with one that is not.
//
// Drive (nd_drive.h) runs before the filter, at the same (over)sampled rate.
// Its gain, compensation and wet share ramp linearly across each control
// block, so automating drive does not zipper. Drive 0 is an exact bypass.
//
// Nothing here allocates: a VoiceFx lives inside the voice pool. Everything
// it reads at run time (the drive tables, the oversampler's taps) is built at
// prepare and only pointed to.

#include "nd_drive.h"
#include "nd_oversampler.h"
#include "nd_program.h"
#include "synth_dsp.h"
#include "synth_filter.h"

namespace nd {

/// One control block's settings for the stage, modulation applied.
struct VoiceFxParams {
    FilterType filter = FilterType::Off;
    double cutoffHz = 20000.0;  ///< after key tracking, velocity, filter envelope, LFO and mod wheel
    double resonance = 0.0;
    DriveCurve driveCurve = DriveCurve::Soft;
    double drive = 0.0;
    double driveMix = 1.0;
};

/// What every voice's stage shares, built at prepare (host thread).
struct VoiceFxShared {
    double sampleRate = 48000.0;
    int factor = 4;  ///< the oversampling factor of a voice with drive > 0 (2 or 4)
    bool adaa = true;  ///< antiderivative antialiasing where a curve allows it (nd_drive.h)
    DriveTables drive;
    OversamplerTaps taps;

    void build(double rate) {
        sampleRate = rate;
        drive.build();
        taps.build();
    }
    int latency() const { return Resampler::latency(factor); }
};

class VoiceFx {
public:
    static constexpr int kBlock = nassau::kControlBlock;

    /// Note-on: decide oversampling from params.drive (held for the voice's
    /// life), reset the filters and the chain.
    void start(const VoiceFxParams& params, const VoiceFxShared& shared) {
        sh_ = &shared;
        factor_ = (params.drive > 0.0) ? shared.factor : 1;
        fs_ = shared.sampleRate * factor_;
        type_ = params.filter;
        curve_ = params.driveCurve;
        for (int c = 0; c < 2; ++c) {
            lad_[c].reset();
            svf_[c].reset();
            hpf_[c].reset();
            if (factor_ > 1) rs_[c].reset(&shared.taps, factor_);
            dc_[c] = DriveChannel{};
        }
        first_ = true;
        drivePrev_ = DriveSetting{};
        driveNow_ = DriveSetting{};
        pos_ = 0;
    }

    bool oversampled() const { return factor_ > 1; }
    int factor() const { return factor_; }
    /// Frames this stage delays its input by (0 without oversampling).
    int latency() const { return Resampler::latency(factor_); }

    /// Once per control block, before that block's samples: the filter's
    /// coefficients (ramped across the block) and the drive's setting.
    void control(const VoiceFxParams& p) {
        double gStart = 0.0, gTarget = 0.0;
        const double res = p.resonance * 100.0;
        switch (type_) {
            case FilterType::Lp24:
                gStart = static_cast<double>(lad_[0].g);
                lad_[0].setControlRate(p.cutoffHz, fs_, res);
                lad_[1].setControlRate(p.cutoffHz, fs_, res);
                gTarget = static_cast<double>(lad_[0].g);
                break;
            case FilterType::Lp12:
                gStart = static_cast<double>(svf_[0].g);
                svf_[0].setControlRate(p.cutoffHz, fs_, res);
                svf_[1].setControlRate(p.cutoffHz, fs_, res);
                gTarget = static_cast<double>(svf_[0].g);
                break;
            case FilterType::Hp12:
            case FilterType::Hp24:
                gStart = static_cast<double>(hpf_[0].g);
                hpf_[0].setControlRate(p.cutoffHz, fs_);
                hpf_[1].setControlRate(p.cutoffHz, fs_);
                gTarget = static_cast<double>(hpf_[0].g);
                break;
            case FilterType::Off:
                break;  // no coefficients: nothing reads them
        }
        if (first_) gStart = gTarget;
        gStart_ = gStart;
        gTarget_ = gTarget;
        ramp_ = (gStart != gTarget);
        const DriveSetting now = DriveSetting::of(sh_->drive, curve_, p.drive, p.driveMix);
        drivePrev_ = first_ ? now : driveNow_;
        driveNow_ = now;
        first_ = false;
        pos_ = 0;
    }

    /// n frames (n <= what is left of the current control block), in place.
    /// Each step (up, drive, filter, down) runs over the whole chunk, so
    /// every decision (filter type, curve, factor) is taken once per chunk,
    /// not per sample.
    void process(float* left, float* right, int n) {
        const int m = n * factor_;  // samples at the stage's rate
        // Filter off, no oversampling, no drive: the stage has nothing to do
        // and leaves the buffers alone (the round trip through double below
        // would be exact too, but costs time).
        if (type_ == FilterType::Off && factor_ == 1 && drivePrev_.wet <= 0.0 && driveNow_.wet <= 0.0) {
            pos_ += m;
            return;
        }
        double bl[kBlock * Resampler::kMaxFactor], br[kBlock * Resampler::kMaxFactor];
        if (factor_ == 1) {
            for (int s = 0; s < n; ++s) {
                bl[s] = static_cast<double>(left[s]);
                br[s] = static_cast<double>(right[s]);
            }
        } else {
            for (int s = 0; s < n; ++s) {
                rs_[0].up(static_cast<double>(left[s]), bl + s * factor_);
                rs_[1].up(static_cast<double>(right[s]), br + s * factor_);
            }
        }
        if (drivePrev_.wet > 0.0 || driveNow_.wet > 0.0) drive(bl, br, n);
        switch (type_) {
            case FilterType::Lp24:
                filterLoop(lad_, bl, br, m, [](LadderFilter& f, nassau_real x) { return f.process(x); });
                break;
            case FilterType::Lp12:
                filterLoop(svf_, bl, br, m, [](SvfFilter& f, nassau_real x) { return f.process(x); });
                break;
            case FilterType::Hp12:
                filterLoop(hpf_, bl, br, m, [](HpfCascade& f, nassau_real x) { return f.process(x, 2); });
                break;
            case FilterType::Hp24:
                filterLoop(hpf_, bl, br, m, [](HpfCascade& f, nassau_real x) { return f.process(x, 4); });
                break;
            case FilterType::Off:
                break;  // a true bypass: the drive's output (or the input) as it is
        }
        if (factor_ == 1) {
            for (int s = 0; s < n; ++s) {
                left[s] = static_cast<float>(bl[s]);
                right[s] = static_cast<float>(br[s]);
            }
        } else {
            for (int s = 0; s < n; ++s) {
                left[s] = static_cast<float>(rs_[0].down(bl + s * factor_));
                right[s] = static_cast<float>(rs_[1].down(br + s * factor_));
            }
        }
        pos_ += m;
    }

private:
    // The drive over n frames (n * factor_ samples), its setting ramped
    // across the control block frame by frame.
    void drive(double* bl, double* br, int n) {
        switch (curve_) {
            case DriveCurve::Soft: driveLoop<DriveCurve::Soft>(bl, br, n); break;
            case DriveCurve::Tube: driveLoop<DriveCurve::Tube>(bl, br, n); break;
            case DriveCurve::Hard: driveLoop<DriveCurve::Hard>(bl, br, n); break;
            case DriveCurve::Fold: driveLoop<DriveCurve::Fold>(bl, br, n); break;
        }
    }

    template <DriveCurve C>
    void driveLoop(double* bl, double* br, int n) {
        const bool adaa = sh_->adaa;
        const bool ramp = drivePrev_.wet != driveNow_.wet || drivePrev_.gain != driveNow_.gain ||
                          drivePrev_.comp != driveNow_.comp;
        const int frame0 = pos_ / factor_;  // frames of this control block already done
        DriveSetting d = driveNow_;
        for (int s = 0; s < n; ++s) {
            if (ramp) {
                const double f = static_cast<double>(frame0 + s + 1) / kBlock;
                d.gain = drivePrev_.gain + (driveNow_.gain - drivePrev_.gain) * f;
                d.comp = drivePrev_.comp + (driveNow_.comp - drivePrev_.comp) * f;
                d.wet = drivePrev_.wet + (driveNow_.wet - drivePrev_.wet) * f;
            }
            for (int j = s * factor_; j < (s + 1) * factor_; ++j) {
                bl[j] = driveShape(d, C, adaa, dc_[0], bl[j]);
                br[j] = driveShape(d, C, adaa, dc_[1], br[j]);
            }
        }
    }

    // The filter over m samples of both channels, with the per-sample g ramp
    // exactly as nassau-analogue's SynthCore (and nassau-dsp's golden
    // battery) does it: g = gStart + (gTarget - gStart) * (i + 1) / blockLen
    // at the block's i-th sample, applied with advanceCoeff before process.
    template <typename F, typename P>
    void filterLoop(F (&f)[2], double* bl, double* br, int m, P proc) {
        const double blockLen = static_cast<double>(kBlock * factor_);
        if (ramp_) {
            for (int i = 0; i < m; ++i) {
                const double gNow = gStart_ + (gTarget_ - gStart_) * static_cast<double>(pos_ + i + 1) / blockLen;
                const nassau_real g = static_cast<nassau_real>(gNow);
                f[0].advanceCoeff(g);
                f[1].advanceCoeff(g);
                bl[i] = static_cast<double>(proc(f[0], static_cast<nassau_real>(bl[i])));
                br[i] = static_cast<double>(proc(f[1], static_cast<nassau_real>(br[i])));
            }
        } else {
            for (int i = 0; i < m; ++i) {
                bl[i] = static_cast<double>(proc(f[0], static_cast<nassau_real>(bl[i])));
                br[i] = static_cast<double>(proc(f[1], static_cast<nassau_real>(br[i])));
            }
        }
    }

    const VoiceFxShared* sh_ = nullptr;
    int factor_ = 1;
    double fs_ = 48000.0;
    FilterType type_ = FilterType::Off;
    DriveCurve curve_ = DriveCurve::Soft;
    LadderFilter lad_[2];
    SvfFilter svf_[2];
    HpfCascade hpf_[2];
    Resampler rs_[2];
    DriveChannel dc_[2];
    double gStart_ = 0.0, gTarget_ = 0.0;
    bool ramp_ = false, first_ = true;
    int pos_ = 0;  // samples (at the stage's rate) into the current control block
    DriveSetting drivePrev_, driveNow_;
};

}  // namespace nd
