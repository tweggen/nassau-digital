// fx_tests.cpp - C5: drive, filter, kit bus (DESIGN §3, §4.2, §4.3).
//
// C5.1 each drive curve's output RMS at moderate input stays within +- 1 dB
//      of the input (output compensation).
// C5.2 the ladder and SVF (and the HP cascade) match nassau-dsp's golden for
//      the same input and settings, byte for byte (cmp), because they are the
//      same code driven the same way.
// C5.3 aliasing: a 10 kHz sine through full drive at the chosen oversampling
//      factor has no component above 0.5 fs - 2 kHz louder than -80 dB
//      relative to the fundamental.
// C5.4 a zone with drive 0 at note-on runs without oversampling; automating
//      its drive above 0 mid-note changes the sound but not the oversampling
//      of that voice; the next note's voice is oversampled.
// C5.5 two overlapping hits through the kit-bus drive show intermodulation
//      products absent when each hit is rendered alone and summed.
// Plus R3 (no allocation in process() over the drive, filter, oversampling
// and kit-bus paths), R8 (byte-identical across reset and block sizes with
// all of them in use) and the per-voice cost against C2.6's budget.
//
// Everything but C5.2 is measured on audio through the public API (R6).
// C5.2 drives the voice's own stage (VoiceFx, the object every voice runs)
// with nassau-dsp's golden battery's input and cutoff schedule, because a
// byte comparison needs the very same input and coefficient sequence, which
// the voice's sample reader and modulation do not hand over bit for bit.

#include "nd_edit.h"
#include "nd_instrument.h"
#include "nd_voice_fx.h"
#include "render_util.h"
#include "test_util.h"
#include "alloc_counter.h"

#include <chrono>
#include <cstring>
#include <fstream>

using namespace nd;
using namespace ndtest;

