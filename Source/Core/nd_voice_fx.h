#pragma once

// nd_voice_fx.h - the slot for a voice's drive -> filter stage (DESIGN §3).
//
//   layers -> sum -> [ Drive -> Filter ] -> Amp -> Pan
//
// The bracketed stage is gate C5's: its filters and shaper come from
// nassau-dsp (gate X1), so it is not built here. What is built is the slot:
// every voice owns a VoiceFx, starts it at note-on with the zone's settings
// and calls it on each control block's stereo sum, before the amp, with the
// block's modulated parameters. Today it passes the block through unchanged.
//
// What C5 fills in, and where (DESIGN §3):
//   * start():   the oversampling decision, made once per voice from the
//                zone's drive at note-on ("the decision holds for the life of
//                the voice"), and the filter's reset;
//   * process(): drive (curve, amount, mix, output compensation), then the
//                filter (type, cutoffHz, resonance), on l/r in place.
// Nothing here may allocate: a VoiceFx lives inside the voice pool.

#include "nd_program.h"

namespace nd {

/// One control block's settings for the stage, modulation applied.
struct VoiceFxParams {
    FilterType filter = FilterType::Lp24;
    double cutoffHz = 20000.0;  ///< after key tracking, velocity, filter envelope, LFO and mod wheel
    double resonance = 0.0;
    DriveCurve driveCurve = DriveCurve::Soft;
    double drive = 0.0;
    double driveMix = 1.0;
};

class VoiceFx {
public:
    /// Note-on. C5: decide oversampling from params.drive, reset the filter.
    void start(const VoiceFxParams& params, double sampleRate) {
        (void)params;
        (void)sampleRate;
    }
    /// One control block (n <= Instrument::kControlBlock), in place.
    /// C5: drive, then filter. Until then: unchanged.
    void process(const VoiceFxParams& params, float* left, float* right, int n) {
        (void)params;
        (void)left;
        (void)right;
        (void)n;
    }
};

}  // namespace nd
