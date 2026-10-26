#include "nd_instrument.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "nd_envelope.h"

namespace nd {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSlots = kMaxPolyphony + Instrument::kFadeSlots;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- The compiled program: fixed-size, copied by a voice at note-on -------------

struct LayerRt {
    uint32_t id = 0;
    int64_t start = 0, end = -1, loopStart = -1, loopEnd = -1;
    int velLo = 0, velHi = 127, velFade = 0, rrGroup = 0;
    double tune = 0.0, level = 0.0, pan = 0.0;
    bool reverse = false;
};

struct ZoneRt {
    uint32_t id = 0;
    int keyLo = 0, keyHi = 127, velLo = 0, velHi = 127, rootKey = 60;
    bool keyTrack = true;
    PlayMode mode = PlayMode::Gated;
    double cutoff = 20000.0, resonance = 0.0, drive = 0.0;
    Envelope::Times amp, filt;
    double lfoRate = 2.0, lfoToPitch = 0.0, lfoToCutoff = 0.0, lfoToPan = 0.0;
    double velToLevel = 1.0, velToCutoff = 0.0, velToStart = 0.0;
    double modWheelToLfoDepth = 0.0, modWheelToCutoff = 0.0, filterKeyTrack = 0.0, filterEnvAmount = 0.0;
    double pan = 0.0, level = 0.0, tune = 0.0;
    int choke = 0, output = 0;
    LayerRt layers[kMaxLayers];
    int numLayers = 0;
};

struct ProgramRt {
    std::vector<ZoneRt> zones;
    int polyphony = kDefaultPolyphony;
    double outputGain = 1.0;
    double masterTune = 0.0;
};

std::unique_ptr<ProgramRt> compile(const Program& p) {
    auto rt = std::make_unique<ProgramRt>();
    rt->polyphony = isValidPolyphony(p.polyphony) ? p.polyphony : kDefaultPolyphony;
    rt->outputGain = dbToGain(p.outputLevel);
    rt->masterTune = p.masterTune;
    for (const Zone& z : p.zones) {
        ZoneRt r;
        r.id = z.id;
        r.keyLo = z.keyLo;
        r.keyHi = z.keyHi;
        r.velLo = z.velLo;
        r.velHi = z.velHi;
        r.rootKey = z.rootKey;
        r.keyTrack = z.keyTrack;
        r.mode = z.mode;
        r.cutoff = z.cutoff;
        r.resonance = z.resonance;
        r.drive = z.drive;
        r.amp = {z.attack, z.hold, z.decay, z.sustain, z.release};
        r.filt = {z.filterAttack, 0.0, z.filterDecay, z.filterSustain, z.filterRelease};
        r.lfoRate = z.lfoRate;
        r.lfoToPitch = z.lfoToPitch;
        r.lfoToCutoff = z.lfoToCutoff;
        r.lfoToPan = z.lfoToPan;
        r.velToLevel = z.velToLevel;
        r.velToCutoff = z.velToCutoff;
        r.velToStart = z.velToStart;
        r.modWheelToLfoDepth = z.modWheelToLfoDepth;
        r.modWheelToCutoff = z.modWheelToCutoff;
        r.filterKeyTrack = z.filterKeyTrack;
        r.filterEnvAmount = z.filterEnvAmount;
        r.pan = z.pan;
        r.level = z.level;
        r.tune = z.tune;
        r.choke = z.choke;
        r.output = z.output;
        for (const Layer& l : z.layers) {
            if (r.numLayers >= kMaxLayers) break;
            LayerRt& o = r.layers[r.numLayers++];
            o.id = l.id;
            o.start = l.start;
            o.end = l.end;
            o.loopStart = l.loopStart;
            o.loopEnd = l.loopEnd;
            // A layer's window is clamped into its zone's (DESIGN §2).
            o.velLo = std::max(l.velLo, z.velLo);
            o.velHi = std::min(l.velHi, z.velHi);
            o.velFade = l.velFade;
            o.rrGroup = l.rrGroup;
            o.tune = l.tune;
            o.level = l.level;
            o.pan = l.pan;
            o.reverse = l.reverse;
        }
        rt->zones.push_back(r);
    }
    return rt;
}

// ---- One voice --------------------------------------------------------------------

struct VoiceLayer {
    std::shared_ptr<const Sample> sample;  // see Instrument::Impl::retired
    double pos = 0.0;
    double factor = 1.0;  // sample-rate ratio x layer tune
    double inc = 0.0;     // this control block's step, frames per output frame
    int64_t lo = 0, hi = 0, loopLo = 0, loopHi = 0;
    int channels = 1;
    bool loop = false, inLoop = false, reverse = false, active = false;
    float gainL = 0.0f, gainR = 0.0f;  // level x velocity x layer pan
};

struct Voice {
    bool active = false;
    uint64_t seq = 0;  // note-on order
    int key = 0, vel = 0;
    bool noteOffSeen = false;
    ZoneRt zone;
    VoiceLayer layers[kMaxLayers];
    int numLayers = 0;
    Envelope amp, filt;
    double keyOffset = 0.0;  // semitones from the root (0 with key tracking off)
    double lfoPhase = 0.0;   // cycles, from note-on
    int ctrlLeft = 0;
    float panL = 1.0f, panR = 1.0f;
    double cutoffHz = 20000.0;  // what the drive/filter stage (C5) is handed
    bool fading = false;
    float fadeGain = 1.0f, fadeStep = 0.0f;
};

// The taps an interpolator reads around floor(pos): x[i0 - L] .. x[i0 + R].
template <Interpolator I>
struct Kernel;

template <>
struct Kernel<Interpolator::Hermite4> {
    static constexpr int L = 1, R = 2, N = 4;
    static void weights(const SincTable&, float t, float* w) {
        const float t2 = t * t, t3 = t2 * t;
        w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
        w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
        w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
        w[3] = 0.5f * (t3 - t2);
    }
};

template <>
struct Kernel<Interpolator::Sinc> {
    static constexpr int L = SincTable::kLeft, R = SincTable::kHalf, N = SincTable::kTaps;
    static void weights(const SincTable& table, float t, float* w) { table.coefficients(t, w); }
};

// Frame index i mapped for the slow path: periodic inside a loop once the
// read point has entered it, zero outside [lo, hi) otherwise.
inline const float* tapFrame(const VoiceLayer& L, int64_t i, const float* zeros) {
    if (L.inLoop) {
        const int64_t len = L.loopHi - L.loopLo;
        int64_t k = (i - L.loopLo) % len;
        if (k < 0) k += len;
        return L.sample->frame(L.loopLo + k);
    }
    if (i < L.lo || i >= L.hi) return zeros;
    return L.sample->frame(i);
}

template <Interpolator I>
void renderLayer(VoiceLayer& L, const SincTable& table, float* bl, float* br, int k) {
    using K = Kernel<I>;
    static const float zeros[2] = {0.0f, 0.0f};
    float w[K::N];
    const int ch = L.channels;
    const double step = L.reverse ? -L.inc : L.inc;
    for (int s = 0; s < k; ++s) {
        const double fl = std::floor(L.pos);
        const int64_t i0 = static_cast<int64_t>(fl);
        K::weights(table, static_cast<float>(L.pos - fl), w);
        const int64_t first = i0 - K::L, last = i0 + K::R;
        const bool fast = L.inLoop ? (first >= L.loopLo && last < L.loopHi) : (first >= L.lo && last < L.hi);
        float a = 0.0f, b = 0.0f;
        if (fast) {
            const float* p = L.sample->frame(first);
            if (ch == 1) {
                for (int j = 0; j < K::N; ++j) a += w[j] * p[j];
            } else {
                for (int j = 0; j < K::N; ++j) {
                    a += w[j] * p[2 * j];
                    b += w[j] * p[2 * j + 1];
                }
            }
        } else {
            for (int j = 0; j < K::N; ++j) {
                const float* p = tapFrame(L, first + j, zeros);
                a += w[j] * p[0];
                if (ch == 2) b += w[j] * p[1];
            }
        }
        if (ch == 1) {
            bl[s] += a * L.gainL;
            br[s] += a * L.gainR;
        } else {
            bl[s] += a * L.gainL;
            br[s] += b * L.gainR;
        }

        L.pos += step;
        if (L.loop) {
            const double len = static_cast<double>(L.loopHi - L.loopLo);
            if (!L.reverse) {
                if (!L.inLoop && L.pos >= static_cast<double>(L.loopLo)) L.inLoop = true;
                if (L.inLoop)
                    while (L.pos >= static_cast<double>(L.loopHi)) L.pos -= len;
            } else {
                if (!L.inLoop && L.pos < static_cast<double>(L.loopHi)) L.inLoop = true;
                if (L.inLoop)
                    while (L.pos < static_cast<double>(L.loopLo)) L.pos += len;
            }
        } else if (L.reverse ? (L.pos < static_cast<double>(L.lo)) : (L.pos >= static_cast<double>(L.hi))) {
            L.active = false;
            return;
        }
    }
}

}  // namespace