namespace {

constexpr double kRate = 48000.0;
const double kRefAmp = std::pow(10.0, DriveConst::kRefDb / 20.0) * std::sqrt(2.0);  // -12 dBFS RMS sine
const char* const kCurveNames[] = {"soft", "tube", "hard", "fold"};

std::string fmt(const char* f, double v) {
    char b[256];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

bool same(const Stereo& x, const Stereo& y) {
    return x.l.size() == y.l.size() && std::memcmp(x.l.data(), y.l.data(), x.l.size() * sizeof(float)) == 0 &&
           std::memcmp(x.r.data(), y.r.data(), x.r.size() * sizeof(float)) == 0;
}

// One zone, one mono layer panned hard left (gain 1 into the left channel),
// velocity-insensitive, no key tracking: the drive sees the sample as it is.
Zone leftZone(PlayMode mode = PlayMode::Loop) {
    Zone z = fullZone(mode, 60);
    z.velToLevel = 0.0;
    z.keyTrack = false;
    z.layers[0].pan = -1.0;
    return z;
}

uint32_t installOne(Instrument& inst, Program& prog, const Zone& z, std::shared_ptr<const Sample> s) {
    prog = Program{};
    addZone(prog, z);
    install(inst, prog);
    inst.setSample(prog.zones[0].layers[0].id, std::move(s));
    return prog.zones[0].id;
}

// ---- Spectrum of a steady tone: exact lines ------------------------------
//
// A sine of f0 at rate fs repeats every P = fs / gcd(fs, f0) samples, and so
// does everything a memoryless curve, linear filters and the oversampler make
// of it once they are settled. Its spectrum is lines at multiples of
// fs / P (2 kHz at 48 kHz for 10 kHz; 100 Hz at 44.1 kHz), and a DFT over a
// whole number of periods sees each line in one bin, with no window and no
// leakage. So each line is measured on its own, at its exact frequency, and
// "above 0.5 fs - 2 kHz" is decided per line, not per smeared bin.

// Amplitude of the line at freq over v[from, from + n) (Goertzel; n a whole
// number of periods). The Nyquist line is real: its amplitude is |X| / n.
double lineAmp(const std::vector<float>& v, size_t from, size_t n, double freq, double rate) {
    const double w = 2.0 * kPi * freq / rate, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < from + n; ++i) {
        const double s0 = v[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    const double mag = std::sqrt(re * re + im * im);
    return (std::fabs(freq - rate / 2) < 1e-9) ? mag / static_cast<double>(n) : 2.0 * mag / static_cast<double>(n);
}

struct Alias {
    double band = -300;      // loudest line above fs/2 - 2 kHz, dB re the fundamental (C5.3)
    double bandHz = 0;
    double anywhere = -300;  // loudest line that is not a harmonic of the tone (an alias), dB
    double anywhereHz = 0;
};

long long gcdll(long long a, long long b) { return b == 0 ? a : gcdll(b, a % b); }

// The lines of a steady f0 tone in v from `from`, over about one second.
Alias aliasOf(const std::vector<float>& v, size_t from, double rate, double f0) {
    const long long fs = std::llround(rate), f = std::llround(f0);
    const long long period = fs / gcdll(fs, f);
    const double spacing = rate / static_cast<double>(period);
    const size_t n = static_cast<size_t>(period * std::max<long long>(1, fs / period));
    const double fund = lineAmp(v, from, n, f0, rate);
    Alias a;
    for (int k = 1; k * spacing <= rate / 2 + 1e-9; ++k) {
        const double fk = k * spacing;
        const double d = db(lineAmp(v, from, n, fk, rate) / fund);
        if (fk > rate / 2 - 2000.0 && d > a.band) {
            a.band = d;
            a.bandHz = fk;
        }
        const double h = fk / f0;
        if (std::fabs(h - std::round(h)) > 1e-9 && d > a.anywhere) {
            a.anywhere = d;
            a.anywhereHz = fk;
        }
    }
    return a;
}

// Amplitude of a sinusoid at freq in v[from, to) (Goertzel).
double toneAmp(const std::vector<float>& v, size_t from, size_t to, double freq, double rate) {
    return lineAmp(v, from, to - from, freq, rate);
}

// A 10 kHz sine (-12 dBFS RMS) through full drive on the instrument, from the
// left channel: drive 1 at note-on (oversampled at `factor`), or drive 0 at
// note-on raised to 1 after the first block (driven at the base rate).
Alias tenKilohertz(double rate, int factor, DriveCurve curve, bool driveAtNoteOn) {
    Instrument inst;
    inst.prepare(rate);
    inst.setOversampling(factor);
    Program prog;
    Zone z = leftZone();
    z.driveCurve = curve;
    z.drive = driveAtNoteOn ? 1.0 : 0.0;
    installOne(inst, prog, z, Sample::mono(rate, sine(10000.0, rate, 1.0, kRefAmp)));
    const size_t frames = static_cast<size_t>(rate * 1.5);
    Stereo out;
    out.l.assign(frames, 0.0f);
    out.r.assign(frames, 0.0f);
    std::vector<Event> ev;
    ev.reserve(2);
    renderInto(inst, {on(0, 60)}, 256, out, ev);
    if (!driveAtNoteOn) {
        prog.zones[0].drive = 1.0;
        install(inst, prog);
    }
    Stereo rest;
    rest.l.assign(frames - 256, 0.0f);
    rest.r.assign(frames - 256, 0.0f);
    renderInto(inst, {}, static_cast<int64_t>(frames - 256), rest, ev, 256, 256);
    std::copy(rest.l.begin(), rest.l.end(), out.l.begin() + 256);
    return aliasOf(out.l, static_cast<size_t>(rate * 0.25), rate, 10000.0);
}

// The same through the voice's stage alone (VoiceFx: drive, lp24 at 20 kHz),
// for the candidates the public API does not expose (antiderivative
// antialiasing on or off).
Alias tenKilohertzStage(double rate, int factor, bool adaa, DriveCurve curve, bool driveAtNoteOn) {
    VoiceFxShared sh;
    sh.build(rate);
    sh.factor = factor;
    sh.adaa = adaa;
    VoiceFx fx;
    VoiceFxParams p;
    p.driveCurve = curve;
    p.drive = driveAtNoteOn ? 1.0 : 0.0;
    fx.start(p, sh);
    p.drive = 1.0;
    const std::vector<float> in = sine(10000.0, rate, 1.5, kRefAmp);
    std::vector<float> l(in), r(in.size(), 0.0f);
    for (size_t b = 0; b + VoiceFx::kBlock <= l.size(); b += VoiceFx::kBlock) {
        fx.control(p);
        fx.process(l.data() + b, r.data() + b, VoiceFx::kBlock);
    }
    return aliasOf(l, static_cast<size_t>(rate * 0.25), rate, 10000.0);
}

// ---- nassau-dsp's golden battery (Tests/filter_golden.cpp there) ------------

constexpr int kGoldenFrames = 24000;

std::vector<float> goldenInput() {
    Xorshift32 rng(0xC0FFEEu);
    std::vector<float> in(kGoldenFrames);
    double saw = 0.0;
    for (int i = 0; i < kGoldenFrames; ++i) {
        saw += 110.0 / 48000.0;
        if (saw >= 1.0) saw -= 1.0;
        in[static_cast<size_t>(i)] = static_cast<float>(0.3 * rng.nextBipolar() + 0.4 * (2.0 * saw - 1.0));
    }
    return in;
}

double goldenCutoff(int b, int blocks) {
    const double t = static_cast<double>(b) / static_cast<double>(blocks - 1);
    return 80.0 * std::pow(12000.0 / 80.0, t);
}

bool readGolden(const char* path, std::vector<float>& v) {
    std::ifstream f(path, std::ios::binary);
    uint32_t n = 0;
    if (!f.read(reinterpret_cast<char*>(&n), sizeof n)) return false;
    v.resize(n);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * sizeof(float))));
}

// The voice's stage (drive 0: no oversampling, drive bypassed) over the
// battery's input and cutoff schedule, in uneven chunks inside each control
// block as a voice splits them at host-block and event boundaries.
void stageOverGolden(FilterType type, double resPercent, const std::vector<float>& in, std::vector<float>& outL,
                     std::vector<float>& outR, const VoiceFxShared& shared) {
    VoiceFx fx;
    VoiceFxParams p;
    p.filter = type;
    p.resonance = resPercent / 100.0;
    p.drive = 0.0;
    fx.start(p, shared);
    outL.assign(in.begin(), in.end());
    outR.assign(in.begin(), in.end());
    const int blocks = kGoldenFrames / VoiceFx::kBlock;
    for (int b = 0; b < blocks; ++b) {
        p.cutoffHz = goldenCutoff(b, blocks);
        fx.control(p);
        const int split = 1 + (b * 7) % (VoiceFx::kBlock - 1);
        float* l = outL.data() + b * VoiceFx::kBlock;
        float* r = outR.data() + b * VoiceFx::kBlock;
        fx.process(l, r, split);
        fx.process(l + split, r + split, VoiceFx::kBlock - split);
    }
}

// ---- cost ---------------------------------------------------------------

