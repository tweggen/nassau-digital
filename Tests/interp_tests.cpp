// interp_tests.cpp - C2's measured choices: the interpolator (DESIGN §3) and
// the per-voice CPU cost (C2.6, DESIGN §13).
//
// Both interpolators are driven through the public API (a sine sample, one
// voice) and measured the same way:
//
//   quality  the output is fitted (least squares) with a sine at the expected
//            frequency; "SNR" is the fit's power over the residual's, so it
//            counts imaging, aliasing and interpolation error, and "gain" is
//            the fitted amplitude over the ideal one (passband droop).
//   cost     wall-clock time to render 20 s of one sounding voice, best of 5,
//            in ns per output frame.
//
// The cost is REPORTED, not asserted: a wall-clock bound measures the machine
// and its load, not the code. The numbers recorded in docs/GATES.md C2 come
// from this test's output in a Release build.

#include "nd_edit.h"
#include "nd_instrument.h"
#include "render_util.h"
#include "test_util.h"

#include <chrono>

using namespace nd;
using namespace ndtest;

namespace {

constexpr double kOut = 48000.0;

struct Quality {
    double snrDb, gainDb;
};

// One mono layer at root 60 playing a sine of `freq` Hz recorded at `srcRate`,
// on `key`; fits the output against the expected sine.
Quality measure(Interpolator interp, double srcRate, double freq, int key) {
    Instrument inst;
    inst.prepare(kOut);
    inst.setInterpolator(interp);
    Program prog;
    Zone z = fullZone(PlayMode::Gated, 60);
    addZone(prog, z);
    install(inst, prog);
    inst.setSample(prog.zones[0].layers[0].id, Sample::mono(srcRate, sine(freq, srcRate, 2.0, 0.5)));
    const int64_t n = 48000;
    const Stereo a = render(inst, {on(0, key)}, n);
    const double ratio = std::exp2((key - 60) / 12.0) * srcRate / kOut;
    const double fOut = freq * ratio * kOut / srcRate;  // Hz at the output
    // Least-squares fit of a*sin + b*cos over a window away from the edges.
    const size_t from = 4800, to = 4800 + 38400;
    double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
    for (size_t i = from; i < to; ++i) {
        const double ph = 2.0 * kPi * fOut * static_cast<double>(i) / kOut;
        const double s = std::sin(ph), c = std::cos(ph), y = a.l[i];
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += y * s;
        yc += y * c;
    }
    const double det = ss * cc - sc * sc;
    const double fa = (ys * cc - yc * sc) / det, fb = (yc * ss - ys * sc) / det;
    double sig = 0, res = 0;
    for (size_t i = from; i < to; ++i) {
        const double ph = 2.0 * kPi * fOut * static_cast<double>(i) / kOut;
        const double fit = fa * std::sin(ph) + fb * std::cos(ph);
        sig += fit * fit;
        res += (a.l[i] - fit) * (a.l[i] - fit);
    }
    const double ideal = 0.5 * std::cos(kPi / 4.0);  // amplitude x centre pan
    return {10.0 * std::log10(sig / std::max(res, 1e-300)), db(std::sqrt(fa * fa + fb * fb) / ideal)};
}

// ns per output frame for one sounding voice (one layer, looping so it never ends).
double costNs(Interpolator interp, int channels, double srcRate, int key, int layers = 1) {
    Instrument inst;
    inst.prepare(kOut);
    inst.setInterpolator(interp);
    Program prog;
    Zone z = fullZone(PlayMode::Loop, 60);
    z.layers.resize(static_cast<size_t>(layers));
    addZone(prog, z);
    install(inst, prog);
    const auto src = noise(static_cast<size_t>(srcRate * 4), 11);
    for (int j = 0; j < layers; ++j)
        inst.setSample(prog.zones[0].layers[static_cast<size_t>(j)].id,
                       channels == 1 ? Sample::mono(srcRate, src) : Sample::stereo(srcRate, src, noise(src.size(), 12)));
    std::vector<float> l(256), r(256);
    const int blocks = static_cast<int>(20.0 * kOut / 256);
    double best = 1e30;
    volatile float sink = 0.0f;
    for (int rep = 0; rep < 5; ++rep) {
        inst.reset();
        Event ev{0, Event::NoteOn, static_cast<uint8_t>(key), 100};
        inst.process(0, &ev, 1, l.data(), r.data(), 256);
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 1; b < blocks; ++b) {
            inst.process(static_cast<int64_t>(b) * 256, nullptr, 0, l.data(), r.data(), 256);
            sink = sink + l[0];
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / ((blocks - 1) * 256.0);
        best = std::min(best, ns);
    }
    // Same loop with no voice: the cost of process() itself, subtracted.
    double empty = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        inst.reset();
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 1; b < blocks; ++b) inst.process(static_cast<int64_t>(b) * 256, nullptr, 0, l.data(), r.data(), 256);
        const auto t1 = std::chrono::steady_clock::now();
        empty = std::min(empty, std::chrono::duration<double, std::nano>(t1 - t0).count() / ((blocks - 1) * 256.0));
    }
    return best - empty;
}