// ---- The instrument ------------------------------------------------------------------

struct Instrument::Impl {
    double sampleRate = 48000.0;
    // The measured choice, docs/GATES.md C2.
    Interpolator interp = Interpolator::Sinc;
    SincTable sinc;

    std::shared_ptr<const Program> program;  // kept for the host's reference
    std::unique_ptr<ProgramRt> rt;

    struct SampleSlot {
        uint32_t layerId;
        std::shared_ptr<const Sample> sample;
    };
    std::vector<SampleSlot> samples;  // sorted by layerId
    // Replaced buffers wait here until no voice holds them. Holding one
    // reference here guarantees a voice dropping its reference on the audio
    // thread is never the last one, so no buffer is freed there (DESIGN §6).
    std::vector<std::shared_ptr<const Sample>> retired;

    std::vector<Voice> voices;  // kSlots: the pool [0, polyphony), fade slots after kMaxPolyphony
    std::vector<int> order;     // active voices in note-on order
    int numOrder = 0;
    uint64_t nextSeq = 1;
    int modWheel = 0;
    int64_t cutFadeSamples = 240;

    const Sample* findSample(uint32_t layerId, std::shared_ptr<const Sample>** ref) {
        size_t lo = 0, hi = samples.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (samples[mid].layerId < layerId) lo = mid + 1;
            else hi = mid;
        }
        if (lo < samples.size() && samples[lo].layerId == layerId && samples[lo].sample) {
            *ref = &samples[lo].sample;
            return samples[lo].sample.get();
        }
        return nullptr;
    }

    void removeFromOrder(int pos) {
        for (int i = pos; i + 1 < numOrder; ++i) order[static_cast<size_t>(i)] = order[static_cast<size_t>(i + 1)];
        --numOrder;
    }

    void killVoice(Voice& v) {
        v.active = false;
        for (auto& l : v.layers) l.sample.reset();  // never the last reference (see retired)
    }

    void startFade(Voice& v) {
        if (v.fading) return;
        v.fading = true;
        v.fadeStep = static_cast<float>(1.0 / static_cast<double>(std::max<int64_t>(1, cutFadeSamples)));
    }

    // A free slot of the pool, stealing the oldest voice if there is none
    // (DESIGN §4.5): the stolen voice moves to a fade slot and fades out there,
    // keeping its place in the summing order.
    int allocate() {
        const int poly = rt ? rt->polyphony : kDefaultPolyphony;
        for (int i = 0; i < poly; ++i)
            if (!voices[static_cast<size_t>(i)].active) return i;
        // Oldest voice of the pool = first in note-on order.
        int victimPos = -1;
        for (int i = 0; i < numOrder; ++i) {
            if (order[static_cast<size_t>(i)] < poly) {
                victimPos = i;
                break;
            }
        }
        if (victimPos < 0) return -1;  // cannot happen: the pool is full of active voices
        const int victim = order[static_cast<size_t>(victimPos)];
        int fadeSlot = -1;
        for (int i = kMaxPolyphony; i < kSlots; ++i) {
            if (!voices[static_cast<size_t>(i)].active) {
                fadeSlot = i;
                break;
            }
        }
        if (fadeSlot < 0) {
            // Every fade slot busy: the oldest fading voice ends at once.
            for (int i = 0; i < numOrder; ++i) {
                if (order[static_cast<size_t>(i)] >= kMaxPolyphony) {
                    fadeSlot = order[static_cast<size_t>(i)];
                    killVoice(voices[static_cast<size_t>(fadeSlot)]);
                    removeFromOrder(i);
                    if (i < victimPos) --victimPos;
                    break;
                }
            }
        }
        Voice& from = voices[static_cast<size_t>(victim)];
        Voice& to = voices[static_cast<size_t>(fadeSlot)];
        to = from;  // copies the voice; shared_ptr copies allocate nothing
        startFade(to);
        order[static_cast<size_t>(victimPos)] = fadeSlot;
        killVoice(from);
        return victim;
    }

    void noteOn(int key, int vel, int64_t /*position*/) {
        if (!rt) return;
        for (const ZoneRt& z : rt->zones) {
            if (key < z.keyLo || key > z.keyHi || vel < z.velLo || vel > z.velHi) continue;
            // The layers this velocity plays.
            int picked[kMaxLayers];
            int numPicked = 0;
            for (int j = 0; j < z.numLayers; ++j) {
                const LayerRt& l = z.layers[j];
                if (vel >= l.velLo && vel <= l.velHi) picked[numPicked++] = j;
            }
            if (numPicked == 0) continue;  // a velocity gap plays nothing (DESIGN §2)
            startVoice(z, key, vel, picked, numPicked);
        }
    }

    void startVoice(const ZoneRt& z, int key, int vel, const int* picked, int numPicked) {
        const int slot = allocate();
        if (slot < 0) return;
        Voice& v = voices[static_cast<size_t>(slot)];
        v = Voice{};
        v.active = true;
        v.seq = nextSeq++;
        v.key = key;
        v.vel = vel;
        v.zone = z;
        v.keyOffset = z.keyTrack ? static_cast<double>(key - z.rootKey) : 0.0;
        const double v01 = static_cast<double>(vel) / 127.0;
        // velocity -> level: full level at 127, (1 - velToLevel) at 0, linear.
        const double velGain = 1.0 - z.velToLevel * (1.0 - v01);
        const double voiceGain = velGain * dbToGain(z.level) * rt->outputGain;
        for (int n = 0; n < numPicked; ++n) {
            const LayerRt& lr = z.layers[picked[n]];
            std::shared_ptr<const Sample>* ref = nullptr;
            const Sample* s = findSample(lr.id, &ref);
            if (!s || s->frames() <= 0) continue;
            VoiceLayer& L = v.layers[v.numLayers];
            L.sample = *ref;
            L.channels = s->channels();
            L.hi = (lr.end < 0) ? s->frames() : std::min<int64_t>(lr.end, s->frames());
            L.lo = std::min<int64_t>(std::max<int64_t>(lr.start, 0), L.hi);
            L.loop = (z.mode == PlayMode::Loop || z.mode == PlayMode::TempoLoop);
            L.loopLo = (lr.loopStart < 0) ? L.lo : clampInt(lr.loopStart, L.lo, L.hi);
            L.loopHi = (lr.loopEnd < 0) ? L.hi : clampInt(lr.loopEnd, L.loopLo, L.hi);
            if (L.loopHi - L.loopLo < 2) L.loop = false;
            L.reverse = lr.reverse;
            if (L.hi - L.lo < 1) continue;
            L.pos = L.reverse ? static_cast<double>(L.hi - 1) : static_cast<double>(L.lo);
            L.factor = std::exp2(lr.tune / 12.0) * s->sampleRate() / sampleRate;
            const double g = voiceGain * dbToGain(lr.level);
            const double pan = clampd(lr.pan, -1.0, 1.0);
            if (L.channels == 1) {
                // A mono layer: constant-power pan (DESIGN §2), -3 dB per side at centre.
                const double a = (pan + 1.0) * kPi / 4.0;
                L.gainL = static_cast<float>(g * std::cos(a));
                L.gainR = static_cast<float>(g * std::sin(a));
            } else {
                // A stereo layer: pan is a balance.
                L.gainL = static_cast<float>(g * (pan > 0.0 ? 1.0 - pan : 1.0));
                L.gainR = static_cast<float>(g * (pan < 0.0 ? 1.0 + pan : 1.0));
            }
            L.active = true;
            ++v.numLayers;
        }
        if (v.numLayers == 0) {
            killVoice(v);
            return;
        }
        v.amp.setup(z.amp, sampleRate);
        v.filt.setup(z.filt, sampleRate);
        v.amp.noteOn();
        v.filt.noteOn();
        v.ctrlLeft = 0;
        order[static_cast<size_t>(numOrder++)] = slot;
    }

    static int64_t clampInt(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

    void noteOff(int key) {
        for (int i = 0; i < numOrder; ++i) {
            Voice& v = voices[static_cast<size_t>(order[static_cast<size_t>(i)])];
            if (v.key != key || v.noteOffSeen) continue;
            v.noteOffSeen = true;
            // One-shot modes ignore note-off (DESIGN §5).
            if (v.zone.mode == PlayMode::OneShot || v.zone.mode == PlayMode::TempoOneShot) continue;
            v.amp.noteOff();
            v.filt.noteOff();
        }
    }

    // Once per control block of a voice: pitch, pan and modulation.
    void control(Voice& v) {
        const ZoneRt& z = v.zone;
        const double lfo = std::sin(2.0 * kPi * v.lfoPhase);
        v.lfoPhase += z.lfoRate * kControlBlock / sampleRate;
        v.lfoPhase -= std::floor(v.lfoPhase);
        const double semis = v.keyOffset + z.tune + rt_masterTune(v) + lfo * z.lfoToPitch;
        const double vf = std::exp2(semis / 12.0);
        for (int j = 0; j < v.numLayers; ++j) v.layers[j].inc = v.layers[j].factor * vf;
        const double pan = clampd(z.pan + lfo * z.lfoToPan, -1.0, 1.0);
        // The voice's pan stage works on a stereo signal: a balance.
        v.panL = static_cast<float>(pan > 0.0 ? 1.0 - pan : 1.0);
        v.panR = static_cast<float>(pan < 0.0 ? 1.0 + pan : 1.0);
    }

    double rt_masterTune(const Voice&) const { return rt ? rt->masterTune : 0.0; }

    // Renders one voice, adding into out. Returns false when the voice ended.
    bool render(Voice& v, float* outL, float* outR, int n) {
        int done = 0;
        while (done < n) {
            if (v.ctrlLeft == 0) {
                control(v);
                v.ctrlLeft = kControlBlock;
            }
            const int k = std::min(n - done, v.ctrlLeft);
            float bl[kControlBlock] = {};
            float br[kControlBlock] = {};
            for (int j = 0; j < v.numLayers; ++j) {
                VoiceLayer& L = v.layers[j];
                if (!L.active) continue;
                if (interp == Interpolator::Sinc) renderLayer<Interpolator::Sinc>(L, sinc, bl, br, k);
                else renderLayer<Interpolator::Hermite4>(L, sinc, bl, br, k);
            }
            // ---- drive -> filter: gate C5 slots in here, on bl/br ----
            for (int s = 0; s < k; ++s) {
                float g = static_cast<float>(v.amp.next());
                v.filt.next();
                if (v.fading) {
                    g *= v.fadeGain;
                    v.fadeGain = std::max(0.0f, v.fadeGain - v.fadeStep);
                }
                outL[done + s] += bl[s] * g * v.panL;
                outR[done + s] += br[s] * g * v.panR;
            }
            done += k;
            v.ctrlLeft -= k;

            bool anyLayer = false;
            for (int j = 0; j < v.numLayers; ++j) anyLayer = anyLayer || v.layers[j].active;
            const bool ampDone = v.amp.idle() ||
                                 (v.amp.stage() == Envelope::Stage::Sustain && v.amp.value() <= 0.0);
            if (!anyLayer || ampDone || (v.fading && v.fadeGain <= 0.0f)) return false;
        }
        return true;
    }
};

