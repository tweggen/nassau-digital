#pragma once

// nd_envelope.h - the envelope generator (DESIGN §4.1 amp: ADSR plus hold;
// §4.4 filter: ADSR, the same generator with hold 0).
//
// Segments are LINEAR in amplitude, each lasting exactly its time rounded to
// whole samples: attack rises from the current level to 1, hold stays at 1,
// decay falls from 1 to the sustain level, release falls from wherever it is
// to 0. So a segment's time is the time it takes, measurable on the envelope
// applied to DC (C4.1), at any level. Per sample it is one add: nothing
// transcendental (R5).

#include <cmath>
#include <cstdint>

namespace nd {

class Envelope {
public:
    enum class Stage : int { Idle = 0, Attack, Hold, Decay, Sustain, Release };

    struct Times {
        double attack = 0.0, hold = 0.0, decay = 0.0, sustain = 1.0, release = 0.0;
    };

    void setup(const Times& t, double sampleRate) {
        attackN_ = samples(t.attack, sampleRate);
        holdN_ = samples(t.hold, sampleRate);
        decayN_ = samples(t.decay, sampleRate);
        releaseN_ = samples(t.release, sampleRate);
        sustain_ = t.sustain < 0.0 ? 0.0 : (t.sustain > 1.0 ? 1.0 : t.sustain);
    }

    void noteOn() {
        value_ = 0.0;
        enter(Stage::Attack);
    }

    void noteOff() {
        if (stage_ != Stage::Idle && stage_ != Stage::Release) enter(Stage::Release);
    }

    void kill() {
        stage_ = Stage::Idle;
        value_ = 0.0;
    }

    /// One sample: returns the level for this sample, then advances.
    double next() {
        const double v = value_;
        if (remaining_ > 0) {
            value_ += step_;
            if (--remaining_ == 0) {
                value_ = target_;
                advance();
            }
        }
        return v;
    }

    Stage stage() const { return stage_; }
    bool idle() const { return stage_ == Stage::Idle; }
    double value() const { return value_; }

private:
    static int64_t samples(double seconds, double sr) {
        if (!(seconds > 0.0)) return 0;
        return static_cast<int64_t>(std::llround(seconds * sr));
    }

    void enter(Stage s) {
        stage_ = s;
        switch (s) {
            case Stage::Attack: ramp(1.0, attackN_); break;
            case Stage::Hold: hold(1.0, holdN_); break;
            case Stage::Decay: ramp(sustain_, decayN_); break;
            case Stage::Sustain:
                remaining_ = 0;
                value_ = sustain_;
                break;
            case Stage::Release: ramp(0.0, releaseN_); break;
            case Stage::Idle:
                remaining_ = 0;
                value_ = 0.0;
                break;
        }
    }

    // A ramp to `target` over n samples; n == 0 jumps there and moves on.
    void ramp(double target, int64_t n) {
        target_ = target;
        if (n <= 0) {
            value_ = target;
            remaining_ = 0;
            advance();
            return;
        }
        step_ = (target - value_) / static_cast<double>(n);
        remaining_ = n;
    }

    void hold(double level, int64_t n) {
        value_ = level;
        target_ = level;
        step_ = 0.0;
        remaining_ = n;
        if (n <= 0) advance();
    }

    void advance() {
        switch (stage_) {
            case Stage::Attack: enter(Stage::Hold); break;
            case Stage::Hold: enter(Stage::Decay); break;
            case Stage::Decay: enter(Stage::Sustain); break;
            case Stage::Release: enter(Stage::Idle); break;
            default: break;
        }
    }

    Stage stage_ = Stage::Idle;
    double value_ = 0.0, step_ = 0.0, target_ = 0.0, sustain_ = 1.0;
    int64_t remaining_ = 0;
    int64_t attackN_ = 0, holdN_ = 0, decayN_ = 0, releaseN_ = 0;
};

}  // namespace nd
