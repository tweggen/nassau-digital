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
// renders into a stereo block buffer and hands it to that stage (VoiceFx,
// nd_voice_fx.h) before the amp, with the block's modulated cutoff; today the
// stage passes the block through unchanged.
//
// Modulation (DESIGN §4.4), the fixed routes, each documented with its
// direction (v = velocity / 127, w = mod wheel / 127, depth = 1 - m + m*w
// with m = modWheelToLfoDepth, lfo = a sine at lfoRate from phase 0 at
// note-on):
//   level    x (1 - velToLevel * (1 - v))                 louder with velocity
//   start    + velToStart * (1 - v) frames                later at low velocity
//   pitch    + lfoToPitch * lfo * depth semitones
//   pan      + lfoToPan * lfo * depth
//   cutoff   x 2^((velToCutoff * v + filterEnvAmount * filterEnv
//                 + lfoToCutoff * lfo * depth + modWheelToCutoff * w
//                 + filterKeyTrack * (key - rootKey)) / 12), clamped to 20..20000 Hz

#include <cstdint>
#include <memory>

#include "nd_interp.h"
#include "nd_program.h"
#include "nd_sample.h"

namespace nd {

/// What one voice's modulation came to at the start of one control block
/// (see Instrument::setModulationTap).
struct ModulationFrame {
    uint32_t zoneId = 0;
    int key = 0;
    int64_t age = 0;          ///< samples since the voice's note-on
    double ampEnv = 0.0;      ///< 0..1
    double filterEnv = 0.0;   ///< 0..1
    double lfo = 0.0;         ///< -1..1, before depth
    double lfoDepth = 1.0;    ///< the mod wheel's scaling of the LFO routes, 0..1
    double pitch = 0.0;       ///< semitones from the sample's recorded pitch, all routes applied
    double pan = 0.0;         ///< the zone pan stage's position, -1..1
    double cutoffHz = 0.0;    ///< what the drive -> filter stage is handed
};

/// Called from process() once per sounding voice per control block. It runs
/// on the audio thread: it must not allocate, lock or block.
using ModulationTap = void (*)(void* user, const ModulationFrame& frame);

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
    /// Previous round-robin choices remembered per key within a page.
    static constexpr int kRrMemoryPerKey = 16;

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

    /// Silences every voice at once, forgets the mod wheel and starts a page.
    void reset();

    /// Starts a render page (DESIGN §2.2). Round-robin never gives a key the
    /// layer of that key's previous note when that note is in the same page;
    /// this forgets the previous notes, so a page's choices depend on the page
    /// alone and a page rendered on its own equals the same page rendered
    /// after its predecessor. The host calls it before the first block of a
    /// page; voices keep sounding.
    void startPage();

    /// Renders n frames (overwriting outL/outR). `timelineFrame` is the
    /// project position of the block's first frame. Audio thread.
    void process(int64_t timelineFrame, const Event* events, int numEvents, float* outL, float* outR, int n);

    /// Voices sounding (including ones fading out after a steal).
    int activeVoices() const;

    /// Installs (or with nullptr removes) a tap that sees each voice's
    /// modulation once per control block: for an editor's modulation display,
    /// and for measuring routes whose target cannot be heard yet (the filter's
    /// cutoff until gate C5). Host thread.
    void setModulationTap(ModulationTap tap, void* user);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nd