Instrument::Instrument() : impl_(std::make_unique<Impl>()) {}
Instrument::~Instrument() = default;

void Instrument::prepare(double sampleRate) {
    Impl& m = *impl_;
    m.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m.cutFadeSamples = std::max<int64_t>(1, static_cast<int64_t>(std::llround(kCutFadeSeconds * m.sampleRate)));
    if (!m.sinc.built()) m.sinc.build();
    m.voices.assign(static_cast<size_t>(kSlots), Voice{});
    m.order.assign(static_cast<size_t>(kSlots), 0);
    reset();
}

void Instrument::setInterpolator(Interpolator interp) { impl_->interp = interp; }
Interpolator Instrument::interpolator() const { return impl_->interp; }

void Instrument::setProgram(std::shared_ptr<const Program> program) {
    Impl& m = *impl_;
    std::unique_ptr<ProgramRt> rt = program ? compile(*program) : nullptr;
    const int oldPoly = m.rt ? m.rt->polyphony : kDefaultPolyphony;
    m.rt = std::move(rt);
    m.program = std::move(program);
    // A smaller pool ends the voices beyond it at once (a state change).
    const int poly = m.rt ? m.rt->polyphony : kDefaultPolyphony;
    if (poly < oldPoly && !m.voices.empty()) {
        for (int i = m.numOrder - 1; i >= 0; --i) {
            const int slot = m.order[static_cast<size_t>(i)];
            if (slot >= poly && slot < kMaxPolyphony) {
                m.killVoice(m.voices[static_cast<size_t>(slot)]);
                m.removeFromOrder(i);
            }
        }
    }
}