std::string f1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
std::string f2(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: interpolator choice and voice cost (C2) ===\n");
#ifdef NDEBUG
    const bool release = true;
#else
    const bool release = false;
#endif

    group("Interpolator quality: SNR (dB) / gain (dB) of a 44.1 kHz sine played on 48 kHz");
    std::printf("    %-28s %-18s %-18s\n", "case", "Hermite4", "Sinc16");
    // Pitch shifts give the same numbers as the root key below the alias
    // limit: the error depends on the source frequency against the source
    // rate, not on the step size.
    const double freqs[] = {100, 1000, 5000, 10000, 15000, 18000};
    double hermite10k = 0, sinc10k = 0;
    for (double f : freqs) {
        const Quality h = measure(Interpolator::Hermite4, 44100.0, f, 60);
        const Quality s = measure(Interpolator::Sinc, 44100.0, f, 60);
        if (f == 10000) {
            hermite10k = h.snrDb;
            sinc10k = s.snrDb;
        }
        std::printf("    %-28s %6s / %-9s %6s / %-9s\n", ("root key, " + f1(f) + " Hz").c_str(), f1(h.snrDb).c_str(),
                    f2(h.gainDb).c_str(), f1(s.snrDb).c_str(), f2(s.gainDb).c_str());
    }
    for (double f : {1000.0, 5000.0, 10000.0}) {
        const Quality h = measure(Interpolator::Hermite4, 44100.0, f, 53);
        const Quality s = measure(Interpolator::Sinc, 44100.0, f, 53);
        std::printf("    %-28s %6s / %-9s %6s / %-9s\n", ("key -7 st, " + f1(f) + " Hz").c_str(), f1(h.snrDb).c_str(),
                    f2(h.gainDb).c_str(), f1(s.snrDb).c_str(), f2(s.gainDb).c_str());
    }
    for (double f : {1000.0, 5000.0}) {
        const Quality h = measure(Interpolator::Hermite4, 44100.0, f, 67);
        const Quality s = measure(Interpolator::Sinc, 44100.0, f, 67);
        std::printf("    %-28s %6s / %-9s %6s / %-9s\n", ("key +7 st, " + f1(f) + " Hz").c_str(), f1(h.snrDb).c_str(),
                    f2(h.gainDb).c_str(), f1(s.snrDb).c_str(), f2(s.gainDb).c_str());
    }
    check("both interpolators measured (the sinc is the cleaner at 10 kHz: " + f1(sinc10k) + " vs " + f1(hermite10k) +
              " dB)",
          sinc10k > hermite10k);

    group("Per-voice cost, ns per output frame (one layer, looping; process() overhead subtracted)");
    if (!release) note("NOTE: not a Release build; these numbers are not the ones to record.");
    std::printf("    %-34s %-10s %-10s\n", "case", "Hermite4", "Sinc16");
    double sincWorst1 = 0, sincWorst4 = 0;
    struct Case {
        const char* name;
        int ch;
        double rate;
        int key;
        int layers;
    } cases[] = {{"mono, 48 kHz, root key", 1, 48000.0, 60, 1},
                 {"mono, 44.1 kHz, +7 st", 1, 44100.0, 67, 1},
                 {"stereo, 44.1 kHz, +7 st", 2, 44100.0, 67, 1},
                 {"stereo, 44.1 kHz, +7 st, 4 layers", 2, 44100.0, 67, 4}};
    for (const Case& c : cases) {
        const double h = costNs(Interpolator::Hermite4, c.ch, c.rate, c.key, c.layers);
        const double s = costNs(Interpolator::Sinc, c.ch, c.rate, c.key, c.layers);
        if (c.ch == 2 && c.layers == 1) sincWorst1 = s;  // the worst one-layer case
        if (c.layers == 4) sincWorst4 = s;
        std::printf("    %-34s %-10s %-10s\n", c.name, f1(h).c_str(), f1(s).c_str());
    }
    note("one stereo one-layer sinc voice at 48 kHz = " + f2(sincWorst1 * 48000.0 / 1e9 * 100.0) +
         " % of one core; four layers = " + f2(sincWorst4 * 48000.0 / 1e9 * 100.0) + " %");
    check("the chosen interpolator is the instrument's default (Sinc)", [] {
        Instrument i;
        i.prepare(48000.0);
        return i.interpolator() == Interpolator::Sinc;
    }());

    return summary("interp_tests");
}