// ns per output frame for one sounding voice (looping, so it never ends),
// process() overhead subtracted, best of 5: C2.6's harness (interp_tests).
double costNs(int layers, double drive, int factor, bool modulatedCutoff) {
    Instrument inst;
    inst.prepare(48000.0);
    inst.setOversampling(factor);
    Program prog;
    Zone z = fullZone(PlayMode::Loop, 60);
    z.layers.resize(static_cast<size_t>(layers));
    z.drive = drive;
    if (modulatedCutoff) {
        z.cutoff = 2000.0;
        z.lfoToCutoff = 12.0;  // the cutoff moves every control block: the per-sample g ramp runs
    }
    addZone(prog, z);
    install(inst, prog);
    const double srcRate = 44100.0;
    const auto src = noise(static_cast<size_t>(srcRate * 4), 11);
    for (int j = 0; j < layers; ++j)
        inst.setSample(prog.zones[0].layers[static_cast<size_t>(j)].id, Sample::stereo(srcRate, src, noise(src.size(), 12)));
    std::vector<float> l(256), r(256);
    const int blocks = static_cast<int>(10.0 * 48000.0 / 256);
    double best = 1e30, empty = 1e30;
    volatile float sink = 0.0f;
    for (int rep = 0; rep < 5; ++rep) {
        inst.reset();
        Event ev{0, Event::NoteOn, 67, 100};
        inst.process(0, &ev, 1, l.data(), r.data(), 256);
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 1; b < blocks; ++b) {
            inst.process(static_cast<int64_t>(b) * 256, nullptr, 0, l.data(), r.data(), 256);
            sink = sink + l[0];
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / ((blocks - 1) * 256.0));
        inst.reset();
        const auto t2 = std::chrono::steady_clock::now();
        for (int b = 1; b < blocks; ++b) inst.process(static_cast<int64_t>(b) * 256, nullptr, 0, l.data(), r.data(), 256);
        const auto t3 = std::chrono::steady_clock::now();
        empty = std::min(empty, std::chrono::duration<double, std::nano>(t3 - t2).count() / ((blocks - 1) * 256.0));
    }
    return best - empty;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("=== NassauDigital core: drive, filter, kit bus (C5) ===\n");
    const bool bench = (argc > 1 && std::string(argv[1]) == "--bench");

    // ---------------------------------------------------------------- C5.1 ---
    group("C5.1 each drive curve keeps a moderate (-12 dBFS RMS) sine within +- 1 dB");
    {
        // The curve's output against its input: the same 1 kHz note with the
        // drive at 0 and at each amount, the filter set out of the way (hp12
        // at 20 Hz: there is no "off"). With the default lp24 at 20 kHz the
        // oversampled voice's filter takes some of a bright curve's top end
        // (shown after the check).
        double worst = 0;
        for (int c = 0; c < 4; ++c) {
            std::string row = std::string(kCurveNames[c]) + ":";
            for (double amount : {0.25, 0.5, 0.75, 1.0}) {
                double rmsOf[2];
                for (int k = 0; k < 2; ++k) {
                    Instrument inst;
                    inst.prepare(kRate);
                    Program prog;
                    Zone z = leftZone();
                    z.filter = FilterType::Hp12;
                    z.cutoff = 20.0;
                    z.driveCurve = static_cast<DriveCurve>(c);
                    z.drive = (k == 0) ? 0.0 : amount;
                    installOne(inst, prog, z, Sample::mono(kRate, sine(1000.0, kRate, 1.0, kRefAmp)));
                    const Stereo a = render(inst, {on(0, 60)}, 24000);
                    rmsOf[k] = rms(a.l, 4800, 24000);
                }
                const double d = db(rmsOf[1] / rmsOf[0]);
                worst = std::max(worst, std::fabs(d));
                row += fmt("  drive %.2f ", amount) + fmt("%+.3f dB", d);
            }
            note(row);
        }
        check("every curve at drive 0.25 / 0.5 / 0.75 / 1: output RMS within +- 1 dB of the input (worst " +
                  fmt("%.3f dB)", worst),
              worst <= 1.0);
        {
            std::string row = "with the default lp24 at 20 kHz, drive 1:";
            for (int c = 0; c < 4; ++c) {
                double rmsOf[2];
                for (int k = 0; k < 2; ++k) {
                    Instrument inst;
                    inst.prepare(kRate);
                    Program prog;
                    Zone z = leftZone();
                    z.driveCurve = static_cast<DriveCurve>(c);
                    z.drive = k == 0 ? 0.0 : 1.0;
                    installOne(inst, prog, z, Sample::mono(kRate, sine(1000.0, kRate, 1.0, kRefAmp)));
                    rmsOf[k] = rms(render(inst, {on(0, 60)}, 24000).l, 4800, 24000);
                }
                row += std::string("  ") + kCurveNames[c] + fmt(" %+.2f dB", db(rmsOf[1] / rmsOf[0]));
            }
            note(row);
        }
        // How the compensation behaves around the reference level (not part
        // of the AC: "moderate input" is the reference).
        for (double level : {-18.0, -6.0}) {
            std::string row = fmt("at %.0f dBFS RMS, drive 1:", level);
            for (int c = 0; c < 4; ++c) {
                double rmsOf[2];
                for (int k = 0; k < 2; ++k) {
                    Instrument inst;
                    inst.prepare(kRate);
                    Program prog;
                    Zone z = leftZone();
                    z.filter = FilterType::Hp12;
                    z.cutoff = 20.0;
                    z.driveCurve = static_cast<DriveCurve>(c);
                    z.drive = k == 0 ? 0.0 : 1.0;
                    installOne(inst, prog, z,
                               Sample::mono(kRate, sine(1000.0, kRate, 1.0, std::pow(10.0, level / 20.0) * std::sqrt(2.0))));
                    rmsOf[k] = rms(render(inst, {on(0, 60)}, 24000).l, 4800, 24000);
                }
                row += std::string("  ") + kCurveNames[c] + fmt(" %+.2f dB", db(rmsOf[1] / rmsOf[0]));
            }
            note(row);
        }
    }

    // ---------------------------------------------------------------- C5.2 ---
    group("C5.2 the voice's filters equal nassau-dsp's golden (cmp)");
    {
        std::vector<float> golden;
        const bool read = readGolden(ND_DSP_FILTER_GOLDEN, golden);
        check(std::string("nassau-dsp's golden read: ") + ND_DSP_FILTER_GOLDEN, read && golden.size() == 11u * kGoldenFrames);
        if (read && golden.size() == 11u * kGoldenFrames) {
            VoiceFxShared shared;
            shared.build(48000.0);
            const std::vector<float> in = goldenInput();
            struct Case {
                const char* name;
                FilterType type;
                double res;
                int slot;  // the battery's case index
            } cases[] = {{"lp24 ladder, resonance 0 %", FilterType::Lp24, 0.0, 0},
                         {"lp24 ladder, resonance 50 %", FilterType::Lp24, 50.0, 1},
                         {"lp24 ladder, resonance 100 %", FilterType::Lp24, 100.0, 2},
                         {"lp12 SVF, resonance 0 %", FilterType::Lp12, 0.0, 3},
                         {"lp12 SVF, resonance 50 %", FilterType::Lp12, 50.0, 4},
                         {"lp12 SVF, resonance 100 %", FilterType::Lp12, 100.0, 5},
                         {"hp12 cascade", FilterType::Hp12, 0.0, 6},
                         {"hp24 cascade", FilterType::Hp24, 0.0, 7}};
            for (const Case& c : cases) {
                std::vector<float> l, r;
                stageOverGolden(c.type, c.res, in, l, r, shared);
                const float* g = golden.data() + static_cast<size_t>(c.slot) * kGoldenFrames;
                const bool eqL = std::memcmp(l.data(), g, kGoldenFrames * sizeof(float)) == 0;
                const bool eqR = std::memcmp(r.data(), g, kGoldenFrames * sizeof(float)) == 0;
                size_t diff = 0;
                double maxErr = 0;
                for (int i = 0; i < kGoldenFrames; ++i) {
                    if (std::memcmp(&l[static_cast<size_t>(i)], &g[i], sizeof(float)) != 0) ++diff;
                    maxErr = std::max(maxErr, std::fabs(static_cast<double>(l[static_cast<size_t>(i)]) - g[i]));
                }
                note(std::string(c.name) + ": " + std::to_string(kGoldenFrames - static_cast<int>(diff)) + "/" +
                     std::to_string(kGoldenFrames) + " samples bit-exact, max abs error " + fmt("%.3e", maxErr));
                check(std::string(c.name) + ": both channels byte-identical to the golden", eqL && eqR);
            }
        }
    }

    group("C5.2 (audio) each filter type's slope through the instrument");
    {
        // A sine an octave pair away from a 1 kHz cutoff, through each type:
        // the attenuation two octaves into the stopband.
        struct T {
            FilterType type;
            const char* name;
            double probe;
            double expectDb;  // the bilinear one-pole cascade's magnitude there, k = 0
        } types[] = {{FilterType::Lp24, "lp24", 4000.0, 0}, {FilterType::Lp12, "lp12", 4000.0, 0},
                     {FilterType::Hp12, "hp12", 250.0, 0}, {FilterType::Hp24, "hp24", 250.0, 0}};
        for (T& t : types) {
            const double gc = std::tan(kPi * 1000.0 / kRate), gp = std::tan(kPi * t.probe / kRate);
            const double r = (t.type == FilterType::Hp12 || t.type == FilterType::Hp24) ? gc / gp : gp / gc;
            const int poles = (t.type == FilterType::Lp24 || t.type == FilterType::Hp24) ? 4 : 2;
            // A one-pole TPT stage is the bilinear one-pole; the SVF at
            // resonance 0 (R = 1) is two coincident real poles, the same.
            t.expectDb = -10.0 * poles * std::log10(1.0 + r * r);
        }
        for (const T& t : types) {
            Instrument inst;
            inst.prepare(kRate);
            Program prog;
            Zone z = leftZone();
            z.filter = t.type;
            z.cutoff = 1000.0;
            installOne(inst, prog, z, Sample::mono(kRate, sine(t.probe, kRate, 1.0, 0.5)));
            const Stereo a = render(inst, {on(0, 60)}, 24000);
            const double got = db(toneAmp(a.l, 4800, 24000, t.probe, kRate) / 0.5);
            note(std::string(t.name) + fmt(" at %.0f Hz (cutoff 1 kHz): ", t.probe) + fmt("%.2f dB", got) +
                 fmt(" (bilinear cascade: %.2f dB)", t.expectDb));
            check(std::string(t.name) + ": two octaves into the stopband as the cascade predicts, +- 0.1 dB",
                  std::fabs(got - t.expectDb) < 0.1);
        }
    }

    // ---------------------------------------------------------------- C5.3 ---
    group("C5.3 aliasing: a 10 kHz sine (-12 dBFS RMS) through full drive, lp24 at 20 kHz");
    {
        // The candidates, through the voice's stage: no oversampling (drive
        // 0 at note-on, raised after), 2x and 4x, each with and without the
        // antiderivative method. Each cell: the loudest line above fs/2 - 2
        // kHz (the AC), then in brackets the loudest alias anywhere.
        std::printf("    %-7s %-5s %-21s %-21s %-21s %-21s %-21s\n", "rate", "curve", "none", "2x", "2x antideriv.",
                    "4x", "4x antideriv.");
        auto cell = [](const Alias& a) {
            char b[64];
            std::snprintf(b, sizeof b, "%6.1f (%6.1f@%5.0f)", a.band, a.anywhere, a.anywhereHz);
            return std::string(b);
        };
        for (double rate : {48000.0, 44100.0}) {
            for (int c = 0; c < 4; ++c) {
                const DriveCurve dc = static_cast<DriveCurve>(c);
                std::printf("    %-7.0f %-5s %-21s %-21s %-21s %-21s %-21s\n", rate, kCurveNames[c],
                            cell(tenKilohertzStage(rate, 4, true, dc, false)).c_str(),
                            cell(tenKilohertzStage(rate, 2, false, dc, true)).c_str(),
                            cell(tenKilohertzStage(rate, 2, true, dc, true)).c_str(),
                            cell(tenKilohertzStage(rate, 4, false, dc, true)).c_str(),
                            cell(tenKilohertzStage(rate, 4, true, dc, true)).c_str());
            }
        }
        // The AC, through the instrument at its defaults.
        Instrument probe;
        probe.prepare(kRate);
        double worst = -300;
        std::string row;
        for (double rate : {48000.0, 44100.0}) {
            for (int c = 0; c < 4; ++c) {
                const Alias a = tenKilohertz(rate, probe.oversampling(), static_cast<DriveCurve>(c), true);
                worst = std::max(worst, a.band);
                row += fmt("  %.1f/", rate / 1000.0) + kCurveNames[c] + fmt(" %.1f", a.band) + fmt(" (@%.0f)", a.bandHz);
            }
        }
        note("through the instrument at its defaults (" + std::to_string(probe.oversampling()) +
             "x), loudest line above fs/2 - 2 kHz, dB:" + row);
        note("at 48 kHz the lines of a 10 kHz tone lie on a 2 kHz grid, so the only one above 22 kHz is 24 kHz,"
             " which a symmetric curve cannot make: 48 kHz alone would pass without oversampling (first column)");
        check("at the chosen factor, every curve, at 48 and 44.1 kHz: no line above fs/2 - 2 kHz louder than -80 dB (worst " +
                  fmt("%.1f dB)", worst),
              worst < -80.0);
    }

    group("C5.4 oversampling is decided at note-on and held for the voice's life");
    {
        struct TapLog {
            int lastFactor = 0;
            double lastDrive = -1;
            static void record(void* user, const ModulationFrame& f) {
                auto* t = static_cast<TapLog*>(user);
                t->lastFactor = f.oversampling;
                t->lastDrive = f.drive;
            }
        } log;
        const double rate = 44100.0;  // 10 kHz's aliases spread over the band at 44.1 kHz
        Instrument inst;
        inst.prepare(rate);
        inst.setModulationTap(&TapLog::record, &log);
        Program prog;
        Zone z = leftZone();
        z.driveCurve = DriveCurve::Soft;
        z.drive = 0.0;
        installOne(inst, prog, z, Sample::mono(rate, sine(10000.0, rate, 1.0, kRefAmp)));
        const int64_t seg = static_cast<int64_t>(rate * 1.5);
        std::vector<Event> ev;
        ev.reserve(4);
        Stereo a;
        a.l.assign(static_cast<size_t>(seg), 0.0f);
        a.r.assign(static_cast<size_t>(seg), 0.0f);
        renderInto(inst, {on(0, 60)}, seg, a, ev);
        const int factorBefore = log.lastFactor;
        // Automate drive to 1 mid-note: a new program snapshot, as qbx sends.
        prog.zones[0].drive = 1.0;
        install(inst, prog);
        Stereo b = a;
        renderInto(inst, {}, seg, b, ev, 256, seg);
        const int factorAfter = log.lastFactor;
        const double driveAfter = log.lastDrive;
        // The next note, with drive 1 at its note-on.
        Stereo c = a;
        renderInto(inst, {off(2 * seg, 60), on(2 * seg + 4410, 60)}, seg, c, ev, 256, 2 * seg);
        const int factorNext = log.lastFactor;
        inst.setModulationTap(nullptr, nullptr);

        const size_t from = static_cast<size_t>(0.25 * rate);
        const Alias before = aliasOf(a.l, from, rate, 10000.0);
        const Alias driven = aliasOf(b.l, from, rate, 10000.0);
        const Alias next = aliasOf(c.l, from, rate, 10000.0);
        const double h3 = lineAmp(b.l, from, 44100, 30000.0, rate) / lineAmp(b.l, from, 44100, 10000.0, rate);
        const double h3a = lineAmp(a.l, from, 44100, 30000.0, rate) / lineAmp(a.l, from, 44100, 10000.0, rate);
        note("voice 1, drive 0 at note-on: oversampling " + std::to_string(factorBefore) + "x, loudest alias " +
             fmt("%.1f dB", before.anywhere) + fmt(" at %.0f Hz", before.anywhereHz));
        note("voice 1 after drive -> 1 mid-note: oversampling " + std::to_string(factorAfter) + "x (drive " +
             fmt("%.2f", driveAfter) + "), loudest alias " + fmt("%.1f dB", driven.anywhere) +
             fmt(" at %.0f Hz", driven.anywhereHz) + fmt(" (the 3rd harmonic, folded to 14.1 kHz: %.1f dB", db(h3)) +
             fmt(", before %.1f dB)", db(h3a)));
        note("voice 2, drive 1 at note-on: oversampling " + std::to_string(factorNext) + "x, loudest alias " +
             fmt("%.1f dB", next.anywhere) + fmt(" at %.0f Hz", next.anywhereHz));
        check("drive 0 at note-on: the voice is not oversampled", factorBefore == 1);
        check("automating drive mid-note changes the sound: the drive's harmonics appear (14.1 kHz up by 60 dB or more)",
              driveAfter == 1.0 && db(h3) > db(h3a) + 60.0);
        check("...but not that voice's oversampling: still 1x, and driven at the base rate (aliases above -40 dB)",
              factorAfter == 1 && driven.anywhere > -40.0);
        check("the next note's voice is oversampled (" + std::to_string(inst.oversampling()) +
                  "x) and its loudest alias is 30 dB or more below the base-rate voice's",
              factorNext == inst.oversampling() && next.anywhere < driven.anywhere - 30.0);
    }

    group("C5.4 an oversampled voice lines up with one that is not (latency compensated)");
    {
        // A click at frame 0 of the sample: where it lands in the output.
        for (int factor : {2, 4}) {
            size_t at[2];
            for (int k = 0; k < 2; ++k) {
                Instrument inst;
                inst.prepare(kRate);
                inst.setOversampling(factor);
                Program prog;
                Zone z = leftZone(PlayMode::Gated);
                z.drive = (k == 0) ? 0.0 : 1e-9;  // no audible drive: just the oversampled path
                z.filter = FilterType::Hp12;
                z.cutoff = 20.0;
                std::vector<float> click(4800, 0.0f);
                click[0] = 0.5f;
                installOne(inst, prog, z, Sample::mono(kRate, click));
                const Stereo a = render(inst, {on(100, 60)}, 2000);
                size_t best = 0;
                for (size_t i = 0; i < a.l.size(); ++i)
                    if (std::fabs(a.l[i]) > std::fabs(a.l[best])) best = i;
                at[k] = best;
            }
            note(std::to_string(factor) + "x: the click peaks at frame " + std::to_string(at[1]) +
                 " oversampled, " + std::to_string(at[0]) + " not (note-on at 100; chain latency " +
                 std::to_string([&] {
                     Instrument i;
                     i.prepare(kRate);
                     i.setOversampling(factor);
                     return i.oversamplingLatency();
                 }()) + " frames)");
            check(std::to_string(factor) + "x: the oversampled voice's click peaks on the same frame", at[0] == at[1]);
        }
        // A short burst that ends: the oversampled voice still plays it to
        // its end, though its layer ended latency() frames before the output
        // got there (the voice outlives its layers until the stage's tail
        // is out), and the burst comes out as the plain voice plays it.
        std::vector<float> burst(240);
        for (size_t i = 0; i < burst.size(); ++i)
            burst[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * i / kRate) *
                                          std::sin(kPi * i / static_cast<double>(burst.size())));
        Stereo outs[2];
        for (int k = 0; k < 2; ++k) {
            Instrument inst;
            inst.prepare(kRate);
            Program prog;
            Zone z = leftZone(PlayMode::OneShot);
            z.drive = (k == 0) ? 0.0 : 1e-9;
            z.filter = FilterType::Hp12;
            z.cutoff = 20.0;
            installOne(inst, prog, z, Sample::mono(kRate, burst));
            outs[k] = render(inst, {on(100, 60)}, 1000);
        }
        double maxDiff = 0;
        for (size_t i = 0; i < 1000; ++i) maxDiff = std::max(maxDiff, std::fabs(double(outs[1].l[i]) - outs[0].l[i]));
        const double e0 = rms(outs[0].l, 0, 1000), e1 = rms(outs[1].l, 0, 1000);
        note(fmt("a 240-frame burst, oneshot: RMS %.6f oversampled, ", e1) + fmt("%.6f plain; ", e0) +
             fmt("largest difference %.2e", maxDiff) + fmt(" (%.1f dB re the burst's peak)", db(maxDiff / 0.5)));
        check("an oversampled voice plays a burst to its end, as the plain voice does (RMS within 0.1 dB, every frame "
              "within -60 dB)",
              std::fabs(db(e1 / e0)) < 0.1 && maxDiff < 0.5e-3);
    }

    // ---------------------------------------------------------------- C5.5 ---
    group("C5.5 the kit-bus drive: overlapping hits intermodulate, each alone does not");
    {
        // Two zones on two keys: 1000 Hz and 1500 Hz. Odd-order products of
        // the (symmetric) bus curve land at 2*1000 - 1500 = 500 Hz and
        // 2*1500 - 1000 = 2000 Hz, where neither tone has a harmonic.
        auto renderHits = [](double busDrive, bool a, bool b) {
            Instrument inst;
            inst.prepare(kRate);
            Program prog;
            prog.busDrive = busDrive;
            Zone za = fullZone(PlayMode::Loop, 60);
            za.keyLo = za.keyHi = 60;
            za.keyTrack = false;
            za.velToLevel = 0.0;
            Zone zb = za;
            zb.keyLo = zb.keyHi = 62;
            addZone(prog, za);
            addZone(prog, zb);
            install(inst, prog);
            inst.setSample(prog.zones[0].layers[0].id, Sample::mono(kRate, sine(1000.0, kRate, 1.0, 0.4)));
            inst.setSample(prog.zones[1].layers[0].id, Sample::mono(kRate, sine(1500.0, kRate, 1.0, 0.4)));
            std::vector<Note> notes;
            if (a) notes.push_back(on(0, 60));
            if (b) notes.push_back(on(0, 62));
            return render(inst, notes, 48000);
        };
        for (double drive : {0.5, 1.0}) {
            const Stereo both = renderHits(drive, true, true);
            const Stereo a = renderHits(drive, true, false), b = renderHits(drive, false, true);
            std::vector<float> sum(both.l.size());
            for (size_t i = 0; i < sum.size(); ++i) sum[i] = a.l[i] + b.l[i];
            const size_t from = 9600, to = 9600 + 38400;  // whole cycles of 500 Hz and 2 kHz
            const double ref = toneAmp(both.l, from, to, 1000.0, kRate);
            const double im500 = db(toneAmp(both.l, from, to, 500.0, kRate) / ref);
            const double im2k = db(toneAmp(both.l, from, to, 2000.0, kRate) / ref);
            const double al500 = db(toneAmp(sum, from, to, 500.0, kRate) / ref);
            const double al2k = db(toneAmp(sum, from, to, 2000.0, kRate) / ref);
            note(fmt("busDrive %.1f: together 500 Hz ", drive) + fmt("%.1f dB, 2 kHz ", im500) + fmt("%.1f dB", im2k) +
                 fmt("; alone and summed 500 Hz %.1f dB, ", al500) + fmt("2 kHz %.1f dB (re the 1 kHz tone)", al2k));
            check(fmt("busDrive %.1f: intermodulation at 500 Hz and 2 kHz together (above -60 dB)", drive),
                  im500 > -60.0 && im2k > -60.0);
            check(fmt("busDrive %.1f: absent when each hit is rendered alone and summed (below -120 dB)", drive),
                  al500 < -120.0 && al2k < -120.0);
        }
        // busDrive 0 is an exact bypass: together equals alone + alone up to
        // the order of float sums (no intermodulation at all).
        const Stereo both = renderHits(0.0, true, true);
        const double im0 = db(toneAmp(both.l, 9600, 48000, 500.0, kRate) / toneAmp(both.l, 9600, 48000, 1000.0, kRate));
        note(fmt("busDrive 0: 500 Hz in the overlapping render %.1f dB", im0));
        check("busDrive 0: no intermodulation (below -120 dB)", im0 < -120.0);
    }

    group("C5.5 the kit bus switches its oversampling on and off without a gap");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = fullZone(PlayMode::Loop, 60);
        z.velToLevel = 0.0;
        addZone(prog, z);
        install(inst, prog);
        inst.setSample(prog.zones[0].layers[0].id, Sample::mono(kRate, sine(220.0, kRate, 1.0, 0.3)));
        Stereo out;
        out.l.assign(48000, 0.0f);
        out.r.assign(48000, 0.0f);
        std::vector<Event> ev;
        ev.reserve(2);
        const bool off0 = inst.kitBusOversampling();
        renderInto(inst, {on(0, 60)}, 12000, out, ev);
        prog.busDrive = 0.3;
        install(inst, prog);
        Stereo part;
        part.l.assign(12000, 0.0f);
        part.r.assign(12000, 0.0f);
        renderInto(inst, {}, 12000, part, ev, 256, 12000);
        const bool on1 = inst.kitBusOversampling();
        std::copy(part.l.begin(), part.l.end(), out.l.begin() + 12000);
        prog.busDrive = 0.0;
        install(inst, prog);
        renderInto(inst, {}, 12000, part, ev, 256, 24000);
        std::copy(part.l.begin(), part.l.end(), out.l.begin() + 24000);
        const bool off2 = inst.kitBusOversampling();
        // The largest sample-to-sample step around each switch, against the
        // tone's own largest step.
        double own = 0, sw1 = 0, sw2 = 0, minRms = 1;
        for (size_t i = 2000; i < 11000; ++i) own = std::max(own, std::fabs(double(out.l[i]) - out.l[i - 1]));
        for (size_t i = 11900; i < 12400; ++i) sw1 = std::max(sw1, std::fabs(double(out.l[i]) - out.l[i - 1]));
        for (size_t i = 23900; i < 24400; ++i) sw2 = std::max(sw2, std::fabs(double(out.l[i]) - out.l[i - 1]));
        for (size_t i = 11000; i + 480 < 26000; i += 120) minRms = std::min(minRms, rms(out.l, i, i + 480));
        note(fmt("largest step: the tone's own %.5f, ", own) + fmt("switching on %.5f, ", sw1) + fmt("off %.5f; ", sw2) +
             fmt("lowest 10 ms RMS across both switches %.4f", minRms) + fmt(" (tone %.4f)", 0.3 * std::cos(kPi / 4) / std::sqrt(2.0)));
        check("the bus oversamples only while busDrive > 0", !off0 && on1 && !off2);
        check("no click switching on or off (no step above 1.5x the tone's own) and no dropout (RMS stays above half)",
              sw1 < 1.5 * own && sw2 < 1.5 * own && minRms > 0.5 * 0.3 * std::cos(kPi / 4) / std::sqrt(2.0));
    }

    // ------------------------------------------------------------------ R8 ---
    group("R8 reset, the same notes, the same bytes, with drive, filters, oversampling and the kit bus");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        prog.busDrive = 0.4;
        Zone z = fullZone(PlayMode::Loop, 60);
        z.layers[0].loopStart = 2000;
        z.drive = 0.6;
        z.driveCurve = DriveCurve::Tube;
        z.filter = FilterType::Lp24;
        z.cutoff = 800.0;
        z.resonance = 0.7;
        z.filterEnvAmount = 36.0;
        z.filterDecay = 0.3;
        z.filterSustain = 0.2;
        z.lfoToCutoff = 5.0;
        z.release = 0.1;
        Zone z2 = z;
        z2.keyLo = 72;
        z.keyHi = 71;
        z2.drive = 0.0;
        z2.filter = FilterType::Lp12;
        z2.driveCurve = DriveCurve::Fold;
        addZone(prog, z);
        addZone(prog, z2);
        install(inst, prog);
        const auto smp = Sample::mono(44100.0, noise(30000, 3));
        inst.setSample(prog.zones[0].layers[0].id, smp);
        inst.setSample(prog.zones[1].layers[0].id, smp);
        const std::vector<Note> notes = {on(0, 60, 100), on(1000, 76, 90), off(9000, 60), on(12345, 67, 30),
                                         off(30000, 76), off(40000, 67), on(41000, 48), wheel(42000, 90)};
        // Halfway, the program changes (the zones' drive and the bus drive
        // automated) at frame 9472 = 256 * 37, a block boundary for both
        // block sizes, so the change lands on the same frame in both.
        Program later = prog;
        later.busDrive = 0.7;
        for (auto& zz : later.zones) zz.drive = 0.9;
        auto run = [&](int block) {
            inst.reset();
            install(inst, prog);
            Stereo out;
            out.l.assign(60000, 0.0f);
            out.r.assign(60000, 0.0f);
            std::vector<Event> ev;
            ev.reserve(notes.size());
            renderInto(inst, notes, 9472, out, ev, block);
            install(inst, later);
            Stereo rest;
            rest.l.assign(60000 - 9472, 0.0f);
            rest.r.assign(60000 - 9472, 0.0f);
            renderInto(inst, notes, 60000 - 9472, rest, ev, block, 9472);
            std::copy(rest.l.begin(), rest.l.end(), out.l.begin() + 9472);
            std::copy(rest.r.begin(), rest.r.end(), out.r.begin() + 9472);
            return out;
        };
        const Stereo a = run(256);
        const Stereo b = run(256);
        const Stereo c = run(37);
        check("two runs after reset are byte-identical (cmp)", same(a, b));
        check("the bytes do not depend on the block size (256 vs 37)", same(a, c));
        check("...and the render is not silence", rms(a.l, 0, 60000) > 0.01);
    }

    // ------------------------------------------------------------------ R3 ---
    group("R3 no allocation in process(): drive, every filter, oversampling at 2x and 4x, kit bus on and off");
    {
        long long total = 0;
        int maxVoices = 0;
        for (int factor : {2, 4}) {
            Instrument inst;
            inst.prepare(kRate);
            inst.setOversampling(factor);
            Program prog;
            prog.polyphony = 16;
            for (int t = 0; t < 4; ++t) {
                Zone z = fullZone(PlayMode::Loop, 60);
                z.keyLo = 36 + 12 * t;
                z.keyHi = z.keyLo + 11;
                z.layers[0].loopStart = 100;
                z.filter = static_cast<FilterType>(t);
                z.cutoff = 1500.0;
                z.resonance = 0.5;
                z.filterEnvAmount = 24.0;
                z.filterDecay = 0.05;
                z.filterSustain = 0.0;
                z.drive = (t % 2) ? 0.7 : 0.0;
                z.driveCurve = static_cast<DriveCurve>(t);
                addZone(prog, z);
            }
            const auto programs = std::make_shared<const Program>(prog);
            Program busOn = prog;
            busOn.busDrive = 0.5;
            const auto programBus = std::make_shared<const Program>(busOn);
            Program driveUp = prog;
            for (auto& z : driveUp.zones) z.drive = 1.0;
            const auto programDrive = std::make_shared<const Program>(driveUp);
            inst.setProgram(programs);
            for (const Zone& z : prog.zones) inst.setSample(z.layers[0].id, Sample::mono(kRate, noise(4000, 9)));
            std::vector<Event> ev(4);
            std::vector<float> l(256), r(256);
            const long long before = newCount().load();
            for (int b = 0; b < 1500; ++b) {
                // Program swaps run between blocks, as in qbx, on the host's
                // side, and allocate (the compiled program): counted apart.
                const long long pause = newCount().load();
                if (b % 300 == 100) inst.setProgram(programBus);
                if (b % 300 == 200) inst.setProgram(programDrive);
                if (b % 300 == 0) inst.setProgram(programs);
                const long long setterAllocs = newCount().load() - pause;
                int ne = 0;
                if (b % 3 == 0) ev[static_cast<size_t>(ne++)] = Event{0, Event::NoteOn, static_cast<uint8_t>(36 + (b * 7) % 48), 100};
                if (b % 5 == 0) ev[static_cast<size_t>(ne++)] = Event{10, Event::NoteOff, static_cast<uint8_t>(36 + ((b - 20) * 7 + 480) % 48), 0};
                inst.process(static_cast<int64_t>(b) * 256, ev.data(), ne, l.data(), r.data(), 256);
                total -= setterAllocs;
                maxVoices = std::max(maxVoices, inst.activeVoices());
            }
            total += newCount().load() - before;
        }
        note("1500 blocks at each of 2x and 4x, four filter types, oversampled and plain voices, drive automated, the kit "
             "bus switched on and off, stealing (up to " + std::to_string(maxVoices) + " voices): operator new in process() " +
             std::to_string(total) + " times");
        check("zero allocations in process()", total == 0);
    }

    // ---------------------------------------------------------------- cost ---
    group("Per-voice cost, ns per output frame (C2.6's case: stereo, 44.1 kHz source, +7 st, looping)");
    {
#ifdef NDEBUG
        const bool release = true;
#else
        const bool release = false;
#endif
        if (!release) note("NOTE: not a Release build; these numbers are not the ones to record.");
        if (!bench) {
            note("run `fx_tests --bench` for the numbers (skipped in the suite: timing is not a pass/fail matter here)");
        } else {
            struct C {
                const char* name;
                int layers;
                double drive;
                int factor;
                bool mod;
            } cases[] = {{"1 layer, drive 0, static cutoff", 1, 0.0, 4, false},
                         {"1 layer, drive 0, cutoff moving", 1, 0.0, 4, true},
                         {"1 layer, drive 0.5, 2x", 1, 0.5, 2, false},
                         {"1 layer, drive 0.5, 4x", 1, 0.5, 4, false},
                         {"1 layer, drive 0.5, 4x, cutoff moving", 1, 0.5, 4, true},
                         {"4 layers, drive 0, static cutoff", 4, 0.0, 4, false},
                         {"4 layers, drive 0.5, 2x", 4, 0.5, 2, false},
                         {"4 layers, drive 0.5, 4x", 4, 0.5, 4, false}};
            for (const C& c : cases)
                note(std::string(c.name) + ": " + fmt("%.1f ns", costNs(c.layers, c.drive, c.factor, c.mod)));
        }
    }

    return summary("fx_tests");
}