void Instrument::setSample(uint32_t layerId, std::shared_ptr<const Sample> sample) {
    Impl& m = *impl_;
    auto it = std::lower_bound(m.samples.begin(), m.samples.end(), layerId,
                               [](const Impl::SampleSlot& s, uint32_t id) { return s.layerId < id; });
    if (it != m.samples.end() && it->layerId == layerId) {
        if (it->sample) m.retired.push_back(std::move(it->sample));
        it->sample = std::move(sample);
    } else {
        m.samples.insert(it, Impl::SampleSlot{layerId, std::move(sample)});
    }
}

int Instrument::collectGarbage() {
    Impl& m = *impl_;
    int freed = 0;
    for (size_t i = 0; i < m.retired.size();) {
        if (m.retired[i].use_count() == 1) {
            m.retired.erase(m.retired.begin() + static_cast<std::ptrdiff_t>(i));
            ++freed;
        } else {
            ++i;
        }
    }
    return freed;
}

void Instrument::reset() {
    Impl& m = *impl_;
    for (auto& v : m.voices) m.killVoice(v);
    m.numOrder = 0;
    m.nextSeq = 1;
    m.modWheel = 0;
}

void Instrument::process(int64_t timelineFrame, const Event* events, int numEvents, float* outL, float* outR,
                         int n) {
    Impl& m = *impl_;
    for (int i = 0; i < n; ++i) {
        outL[i] = 0.0f;
        outR[i] = 0.0f;
    }
    if (m.voices.empty()) return;  // not prepared
    int pos = 0;
    int e = 0;
    while (pos < n || e < numEvents) {
        // Events at this frame first.
        while (e < numEvents && events[e].offset <= pos) {
            const Event& ev = events[e++];
            switch (ev.type) {
                case Event::NoteOn:
                    if (ev.value == 0) m.noteOff(ev.key);
                    else m.noteOn(ev.key, ev.value, timelineFrame + pos);
                    break;
                case Event::NoteOff: m.noteOff(ev.key); break;
                case Event::ModWheel: m.modWheel = ev.value; break;
            }
        }
        if (pos >= n) break;
        const int until = (e < numEvents) ? std::min(n, std::max(pos + 1, static_cast<int>(events[e].offset))) : n;
        for (int i = 0; i < m.numOrder;) {
            Voice& v = m.voices[static_cast<size_t>(m.order[static_cast<size_t>(i)])];
            if (!m.render(v, outL + pos, outR + pos, until - pos)) {
                m.killVoice(v);
                m.removeFromOrder(i);
            } else {
                ++i;
            }
        }
        pos = until;
    }
}

int Instrument::activeVoices() const { return impl_->numOrder; }

}  // namespace nd
