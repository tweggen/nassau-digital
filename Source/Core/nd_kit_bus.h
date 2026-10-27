#pragma once

// nd_kit_bus.h - the kit bus's drive, after the sum of an output's voices
// (DESIGN §3: "overlapping hits distort together, the 909 driven into a hot
// desk channel"; §4.5 / §12: the program's `busDrive`). Every output has one
// of its own (DESIGN §3.1: "its own sum and its own kit-bus drive"); the
// instrument runs an aux output's only while something feeds it.
//
//   an output's voices -> sum -> [ kit-bus drive ] -> output level -> out
//
// The bus has one drive and nothing pluggable (DESIGN §1). Its amount is the
// program's busDrive; the program has no curve for it, so the bus uses the
// `soft` curve (nd_drive.h), mix 1. (Which curve, and whether the program
// should name one, is the author's question; see the C5 PR.)
//
// Oversampling (DESIGN §3): the bus oversamples while its drive is above
// zero, at the voices' factor, and the change is ramped over one block.
// busDrive 0 is an exact bypass with no delay, so a program without bus drive
// renders exactly as before this stage existed. When the drive goes above
// zero, the oversampled chain starts from rest and the output crossfades from
// the dry sum to the chain's output; back at zero it crossfades the other way
// and stops. (Priming the chain with the sum's recent history was tried and
// changed nothing measurable: the chain's start lies inside the first fifth
// of the crossfade.) The chain delays by latency()
// frames, which, unlike a voice's, cannot be compensated here (the bus cannot
// see its input ahead of time): while the bus drive is on, the bus is that
// many frames late, and the switch crossfades between two copies of the sum
// that far apart. Over one 32-frame control block that crossfade is audible
// as a click on a low tone (a step 2.3x the tone's own on a 220 Hz sine,
// measured), so the "one block" of the switch is kSwitchFrames = 256 frames,
// a typical host block. The drive's own setting ramps per control block.
//
// Every block is counted on the bus's own grid from reset(), so the bytes do
// not depend on the host's block size (R8). Allocation-free after prepare
// (R3).

#include "nd_drive.h"
#include "nd_oversampler.h"
#include "nd_voice_fx.h"
#include "synth_dsp.h"

namespace nd {

class KitBus {
public:
    static constexpr int kBlock = nassau::kControlBlock;
    static constexpr int kSwitchFrames = 8 * kBlock;  ///< the crossfade when the oversampling switches
    static constexpr DriveCurve kCurve = DriveCurve::Soft;

    void prepare(const VoiceFxShared* shared) {
        sh_ = shared;
        reset();
    }

    void reset() {
        target_ = 0.0;
        engaged_ = false;
        fadeDir_ = 0;
        fadeDone_ = 0;
        pos_ = 0;
        sPrev_ = sNow_ = DriveSetting{};
    }

    /// The amount (the program's busDrive, 0..1), taken at the next block.
    void setDrive(double amount) { target_ = amount > 0.0 ? (amount > 1.0 ? 1.0 : amount) : 0.0; }

    bool engaged() const { return engaged_; }
    /// Frames the bus delays its output by (0 while its drive is off).
    int latency() const { return engaged_ ? Resampler::latency(rs_[0].factor()) : 0; }

    /// n frames in place.
    void process(float* l, float* r, int n) {
        if (!sh_) return;
        for (int s = 0; s < n; ++s) {
            if (pos_ == 0) startBlock();
            const double xl = static_cast<double>(l[s]), xr = static_cast<double>(r[s]);
            if (engaged_) {
                const double f = static_cast<double>(pos_ + 1) / kBlock;
                DriveSetting d = sNow_;
                d.gain = sPrev_.gain + (sNow_.gain - sPrev_.gain) * f;
                d.comp = sPrev_.comp + (sNow_.comp - sPrev_.comp) * f;
                d.wet = sPrev_.wet + (sNow_.wet - sPrev_.wet) * f;
                const double wl = chain(0, d, xl), wr = chain(1, d, xr);
                // The crossfade between the dry sum and the chain while the
                // oversampling switches on (+1) or off (-1).
                double m = 1.0;
                if (fadeDir_ != 0) {
                    const double t = static_cast<double>(++fadeDone_) / kSwitchFrames;
                    m = fadeDir_ > 0 ? t : 1.0 - t;
                }
                l[s] = static_cast<float>(xl + m * (wl - xl));
                r[s] = static_cast<float>(xr + m * (wr - xr));
                if (fadeDir_ != 0 && fadeDone_ == kSwitchFrames) {
                    if (fadeDir_ < 0) engaged_ = false;  // faded out: stop the chain
                    fadeDir_ = 0;
                }
            }
            if (++pos_ == kBlock) pos_ = 0;
        }
    }

private:
    void startBlock() {
        const DriveSetting now = DriveSetting::of(sh_->drive, kCurve, target_, 1.0);
        if (fadeDir_ != 0) {
            sPrev_ = sNow_;  // a switch finishes before the next one starts
            if (fadeDir_ > 0 && target_ > 0.0) sNow_ = now;
        } else if (target_ > 0.0 && !engaged_) {
            engaged_ = true;
            fadeDir_ = 1;
            fadeDone_ = 0;
            sPrev_ = sNow_ = now;
            for (int c = 0; c < 2; ++c) {
                rs_[c].reset(&sh_->taps, sh_->factor);
                dc_[c] = DriveChannel{};
            }
        } else if (target_ <= 0.0 && engaged_) {
            fadeDir_ = -1;
            fadeDone_ = 0;
            sPrev_ = sNow_;  // hold the last setting while fading out
        } else if (engaged_) {
            sPrev_ = sNow_;
            sNow_ = now;
        }
    }

    inline double chain(int c, const DriveSetting& d, double x) {
        double u[Resampler::kMaxFactor];
        rs_[c].up(x, u);
        // The chain's own factor: the instrument's may have changed since the
        // chain started (it takes effect at the next switch-on).
        for (int j = 0; j < rs_[c].factor(); ++j) u[j] = driveShape(d, kCurve, sh_->adaa, dc_[c], u[j]);
        return rs_[c].down(u);
    }

    const VoiceFxShared* sh_ = nullptr;
    double target_ = 0.0;
    bool engaged_ = false;
    int fadeDir_ = 0;   // +1 switching on, -1 switching off, 0 not switching
    int fadeDone_ = 0;  // frames of the switch's crossfade done
    int pos_ = 0;
    DriveSetting sPrev_, sNow_;
    Resampler rs_[2];
    DriveChannel dc_[2];
};

}  // namespace nd
