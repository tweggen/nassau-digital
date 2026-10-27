#include "nd_instrument.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "nd_envelope.h"
#include "nd_kit_bus.h"
#include "nd_voice_fx.h"

static_assert(nd::Instrument::kControlBlock == nassau::kControlBlock,
              "the voice's control grid is nassau-dsp's control block (DESIGN §11)");

namespace nd {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSlots = kMaxPolyphony + Instrument::kFadeSlots;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// SplitMix64's finaliser: a well-mixed 64-bit hash, deterministic everywhere.
uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// The round-robin pick (DESIGN §2.2): a function of the note's timeline
// position, its key and the zone (and the group), never of earlier notes.
uint64_t rrHash(int64_t position, int key, uint32_t zoneId, int group) {
    uint64_t h = mix64(static_cast<uint64_t>(position));
    h = mix64(h ^ (static_cast<uint64_t>(static_cast<uint32_t>(key)) << 32) ^ zoneId);
    return mix64(h ^ static_cast<uint64_t>(static_cast<uint32_t>(group)));
}

// Equal-power velocity crossfade (DESIGN §2): over velFade steps at each edge
// of the layer's window that lies inside the zone's range, the level ramps as
// sin(pi/2 * t), so two layers overlapping by that width sum to constant
// power. An edge on the zone's own boundary has nothing to fade against and
// does not fade. velFade 0 is a hard switch.
double velocityGain(int vel, int lo, int hi, int fade, int zoneLo, int zoneHi) {
    if (fade <= 0) return 1.0;
    double g = 1.0;
    const double f = static_cast<double>(fade);
    if (lo > zoneLo) {
        const double t = (vel - lo) / f;
        if (t < 1.0) g *= std::sin(1.5707963267948966 * std::max(t, 0.0));
    }
    if (hi < zoneHi) {
        const double t = (hi - vel) / f;
        if (t < 1.0) g *= std::sin(1.5707963267948966 * std::max(t, 0.0));
    }
    return g;
}
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- The compiled program: fixed-size, copied by a voice at note-on -------------

struct LayerRt {
    uint32_t id = 0;
    int64_t start = 0, end = -1, loopStart = -1, loopEnd = -1;
    int velLo = 0, velHi = 127, velFade = 0, rrGroup = 0;
    double tune = 0.0, level = 0.0, pan = 0.0, beats = 0.0;
    bool reverse = false;
};

struct ZoneRt {
    uint32_t id = 0;
    int keyLo = 0, keyHi = 127, velLo = 0, velHi = 127, rootKey = 60;
    bool keyTrack = true;
    PlayMode mode = PlayMode::Gated;
    TempoMethod tempoMethod = TempoMethod::Stretch;
    FilterType filter = FilterType::Lp24;
    DriveCurve driveCurve = DriveCurve::Soft;
    double cutoff = 20000.0, resonance = 0.0, drive = 0.0, driveMix = 1.0;
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
    double busDrive = 0.0;
};

std::unique_ptr<ProgramRt> compile(const Program& p) {
    auto rt = std::make_unique<ProgramRt>();
    rt->polyphony = isValidPolyphony(p.polyphony) ? p.polyphony : kDefaultPolyphony;
    rt->outputGain = dbToGain(p.outputLevel);
    rt->masterTune = p.masterTune;
    rt->busDrive = p.busDrive;
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
        r.tempoMethod = z.tempoMethod;
        r.filter = z.filter;
        r.driveCurve = z.driveCurve;
        r.cutoff = z.cutoff;
        r.resonance = z.resonance;
        r.drive = z.drive;
        r.driveMix = z.driveMix;
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
            o.beats = l.beats;
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
    double tempoBase = 0.0;  // resample tempo modes: the tempo the layer plays at its own rate (BPM); 0 = none
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
    bool layersDone = false;    // every layer has ended; the stage's tail is still sounding
    int64_t tailAge = 0;        // samples since the layers ended
    float panL = 1.0f, panR = 1.0f;
    int64_t age = 0;            // samples since note-on
    double velocity01 = 0.0;
    VoiceFx fx;                 // the drive -> filter stage (nd_voice_fx.h)
    VoiceFxParams fxParams;     // this control block's, modulation applied
    bool fading = false;
    int64_t fadeLeft = 0, fadeTotal = 1;  // fade gain = fadeLeft / fadeTotal, exactly 0 at the end
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
    double tempo = Instrument::kDefaultTempo;  // the song tempo, BPM (DESIGN §5)
    int64_t cutFadeSamples = 240;
    int64_t maxTailSamples = 48000;
    VoiceFxShared fx;  // the drive tables and oversampler taps every voice reads
    KitBus bus;
    ModulationTap tap = nullptr;
    void* tapUser = nullptr;

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
        v.fadeTotal = std::max<int64_t>(1, cutFadeSamples);
        v.fadeLeft = v.fadeTotal;
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

    // ---- Round-robin memory: per key, the layer each (zone, group) chose last
    // in this page. Fixed size, allocated at prepare.
    struct RrEntry {
        uint32_t zoneId = 0;
        int group = 0;
        uint32_t layerId = 0;
    };
    std::vector<RrEntry> rrMemory;  // 128 keys x kRrMemoryPerKey
    std::vector<int> rrCount;       // entries used per key
    std::vector<int> rrNextWrite;   // ring position per key once full

    RrEntry* rrFind(int key, uint32_t zoneId, int group) {
        RrEntry* base = &rrMemory[static_cast<size_t>(key * kRrMemoryPerKey)];
        for (int i = 0; i < rrCount[static_cast<size_t>(key)]; ++i)
            if (base[i].zoneId == zoneId && base[i].group == group) return &base[i];
        return nullptr;
    }

    void rrRemember(int key, uint32_t zoneId, int group, uint32_t layerId) {
        if (RrEntry* e = rrFind(key, zoneId, group)) {
            e->layerId = layerId;
            return;
        }
        RrEntry* base = &rrMemory[static_cast<size_t>(key * kRrMemoryPerKey)];
        int& count = rrCount[static_cast<size_t>(key)];
        int slot;
        if (count < kRrMemoryPerKey) {
            slot = count++;
        } else {
            int& w = rrNextWrite[static_cast<size_t>(key)];
            slot = w;
            w = (w + 1) % kRrMemoryPerKey;
        }
        base[slot] = RrEntry{zoneId, group, layerId};
    }

    void startPage() {
        std::fill(rrCount.begin(), rrCount.end(), 0);
        std::fill(rrNextWrite.begin(), rrNextWrite.end(), 0);
    }

    // The layers a note plays in zone z, with their velocity gains. Returns
    // how many; 0 for a velocity gap (DESIGN §2: plays nothing).
    int pickLayers(const ZoneRt& z, int key, int vel, int64_t position, int* picked, double* gains) {
        int cand[kMaxLayers];
        double candGain[kMaxLayers];
        int n = 0;
        for (int j = 0; j < z.numLayers; ++j) {
            const LayerRt& l = z.layers[j];
            if (vel < l.velLo || vel > l.velHi) continue;
            const double g = velocityGain(vel, l.velLo, l.velHi, l.velFade, z.velLo, z.velHi);
            if (g <= 0.0) continue;
            cand[n] = j;
            candGain[n] = g;
            ++n;
        }
        // Round-robin (DESIGN §2.2): of the candidates sharing a group number
        // (> 0), exactly one plays: picked by a hash of the note's position,
        // key and zone, and moved on by one when it would repeat the layer the
        // same key's previous note played in this page.
        bool keep[kMaxLayers];
        for (int i = 0; i < n; ++i) keep[i] = true;
        for (int i = 0; i < n; ++i) {
            const int group = z.layers[cand[i]].rrGroup;
            if (group <= 0 || !keep[i]) continue;
            bool seen = false;
            for (int k = 0; k < i; ++k) seen = seen || (z.layers[cand[k]].rrGroup == group);
            if (seen) continue;  // this group was decided at its first member
            int members[kMaxLayers];
            int m = 0;
            for (int k = i; k < n; ++k)
                if (z.layers[cand[k]].rrGroup == group) members[m++] = k;
            int choice = static_cast<int>(rrHash(position, key, z.id, group) % static_cast<uint64_t>(m));
            if (m > 1) {
                if (const RrEntry* prev = rrFind(key, z.id, group)) {
                    if (z.layers[cand[members[choice]]].id == prev->layerId) choice = (choice + 1) % m;
                }
            }
            for (int k = 0; k < m; ++k) keep[members[k]] = (k == choice);
            rrRemember(key, z.id, group, z.layers[cand[members[choice]]].id);
        }
        int out = 0;
        for (int i = 0; i < n; ++i) {
            if (!keep[i]) continue;
            picked[out] = cand[i];
            gains[out] = candGain[i];
            ++out;
        }
        return out;
    }

    // Choke groups are by number only (DESIGN §2): a note in group g fades out
    // the sounding voices of group g, and nothing else.
    void choke(int group) {
        for (int i = 0; i < numOrder; ++i) {
            Voice& v = voices[static_cast<size_t>(order[static_cast<size_t>(i)])];
            if (v.zone.choke == group) startFade(v);
        }
    }

    void noteOn(int key, int vel, int64_t position) {
        if (!rt) return;
        // Decide first which zones play, so a note never chokes voices it
        // starts itself (two zones of one group on one key). At most 64
        // overlapping zones sound for one note; more are ignored.
        int zoneIdx[64];
        int picked[64][kMaxLayers];
        double gains[64][kMaxLayers];
        int numPicked[64];
        int numZones = 0;
        for (size_t zi = 0; zi < rt->zones.size() && numZones < 64; ++zi) {
            const ZoneRt& z = rt->zones[zi];
            if (key < z.keyLo || key > z.keyHi || vel < z.velLo || vel > z.velHi) continue;
            const int n = pickLayers(z, key, vel, position, picked[numZones], gains[numZones]);
            if (n == 0) continue;
            zoneIdx[numZones] = static_cast<int>(zi);
            numPicked[numZones] = n;
            ++numZones;
        }
        // Choke groups first, then stealing (DESIGN §4.5).
        for (int i = 0; i < numZones; ++i) {
            const int g = rt->zones[static_cast<size_t>(zoneIdx[i])].choke;
            if (g > 0) choke(g);
        }
        for (int i = 0; i < numZones; ++i)
            startVoice(rt->zones[static_cast<size_t>(zoneIdx[i])], key, vel, picked[i], gains[i], numPicked[i]);
    }

    void startVoice(const ZoneRt& z, int key, int vel, const int* picked, const double* velGains, int numPicked) {
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
        const double voiceGain = velGain * dbToGain(z.level);
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
            // velocity -> sample start: velToStart frames later at velocity 0,
            // none at 127 (DESIGN §4.4). A reversed layer starts that much
            // earlier from its end.
            const double skip = std::floor(z.velToStart * (1.0 - v01));
            L.pos = L.reverse ? static_cast<double>(L.hi - 1) - skip : static_cast<double>(L.lo) + skip;
            if (L.pos < static_cast<double>(L.lo) || L.pos >= static_cast<double>(L.hi)) continue;
            L.factor = std::exp2(lr.tune / 12.0) * s->sampleRate() / sampleRate;
            // Tempo modes by resampling (DESIGN §5): the layer's `beats` span
            // its played length [start, end), which at the sample's own rate
            // makes the tempo it plays at unchanged. At another song tempo the
            // read rate is scaled by songTempo / that tempo (control()), so
            // the pitch moves with it. Unknown beats (0) leave the rate alone.
            // A stretch zone is handed a buffer already rendered to the song
            // tempo by the host (DESIGN §5), so it plays at its own rate here.
            const bool tempoMode = (z.mode == PlayMode::TempoLoop || z.mode == PlayMode::TempoOneShot);
            if (tempoMode && z.tempoMethod == TempoMethod::Resample && lr.beats > 0.0)
                L.tempoBase = lr.beats * 60.0 * s->sampleRate() / static_cast<double>(L.hi - L.lo);
            const double g = voiceGain * dbToGain(lr.level) * velGains[n];
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
        v.velocity01 = v01;
        v.fxParams.filter = z.filter;
        v.fxParams.resonance = z.resonance;
        v.fxParams.driveCurve = z.driveCurve;
        v.fxParams.drive = z.drive;
        v.fxParams.driveMix = z.driveMix;
        v.fxParams.cutoffHz = z.cutoff;
        // The oversampling decision, from the drive at note-on, for the voice's
        // whole life (DESIGN §3).
        v.fx.start(v.fxParams, fx);
        v.ctrlLeft = 0;
        // An oversampled stage delays by latency() frames. Run the front (the
        // layers and the stage) that many frames ahead now, discarding what
        // comes out (the stage's history of silence before the note), so the
        // note's first frame leaves the stage at the voice's first frame.
        if (const int d = v.fx.latency(); d > 0) {
            float bl[kControlBlock], br[kControlBlock];
            for (int left = d; left > 0;) {
                if (v.ctrlLeft == 0) {
                    control(v);
                    v.ctrlLeft = kControlBlock;
                }
                const int k = std::min(left, v.ctrlLeft);
                front(v, bl, br, k);
                v.ctrlLeft -= k;
                left -= k;
            }
        }
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
        const double w = static_cast<double>(modWheel) / 127.0;
        // The mod wheel scales the LFO's depth (DESIGN §4.4).
        const double depth = 1.0 - z.modWheelToLfoDepth + z.modWheelToLfoDepth * w;
        const double semis = v.keyOffset + z.tune + rt_masterTune(v) + lfo * depth * z.lfoToPitch;
        const double vf = std::exp2(semis / 12.0);
        // The tempo ratio is read here, once per control block, so a tempo
        // change reaches a sounding voice at its next control block. Only the
        // read rate changes, never the read position: the waveform carries on
        // from where it was, without a step (C7.2).
        for (int j = 0; j < v.numLayers; ++j) {
            VoiceLayer& L = v.layers[j];
            L.inc = L.factor * vf * (L.tempoBase > 0.0 ? tempo / L.tempoBase : 1.0);
        }
        const double pan = clampd(z.pan + lfo * depth * z.lfoToPan, -1.0, 1.0);
        // The voice's pan stage works on a stereo signal: a balance.
        v.panL = static_cast<float>(pan > 0.0 ? 1.0 - pan : 1.0);
        v.panR = static_cast<float>(pan < 0.0 ? 1.0 + pan : 1.0);
        const double cutSemis = z.velToCutoff * v.velocity01 + z.filterEnvAmount * v.filt.value() +
                                z.lfoToCutoff * lfo * depth + z.modWheelToCutoff * w +
                                z.filterKeyTrack * static_cast<double>(v.key - z.rootKey);
        v.fxParams.cutoffHz = clampd(z.cutoff * std::exp2(cutSemis / 12.0), 20.0, 20000.0);
        v.fx.control(v.fxParams);
        if (tap) {
            ModulationFrame f;
            f.zoneId = z.id;
            f.key = v.key;
            f.age = v.age;
            f.ampEnv = v.amp.value();
            f.filterEnv = v.filt.value();
            f.lfo = lfo;
            f.lfoDepth = depth;
            f.pitch = semis;
            f.pan = pan;
            f.cutoffHz = v.fxParams.cutoffHz;
            f.drive = v.fxParams.drive;
            f.oversampling = v.fx.factor();
            tap(tapUser, f);
        }
    }

    double rt_masterTune(const Voice&) const { return rt ? rt->masterTune : 0.0; }

    // The front of a voice: k frames of its layers, summed, through the drive
    // -> filter stage, into bl/br (overwritten). k <= what is left of the
    // control block.
    void front(Voice& v, float* bl, float* br, int k) {
        for (int s = 0; s < k; ++s) bl[s] = br[s] = 0.0f;
        for (int j = 0; j < v.numLayers; ++j) {
            VoiceLayer& L = v.layers[j];
            if (!L.active) continue;
            if (interp == Interpolator::Sinc) renderLayer<Interpolator::Sinc>(L, sinc, bl, br, k);
            else renderLayer<Interpolator::Hermite4>(L, sinc, bl, br, k);
        }
        v.fx.process(bl, br, k);
    }

    // Renders one voice, adding into out. Returns false when the voice ended.
    bool render(Voice& v, float* outL, float* outR, int n) {
        int done = 0;
        while (done < n) {
            if (v.ctrlLeft == 0) {
                control(v);
                v.ctrlLeft = kControlBlock;
            }
            const int k = std::min(n - done, v.ctrlLeft);
            float bl[kControlBlock];
            float br[kControlBlock];
            front(v, bl, br, k);
            float tailPeak = 0.0f;
            for (int s = 0; s < k; ++s) {
                float g = static_cast<float>(v.amp.next());
                v.filt.next();
                if (v.fading) {
                    g *= static_cast<float>(static_cast<double>(v.fadeLeft) / static_cast<double>(v.fadeTotal));
                    if (v.fadeLeft > 0) --v.fadeLeft;
                }
                outL[done + s] += bl[s] * g * v.panL;
                outR[done + s] += br[s] * g * v.panR;
                tailPeak = std::max(tailPeak, std::max(std::fabs(bl[s]), std::fabs(br[s])));
            }
            done += k;
            v.ctrlLeft -= k;
            v.age += k;

            const bool ampDone = v.amp.idle() ||
                                 (v.amp.stage() == Envelope::Stage::Sustain && v.amp.value() <= 0.0);
            if (ampDone || (v.fading && v.fadeLeft == 0)) return false;
            // When every layer has ended, the stage still holds a tail: the
            // oversampler's delay and the filter's ringing. The voice ends once
            // the delay has passed and a whole chunk of the tail is below
            // kTailFloor; a tail that will not die (a self-oscillating filter
            // on a one-shot zone) is faded out after kMaxTailSeconds.
            if (!v.layersDone) {
                bool anyLayer = false;
                for (int j = 0; j < v.numLayers; ++j) anyLayer = anyLayer || v.layers[j].active;
                if (!anyLayer) v.layersDone = true;
            } else {
                v.tailAge += k;
                if (v.tailAge >= v.fx.latency() && tailPeak <= kTailFloor) return false;
                if (v.tailAge >= maxTailSamples) startFade(v);
            }
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
    m.maxTailSamples = static_cast<int64_t>(std::llround(kMaxTailSeconds * m.sampleRate));
    m.fx.build(m.sampleRate);
    m.bus.prepare(&m.fx);
    if (!m.sinc.built()) m.sinc.build();
    m.voices.assign(static_cast<size_t>(kSlots), Voice{});
    m.order.assign(static_cast<size_t>(kSlots), 0);
    m.rrMemory.assign(static_cast<size_t>(128 * kRrMemoryPerKey), Impl::RrEntry{});
    m.rrCount.assign(128, 0);
    m.rrNextWrite.assign(128, 0);
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
    // Drive follows a sounding voice (DESIGN §3: automating drive changes the
    // sound of the note, and never its oversampling, decided at note-on).
    // Every other zone parameter stays as the voice started with it.
    if (m.rt) {
        for (int i = 0; i < m.numOrder; ++i) {
            Voice& v = m.voices[static_cast<size_t>(m.order[static_cast<size_t>(i)])];
            for (const ZoneRt& z : m.rt->zones) {
                if (z.id != v.zone.id) continue;
                v.zone.drive = z.drive;
                v.fxParams.drive = z.drive;
                break;
            }
        }
    }
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
    m.bus.reset();
    m.startPage();
}

void Instrument::startPage() { impl_->startPage(); }

void Instrument::setTempo(double bpm) {
    if (std::isfinite(bpm) && bpm > 0.0) impl_->tempo = bpm;
}

double Instrument::tempo() const { return impl_->tempo; }

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
    // The kit bus, after the sum of every voice (DESIGN §3), then the output
    // level.
    if (m.rt) {
        m.bus.setDrive(m.rt->busDrive);
        m.bus.process(outL, outR, n);
        if (m.rt->outputGain != 1.0) {
            const float g = static_cast<float>(m.rt->outputGain);
            for (int i = 0; i < n; ++i) {
                outL[i] *= g;
                outR[i] *= g;
            }
        }
    }
}

int Instrument::activeVoices() const { return impl_->numOrder; }

void Instrument::setOversampling(int factor) {
    if (factor == 2 || factor == 4) impl_->fx.factor = factor;
}

int Instrument::oversampling() const { return impl_->fx.factor; }

int Instrument::oversamplingLatency() const { return impl_->fx.latency(); }

bool Instrument::kitBusOversampling() const { return impl_->bus.engaged(); }

void Instrument::setModulationTap(ModulationTap tap, void* user) {
    impl_->tap = tap;
    impl_->tapUser = user;
}

}  // namespace nd
