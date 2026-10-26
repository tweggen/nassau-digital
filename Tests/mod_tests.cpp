// mod_tests.cpp - C4: envelopes and modulation (DESIGN §4.1, §4.4).
//
// C4.1 the amp envelope (ADSR + hold) and the filter envelope each match
//      their segment times +- 1 ms, measured on the envelope applied to DC.
// C4.2 the LFO at 2 Hz on pitch measures 2.00 +- 0.01 Hz of modulation;
//      velocity -> level, cutoff and sample start each move their quantity in
//      the documented direction; the mod wheel scales the LFO's depth.
//
// The amp envelope, level, start, pitch and pan are measured on audio. The
// filter envelope and the cutoff routes have no audible target until gate
// C5 builds the filter, so they are measured where the voice hands them to
// the drive -> filter stage: the public modulation tap
// (Instrument::setModulationTap), once per 32-sample control block.

#include "nd_edit.h"
#include "nd_envelope.h"
#include "nd_instrument.h"
#include "render_util.h"
#include "test_util.h"

using namespace nd;
using namespace ndtest;

namespace {

constexpr double kRate = 48000.0;
constexpr double kMs = 48.0;  // samples per millisecond
size_t at(double seconds) { return static_cast<size_t>(std::llround(seconds * kRate)); }

std::string ms(double samples) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f ms", samples / kMs);
    return b;
}

// One zone, one mono layer panned hard left (gain 1 on L), velocity-insensitive.
Zone dcZone(PlayMode mode) {
    Zone z = fullZone(mode, 60);
    z.velToLevel = 0.0;
    z.keyTrack = false;
    z.layers[0].pan = -1.0;
    return z;
}

void installOne(Instrument& inst, Program& prog, const Zone& z, std::shared_ptr<const Sample> s) {
    prog = Program{};
    addZone(prog, z);
    install(inst, prog);
    inst.setSample(prog.zones[0].layers[0].id, std::move(s));
}

struct TapLog {
    std::vector<ModulationFrame> frames;
    static void record(void* user, const ModulationFrame& f) {
        auto* log = static_cast<TapLog*>(user);
        if (log->frames.size() < log->frames.capacity()) log->frames.push_back(f);  // no allocation in the tap
    }
};

// Where the line through (x0, y0) and (x1, y1) reaches y.
double crossAt(double x0, double y0, double x1, double y1, double y) { return x0 + (y - y0) * (x1 - x0) / (y1 - y0); }

// The instantaneous frequency of a sine, one value per period (from rising
// zero crossings), as (time in samples, Hz) pairs.
void instFreq(const std::vector<float>& v, size_t from, size_t to, std::vector<double>& t, std::vector<double>& f) {
    double prev = -1.0;
    for (size_t i = from + 1; i < to; ++i) {
        const double y0 = v[i - 1], y1 = v[i];
        if (y0 < 0.0 && y1 >= 0.0) {
            const double c = static_cast<double>(i - 1) + (-y0) / (y1 - y0);
            if (prev >= 0.0) {
                t.push_back(0.5 * (c + prev));
                f.push_back(kRate / (c - prev));
            }
            prev = c;
        }
    }
}

