#pragma once

// nd_instrument.h - the sampler instrument: voices, events, process()
// (DESIGN §2, §3, §4, §5, §6.1, §9.1).
//
// Threading (DESIGN §9.1). prepare(), setProgram(), setSample() and
// collectGarbage() allocate and run on the host's thread; process() runs on
// the audio thread and neither allocates, locks nor does I/O (R3). The host
// must not run a setter concurrently with process(): in qbx the snapshots are
// swapped under the slot processor's mutex, which process() runs under too,
// so process() itself takes no lock.
//
// Snapshots. setProgram() compiles the program into a flat, fixed-size form
// the voices copy at note-on, so a voice keeps the zone and layers it started
// with for its whole life, whatever program arrives later. setSample() installs
// a buffer for a layer id; a voice reads the buffer current at its note-on and
// keeps it to its end (DESIGN §6.1, rule 4). A replaced buffer is never freed
// on the audio thread: it is parked, and collectGarbage() (host thread) frees
// it once no voice holds it (DESIGN §6).
//
// Determinism (R8). The output depends only on the program, the samples, the
// events and their positions: no clock, no rand(). Each voice runs its own
// 32-sample control grid counted from its note-on, so the bytes do not depend
// on the host's block size either. Voices are summed in note-on order, so the
// floating-point sum does not depend on which pool slot a voice landed in.
//
// The signal path of one voice (DESIGN §3):
//
//   layers (read, pitch, level, layer pan) -> sum -> [drive -> filter] -> amp -> pan -> out
//
// The bracketed stage is gate C5's (it waits for nassau-dsp). The voice
// renders into a stereo block buffer and hands it to that stage before the
// amp; today the stage passes the block through unchanged.

#include <cstdint>
#include <memory>

#include "nd_interp.h"
#include "nd_program.h"
#include "nd_sample.h"

namespace nd {

struct Event {
    enum Type : uint8_t { NoteOn = 0, NoteOff = 1, ModWheel = 2 };
    int32_t offset = 0;  ///< frame within the block, ascending
    Type type = NoteOn;
    uint8_t key = 0;     ///< NoteOn/NoteOff: MIDI key
    uint8_t value = 0;   ///< NoteOn: velocity (0 = note-off); ModWheel: 0..127
};

class Instrument {
public:
    /// Each voice's control grid, in samples (pitch, pan, LFO and the other
    /// modulation are updated once per this many samples).
    static constexpr int kControlBlock = 32;
    /// Voices beyond the pool that a stolen voice fades out in.
    static constexpr int kFadeSlots = 32;
    /// The fade of a stolen or choked voice, in seconds.
    static constexpr double kCutFadeSeconds = 0.005;

    Instrument();
    ~Instrument();
    Instrument(const Instrument&) = delete;
    Instrument& operator=(const Instrument&) = delete;

    /// Allocates the voice pool (for the largest polyphony, so changing the
    /// polyphony later allocates nothing) and the interpolator's table, and
    /// resets. Host thread.
    void prepare(double sampleRate);

    /// Which interpolator reads every layer (DESIGN §3). The default is the
    /// measured choice (docs/GATES.md C2). Host thread.
    void setInterpolator(Interpolator interp);
    Interpolator interpolator() const;

    /// Installs a program snapshot. Host thread.
    void setProgram(std::shared_ptr<const Program> program);
    /// Installs the buffer layer `layerId` plays (nullptr removes it). Host thread.
    void setSample(uint32_t layerId, std::shared_ptr<const Sample> sample);
    /// Frees replaced buffers no voice holds any more; returns how many. Host thread.
    int collectGarbage();

    /// Silences every voice at once and forgets the mod wheel.
    void reset();

    /// Renders n frames (overwriting outL/outR). `timelineFrame` is the
    /// project position of the block's first frame. Audio thread.
    void process(int64_t timelineFrame, const Event* events, int numEvents, float* outL, float* outR, int n);

    /// Voices sounding (including ones fading out after a steal).
    int activeVoices() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nd
