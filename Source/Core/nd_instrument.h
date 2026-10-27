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
//   layers (read, pitch, level, layer pan) -> sum -> [drive -> filter] -> amp -> pan
//   all voices -> sum -> kit-bus drive -> output level -> out
//
// The bracketed stage (VoiceFx, nd_voice_fx.h) is nassau-dsp's filters behind
// the drive waveshaper (nd_drive.h), oversampled when the zone's drive is
// above zero at note-on. Its oversampling delay is compensated: an
// oversampled voice runs its layers and that stage `oversamplingLatency()`
// frames ahead of its amp, so it lines up with one that is not.
//
// After every voice: the kit bus's drive (nd_kit_bus.h, the program's
// busDrive), then the program's output level (DESIGN §3: Σ -> kit-bus drive
// -> output).
//
// Tempo (DESIGN §5). A resample tempo mode is one more factor in the read
// rate, songTempo / layerTempo, applied per control block (setTempo()).
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
    double pitch = 0.0;       ///< semitones from the sample's recorded pitch, all routes applied (not a resample tempo mode's ratio, which is per layer)
    double pan = 0.0;         ///< the zone pan stage's position, -1..1
    double cutoffHz = 0.0;    ///< what the drive -> filter stage is handed
    double drive = 0.0;       ///< the drive amount the stage is handed (follows the program)
    int oversampling = 1;     ///< the voice's oversampling factor, fixed at note-on (1 = none)
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
    /// The song tempo until the host sets one, in beats per minute.
    static constexpr double kDefaultTempo = 120.0;
    /// A voice whose layers have all ended keeps sounding while its drive ->
    /// filter stage still rings, until a control block's worth of it stays
    /// below this level...
    static constexpr float kTailFloor = 1e-6f;
    /// ...or for at most this long, after which it is faded out (a
    /// self-oscillating filter on a one-shot zone would never fall silent).
    static constexpr double kMaxTailSeconds = 1.0;

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

    /// The song tempo, in beats per minute, that the tempo modes follow
    /// (DESIGN §5). A `tempo-loop` or `tempo-oneshot` zone whose method is
    /// `resample` plays each layer with `beats` > 0 at songTempo / layerTempo
    /// times its own rate, where layerTempo = beats * 60 / (the layer's
    /// played length [start, end) in seconds at the sample's rate): the loop
    /// period follows the tempo and the pitch moves by 12 * log2 of that
    /// ratio. Sounding voices follow a change too, from their next control
    /// block, by changing their read rate and never their read position, so
    /// a change makes no step. A non-finite or non-positive tempo is ignored.
    /// Allocation-free; call it between process() calls (the host's tempo at
    /// the block's start). Unaffected: the `stretch` method (the host hands a
    /// buffer already rendered to the tempo) and layers with beats = 0.
    void setTempo(double bpm);
    double tempo() const;

    /// Voices sounding (including ones fading out after a steal).
    int activeVoices() const;

    /// The oversampling factor (2 or 4) of a voice whose drive is above zero
    /// at note-on, and of the kit bus while its drive is above zero (DESIGN
    /// §3: "a measured choice"; the default is the one docs/GATES.md C5
    /// measured). Voices already sounding keep theirs. Host thread.
    void setOversampling(int factor);
    int oversampling() const;
    /// The oversampling chain's delay, in frames, at the current factor. A
    /// voice compensates it; the kit bus, while its drive is on, does not
    /// (its output is that many frames late then).
    int oversamplingLatency() const;
    /// Whether the kit bus is running its oversampled drive (busDrive > 0).
    bool kitBusOversampling() const;

    /// Installs (or with nullptr removes) a tap that sees each voice's
    /// modulation once per control block: for an editor's modulation display,
    /// and for reading what the drive -> filter stage was handed. Host thread.
    void setModulationTap(ModulationTap tap, void* user);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nd