// Rate (Hz) and depth (Hz, half peak-to-peak by RMS*sqrt2) of the frequency
// modulation in v[from, to).
void fmRateDepth(const std::vector<float>& v, size_t from, size_t to, double& rate, double& depth) {
    std::vector<double> t, f;
    instFreq(v, from, to, t, f);
    double mean = 0;
    for (double x : f) mean += x;
    mean /= static_cast<double>(f.size());
    double first = -1, last = -1, sq = 0;
    int count = 0;
    for (size_t i = 0; i < f.size(); ++i) sq += (f[i] - mean) * (f[i] - mean);
    depth = std::sqrt(sq / static_cast<double>(f.size())) * std::sqrt(2.0);
    for (size_t i = 1; i < f.size(); ++i) {
        const double a = f[i - 1] - mean, b = f[i] - mean;
        if (a < 0.0 && b >= 0.0) {
            const double c = t[i - 1] + (-a) / (b - a) * (t[i] - t[i - 1]);
            if (first < 0) first = c;
            last = c;
            ++count;
        }
    }
    rate = (count >= 2) ? (count - 1) * kRate / (last - first) : 0.0;
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: envelopes and modulation (C4) ===\n");

    // ---------------------------------------------------------------- C4.1 ---
    group("C4.1 the amp envelope (ADSR + hold) on DC: each segment's time +- 1 ms");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = dcZone(PlayMode::Gated);
        z.attack = 0.050;
        z.hold = 0.030;
        z.decay = 0.100;
        z.sustain = 0.5;
        z.release = 0.200;
        installOne(inst, prog, z, Sample::mono(kRate, std::vector<float>(at(2.0), 1.0f)));
        const size_t tOff = at(0.5);
        const Stereo a = render(inst, {on(0, 60), off(static_cast<int64_t>(tOff), 60)}, at(1.0));
        const std::vector<float>& y = a.l;
        size_t attackEnd = 0, holdEnd = 0, decayEnd = 0, releaseEnd = 0;
        for (size_t i = 0; i < y.size(); ++i)
            if (y[i] >= 1.0f - 1e-6f) { attackEnd = i; break; }
        for (size_t i = attackEnd; i < y.size(); ++i)
            if (y[i] < 1.0f - 1e-6f) { holdEnd = i - 1; break; }
        for (size_t i = holdEnd; i < y.size(); ++i)
            if (y[i] <= 0.5f + 1e-6f) { decayEnd = i; break; }
        for (size_t i = tOff; i < y.size(); ++i)
            if (y[i] == 0.0f) { releaseEnd = i; break; }
        const double attack = static_cast<double>(attackEnd), hold = static_cast<double>(holdEnd - attackEnd);
        const double decay = static_cast<double>(decayEnd - holdEnd), release = static_cast<double>(releaseEnd - tOff);
        note("attack " + ms(attack) + " (set 50), hold " + ms(hold) + " (30), decay " + ms(decay) + " (100), sustain " +
             std::to_string(y[at(0.3)]) + " (0.5), release " + ms(release) + " (200)");
        check("attack 50 ms +- 1 ms", std::fabs(attack - 50 * kMs) <= kMs);
        check("hold 30 ms +- 1 ms", std::fabs(hold - 30 * kMs) <= kMs);
        check("decay 100 ms +- 1 ms", std::fabs(decay - 100 * kMs) <= kMs);
        check("sustain level 0.5", std::fabs(y[at(0.3)] - 0.5f) < 1e-6f);
        check("release 200 ms +- 1 ms", std::fabs(release - 200 * kMs) <= kMs);
        // Release from mid-attack takes the release time too (from wherever it is).
        inst.reset();
        const Stereo b = render(inst, {on(0, 60), off(at(0.025), 60)}, at(0.5));
        size_t end2 = 0;
        for (size_t i = at(0.025); i < b.l.size(); ++i)
            if (b.l[i] == 0.0f) { end2 = i; break; }
        note("released mid-attack at 25 ms (level " + std::to_string(b.l[at(0.025)]) + "): release lasts " +
             ms(static_cast<double>(end2 - at(0.025))));
        check("a release begun mid-attack lasts the release time, 200 ms +- 1 ms",
              std::fabs(static_cast<double>(end2 - at(0.025)) - 200 * kMs) <= kMs);
    }

    group("C4.1 the filter envelope (ADSR), measured at the drive -> filter stage");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = dcZone(PlayMode::Gated);
        z.filterAttack = 0.040;
        z.filterDecay = 0.120;
        z.filterSustain = 0.3;
        z.filterRelease = 0.150;
        z.release = 0.5;  // the voice outlives the filter release
        installOne(inst, prog, z, Sample::mono(kRate, std::vector<float>(at(2.0), 1.0f)));
        TapLog log;
        log.frames.reserve(100000);
        inst.setModulationTap(&TapLog::record, &log);
        const double tOff = 0.4 * kRate;
        render(inst, {on(0, 60), off(static_cast<int64_t>(tOff), 60)}, at(1.0));
        inst.setModulationTap(nullptr, nullptr);
        // Linear segments: fit each through two of its tapped points, then
        // intersect with the level it ends at.
        auto valueAt = [&](double age) {
            for (size_t i = 1; i < log.frames.size(); ++i)
                if (static_cast<double>(log.frames[i].age) >= age) return i;
            return log.frames.size() - 1;
        };
        const auto& F = log.frames;
        const size_t a0 = valueAt(10 * kMs), a1 = valueAt(30 * kMs);
        const double attackEnd = crossAt(F[a0].age, F[a0].filterEnv, F[a1].age, F[a1].filterEnv, 1.0);
        const double attackStart = crossAt(F[a0].age, F[a0].filterEnv, F[a1].age, F[a1].filterEnv, 0.0);
        const size_t d0 = valueAt(60 * kMs), d1 = valueAt(140 * kMs);
        const double decayStart = crossAt(F[d0].age, F[d0].filterEnv, F[d1].age, F[d1].filterEnv, 1.0);
        const double decayEnd = crossAt(F[d0].age, F[d0].filterEnv, F[d1].age, F[d1].filterEnv, 0.3);
        const size_t r0 = valueAt(tOff + 20 * kMs), r1 = valueAt(tOff + 120 * kMs);
        const double relStart = crossAt(F[r0].age, F[r0].filterEnv, F[r1].age, F[r1].filterEnv, 0.3);
        const double relEnd = crossAt(F[r0].age, F[r0].filterEnv, F[r1].age, F[r1].filterEnv, 0.0);
        const double sustain = F[valueAt(300 * kMs)].filterEnv;
        note(std::to_string(F.size()) + " control frames; attack " + ms(attackEnd - attackStart) + " (set 40), decay " +
             ms(decayEnd - decayStart) + " (120), sustain " + std::to_string(sustain) + " (0.3), release " +
             ms(relEnd - relStart) + " (150), release starts " + ms(relStart - tOff) + " after note-off");
        check("filter attack 40 ms +- 1 ms (and starts at note-on)",
              std::fabs(attackEnd - attackStart - 40 * kMs) <= kMs && std::fabs(attackStart) <= kMs);
        check("filter decay 120 ms +- 1 ms (and starts where the attack ends)",
              std::fabs(decayEnd - decayStart - 120 * kMs) <= kMs && std::fabs(decayStart - attackEnd) <= kMs);
        check("filter sustain level 0.3", std::fabs(sustain - 0.3) < 1e-9);
        check("filter release 150 ms +- 1 ms (and starts at note-off)",
              std::fabs(relEnd - relStart - 150 * kMs) <= kMs && std::fabs(relStart - tOff) <= kMs);
        // The generator itself, sample by sample, on DC (the same class).
        Envelope e;
        e.setup({0.040, 0.0, 0.120, 0.3, 0.150}, kRate);
        e.noteOn();
        size_t i = 0, aEnd = 0, dEnd = 0;
        for (; i < at(1.0); ++i) {
            const double v = e.next();
            if (!aEnd && v >= 1.0) aEnd = i;
            if (aEnd && !dEnd && v <= 0.3) { dEnd = i; break; }
        }
        note("the generator alone: attack " + ms(static_cast<double>(aEnd)) + ", decay " +
             ms(static_cast<double>(dEnd - aEnd)));
        check("the envelope generator alone: attack 40 ms and decay 120 ms, exact to the sample",
              aEnd == at(0.040) && dEnd - aEnd == at(0.120));
    }

    // ---------------------------------------------------------------- C4.2 ---
    group("C4.2 the LFO at 2 Hz on pitch measures 2.00 +- 0.01 Hz of modulation");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = dcZone(PlayMode::Loop);
        z.lfoRate = 2.0;
        z.lfoToPitch = 1.0;  // +- 1 semitone
        installOne(inst, prog, z, Sample::mono(kRate, sine(1000.0, kRate, 7.0)));
        const Stereo a = render(inst, {on(0, 60)}, at(6.0));
        double rate = 0, depth = 0;
        fmRateDepth(a.l, at(0.25), at(5.75), rate, depth);
        note("measured modulation rate " + std::to_string(rate) + " Hz, depth +-" + std::to_string(depth) +
             " Hz around 1 kHz (1 semitone = +59.5 / -56.1 Hz)");
        check("modulation rate 2.00 +- 0.01 Hz", std::fabs(rate - 2.0) <= 0.01);
        check("modulation depth is about a semitone (40..70 Hz at 1 kHz)", depth > 40.0 && depth < 70.0);

        // The mod wheel scales the depth (modWheelToLfoDepth = 1: depth = wheel).
        z.modWheelToLfoDepth = 1.0;
        installOne(inst, prog, z, Sample::mono(kRate, sine(1000.0, kRate, 7.0)));
        double d[3];
        const int wheels[3] = {0, 64, 127};
        for (int k = 0; k < 3; ++k) {
            inst.reset();
            const Stereo b = render(inst, {wheel(0, wheels[k]), on(0, 60)}, at(3.0));
            double r = 0;
            fmRateDepth(b.l, at(0.25), at(2.75), r, d[k]);
        }
        note("depth with modWheelToLfoDepth 1: wheel 0 -> " + std::to_string(d[0]) + " Hz, 64 -> " + std::to_string(d[1]) +
             " Hz, 127 -> " + std::to_string(d[2]) + " Hz");
        check("mod wheel 0 removes the LFO (depth below 0.05 Hz)", d[0] < 0.05);
        check("mod wheel 64 gives about half the depth of 127 (ratio 0.504 +- 0.03)", std::fabs(d[1] / d[2] - 64.0 / 127.0) < 0.03);
        check("mod wheel 127 gives the full depth (as without the wheel route, +- 2 %)", std::fabs(d[2] / depth - 1.0) < 0.02);
    }

    group("C4.2 the LFO on pan (audio)");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = dcZone(PlayMode::Gated);
        z.layers[0].pan = 0.0;
        z.lfoRate = 2.0;
        z.lfoToPan = 1.0;
        installOne(inst, prog, z, Sample::mono(kRate, std::vector<float>(at(3.0), 1.0f)));
        const Stereo a = render(inst, {on(0, 60)}, at(2.5));
        // L - R swings with the LFO: count its rising zero crossings.
        std::vector<float> diff(a.l.size());
        for (size_t i = 0; i < diff.size(); ++i) diff[i] = a.l[i] - a.r[i];
        const double f = zeroCrossingF0(diff, at(0.1), at(2.4), kRate);
        note("L - R oscillates at " + std::to_string(f) + " Hz; L ranges " + std::to_string(peak(a.l, at(0.1), at(2.4))) +
             " max, R " + std::to_string(peak(a.r, at(0.1), at(2.4))) + " max");
        check("LFO -> pan moves the image at the LFO's rate (2.00 +- 0.01 Hz)", std::fabs(f - 2.0) <= 0.01);
    }

    group("C4.2 velocity -> level (audio): louder with velocity");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = dcZone(PlayMode::Gated);
        z.velToLevel = 1.0;
        installOne(inst, prog, z, Sample::mono(kRate, sine(500.0, kRate, 0.5)));
        double r[3];
        const int vels[3] = {32, 64, 127};
        for (int k = 0; k < 3; ++k) {
            inst.reset();
            const Stereo a = render(inst, {on(0, 60, vels[k])}, at(0.3));
            r[k] = rms(a.l, at(0.05), at(0.25));
        }
        note("RMS at velocity 32 / 64 / 127: " + std::to_string(r[0]) + " / " + std::to_string(r[1]) + " / " +
             std::to_string(r[2]) + " (documented: x(1 - velToLevel*(1 - v/127)), so 0.252 / 0.504 / 1 of full)");
        check("level rises with velocity", r[0] < r[1] && r[1] < r[2]);
        check("as documented: velocity 64 is 64/127 of velocity 127 (+- 0.1 %)", std::fabs(r[1] / r[2] - 64.0 / 127.0) < 1e-3);
    }

    group("C4.2 velocity -> sample start (audio): later in the sample at low velocity");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = dcZone(PlayMode::Gated);
        z.velToStart = 12700.0;  // 100 frames per velocity step
        std::vector<float> ramp(48000);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 48000.0f;
        installOne(inst, prog, z, Sample::mono(kRate, ramp));
        double startAt[3];
        const int vels[3] = {1, 64, 127};
        for (int k = 0; k < 3; ++k) {
            inst.reset();
            const Stereo a = render(inst, {on(0, 60, vels[k])}, 400);
            // The ramp's value 100 frames in says where in the sample it started.
            startAt[k] = a.l[100] * 48000.0 - 100.0;
        }
        note("start frame at velocity 1 / 64 / 127: " + std::to_string(startAt[0]) + " / " + std::to_string(startAt[1]) +
             " / " + std::to_string(startAt[2]) + " (documented: velToStart * (1 - v/127) = 12600 / 6300 / 0)");
        check("the start moves later as velocity falls", startAt[0] > startAt[1] && startAt[1] > startAt[2]);
        check("as documented, +- 1 frame", std::fabs(startAt[0] - 12600) <= 1 && std::fabs(startAt[1] - 6300) <= 1 &&
                                               std::fabs(startAt[2]) <= 1);
    }

    group("C4.2 cutoff routes, at the drive -> filter stage (no filter until C5)");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = dcZone(PlayMode::Gated);
        z.cutoff = 1000.0;
        installOne(inst, prog, z, Sample::mono(kRate, std::vector<float>(at(1.0), 1.0f)));
        TapLog log;
        log.frames.reserve(200000);
        inst.setModulationTap(&TapLog::record, &log);
        auto cutoffOf = [&](const Zone& zz, std::vector<Note> notes, double atSeconds) {
            installOne(inst, prog, zz, Sample::mono(kRate, std::vector<float>(at(1.0), 1.0f)));
            inst.reset();
            log.frames.clear();
            render(inst, notes, at(atSeconds) + 64);
            for (const auto& f : log.frames)
                if (static_cast<double>(f.age) >= atSeconds * kRate) return f.cutoffHz;
            return -1.0;
        };
        Zone zv = z;
        zv.velToCutoff = 24.0;
        const double c1 = cutoffOf(zv, {on(0, 60, 1)}, 0.1), c64 = cutoffOf(zv, {on(0, 60, 64)}, 0.1),
                     c127 = cutoffOf(zv, {on(0, 60, 127)}, 0.1);
        note("velToCutoff +24: cutoff at velocity 1 / 64 / 127 = " + std::to_string(c1) + " / " + std::to_string(c64) +
             " / " + std::to_string(c127) + " Hz (documented: x 2^(24 v/127 / 12))");
        check("velocity -> cutoff: higher velocity, higher cutoff", c1 < c64 && c64 < c127);
        check("as documented: velocity 127 is two octaves above the base (4000 Hz +- 0.1 %)", std::fabs(c127 / 4000.0 - 1.0) < 1e-3);

        Zone zf = z;
        zf.filterEnvAmount = 12.0;
        zf.filterAttack = 0.0;
        zf.filterDecay = 0.2;
        zf.filterSustain = 0.0;
        const double e0 = cutoffOf(zf, {on(0, 60)}, 0.0), e1 = cutoffOf(zf, {on(0, 60)}, 0.1),
                     e2 = cutoffOf(zf, {on(0, 60)}, 0.3);
        note("filterEnvAmount +12, decay 0.2 s to 0: cutoff at 0 / 0.1 / 0.3 s = " + std::to_string(e0) + " / " +
             std::to_string(e1) + " / " + std::to_string(e2) + " Hz");
        check("filter envelope -> cutoff: an octave up at the peak, back to the base after the decay",
              std::fabs(e0 / 2000.0 - 1.0) < 1e-3 && e1 < e0 && e1 > e2 && std::fabs(e2 / 1000.0 - 1.0) < 1e-3);

        Zone zw = z;
        zw.modWheelToCutoff = -12.0;
        const double w0 = cutoffOf(zw, {on(0, 60)}, 0.1), w127 = cutoffOf(zw, {wheel(0, 127), on(0, 60)}, 0.1);
        note("modWheelToCutoff -12: cutoff at wheel 0 / 127 = " + std::to_string(w0) + " / " + std::to_string(w127) + " Hz");
        check("mod wheel -> cutoff: full wheel moves it by the amount (-12 st: 500 Hz)",
              std::fabs(w0 / 1000.0 - 1.0) < 1e-6 && std::fabs(w127 / 500.0 - 1.0) < 1e-3);

        Zone zl = z;
        zl.lfoToCutoff = 12.0;
        zl.lfoRate = 2.0;
        installOne(inst, prog, zl, Sample::mono(kRate, std::vector<float>(at(2.0), 1.0f)));
        inst.reset();
        log.frames.clear();
        render(inst, {on(0, 60)}, at(1.5));
        double lo = 1e9, hi = 0;
        for (const auto& f : log.frames) {
            lo = std::min(lo, f.cutoffHz);
            hi = std::max(hi, f.cutoffHz);
        }
        note("lfoToCutoff +12: cutoff swings " + std::to_string(lo) + " .. " + std::to_string(hi) + " Hz");
        check("LFO -> cutoff swings an octave each way (500 .. 2000 Hz +- 0.5 %)",
              std::fabs(lo / 500.0 - 1.0) < 5e-3 && std::fabs(hi / 2000.0 - 1.0) < 5e-3);
        inst.setModulationTap(nullptr, nullptr);
    }

    return summary("mod_tests");
}
