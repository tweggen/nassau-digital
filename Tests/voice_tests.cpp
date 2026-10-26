// voice_tests.cpp - C2: one voice (DESIGN §2, §3 without drive and filter,
// §5 oneshot / gated / loop). Every test signal is generated here; no WAV.
//
// C2.1 key 72 against root 60 plays at 2.000 +- 0.001 x (autocorrelation).
// C2.2 a 44.1 kHz sample in a 48 kHz render keeps its pitch +- 0.1 cent.
// C2.3 oneshot ignores note-off; gated releases; loop's period is the loop
//      length +- 1 sample.
// C2.4 a mono layer at centre is -3.0 +- 0.1 dB per side; hard left leaves
//      the right channel below -120 dB.
// C2.5 reset, the same notes, the same bytes.
// R3   no allocation in process() (global operator new counter).

#include "nd_edit.h"
#include "nd_instrument.h"
#include "render_util.h"
#include "test_util.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

using namespace nd;
using namespace ndtest;

// ---- R3: a global allocation counter ------------------------------------------
namespace {
std::atomic<long long> gNews{0};
}
void* operator new(std::size_t sz) {
    ++gNews;
    if (void* p = std::malloc(sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t sz) {
    ++gNews;
    if (void* p = std::malloc(sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr double kRate = 48000.0;

size_t at(double seconds) { return static_cast<size_t>(std::llround(seconds * kRate)); }

// One zone, one layer, the given sample; returns the layer id.
uint32_t oneLayer(Instrument& inst, Program& prog, Zone z, std::shared_ptr<const Sample> s) {
    prog = Program{};
    addZone(prog, z);
    const uint32_t layerId = prog.zones[0].layers[0].id;
    install(inst, prog);
    inst.setSample(layerId, std::move(s));
    return layerId;
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: one voice (C2) ===\n");

    // ---------------------------------------------------------------- C2.1 ---
    group("C2.1 key 72 against root key 60 measures 2.000 +- 0.001 x the frequency (autocorrelation)");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        oneLayer(inst, prog, fullZone(PlayMode::Gated, 60), Sample::mono(kRate, sine(440.0, kRate, 2.0)));
        const Stereo a = render(inst, {on(0, 60)}, at(0.6));
        inst.reset();
        const Stereo b = render(inst, {on(0, 72)}, at(0.6));
        const double p60 = autocorrPeriod(a.l, at(0.05), at(0.55), 60, 160);
        const double p72 = autocorrPeriod(b.l, at(0.05), at(0.55), 30, 80);
        const double ratio = p60 / p72;
        note("period on 60: " + std::to_string(p60) + " samples (" + std::to_string(kRate / p60) +
             " Hz), on 72: " + std::to_string(p72) + " samples; ratio " + std::to_string(ratio));
        check("frequency ratio 72/60 = 2.000 +- 0.001", std::fabs(ratio - 2.0) <= 0.001);
        check("key 60 plays the sample at its recorded pitch (440 Hz +- 0.1 Hz)", std::fabs(kRate / p60 - 440.0) <= 0.1);
    }

    // ---------------------------------------------------------------- C2.2 ---
    group("C2.2 a 44.1 kHz sample in a 48 kHz render plays at its recorded pitch +- 0.1 cent");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        // 1000 Hz as recorded at 44.1 kHz: 44.1 samples per period.
        oneLayer(inst, prog, fullZone(PlayMode::Gated, 60), Sample::mono(44100.0, sine(1000.0, 44100.0, 3.0)));
        const Stereo a = render(inst, {on(0, 60)}, at(2.5));
        const double f = zeroCrossingF0(a.l, at(0.05), at(2.45), kRate);
        const double cents = 1200.0 * std::log2(f / 1000.0);
        note("measured " + std::to_string(f) + " Hz, " + std::to_string(cents) + " cent");
        check("pitch error within +- 0.1 cent", std::fabs(cents) <= 0.1);
        const double fa = autocorrPeriod(a.l, at(0.05), at(1.0), 40, 60);
        note("cross-check by autocorrelation: period " + std::to_string(fa) + " samples = " +
             std::to_string(kRate / fa) + " Hz");
        check("the rate ratio is in the pitch ratio: the 48 kHz period is 48.000 samples +- 0.01",
              std::fabs(fa - 48.0) <= 0.01);
        // At 44.1 kHz output, the same sample needs no rate conversion at all.
        Instrument inst2;
        inst2.prepare(44100.0);
        Program p2;
        oneLayer(inst2, p2, fullZone(PlayMode::Gated, 60), Sample::mono(44100.0, sine(1000.0, 44100.0, 3.0)));
        const Stereo b = render(inst2, {on(0, 60)}, 44100 * 2);
        const double f2 = zeroCrossingF0(b.l, 2205, 44100 * 2 - 2205, 44100.0);
        note("the same sample rendered at 44.1 kHz: " + std::to_string(f2) + " Hz");
        check("...and at 44.1 kHz output it measures 1000 Hz +- 0.1 cent too",
              std::fabs(1200.0 * std::log2(f2 / 1000.0)) <= 0.1);
    }

    // ---------------------------------------------------------------- C2.3 ---
    group("C2.3 oneshot ignores note-off, gated releases, loop sustains with the loop's period");
    {
        const auto tone = Sample::mono(kRate, sine(1000.0, kRate, 0.5));  // 0.5 s, RMS 0.3536
        // oneshot: note-off at 0.1 s; the sample still plays to its end at 0.5 s.
        {
            Instrument inst;
            inst.prepare(kRate);
            Program prog;
            Zone z = fullZone(PlayMode::OneShot, 60);
            z.release = 0.05;
            oneLayer(inst, prog, z, tone);
            const Stereo a = render(inst, {on(0, 60), off(at(0.1), 60)}, at(0.7));
            const double before = rms(a.l, at(0.02), at(0.09));
            const double after = rms(a.l, at(0.3), at(0.48));
            const double tail = peak(a.l, at(0.5) + 64, at(0.7));
            note("oneshot RMS before note-off " + std::to_string(before) + ", 0.2-0.38 s after it " +
                 std::to_string(after) + ", peak after the sample's end " + std::to_string(tail));
            check("oneshot: level after note-off equals level before it (+- 1 %)",
                  std::fabs(after / before - 1.0) <= 0.01);
            check("oneshot: the voice ends with the sample (silent after 0.5 s)", tail == 0.0 && inst.activeVoices() == 0);
        }
        // gated: note-off at 0.1 s, release 0.1 s (linear): half level at 0.15 s, silence after 0.2 s.
        {
            Instrument inst;
            inst.prepare(kRate);
            Program prog;
            Zone z = fullZone(PlayMode::Gated, 60);
            z.release = 0.1;
            oneLayer(inst, prog, z, tone);
            const Stereo a = render(inst, {on(0, 60), off(at(0.1), 60)}, at(0.7));
            const double before = rms(a.l, at(0.02), at(0.09));
            const double mid = rms(a.l, at(0.145), at(0.155));
            const double late = rms(a.l, at(0.19), at(0.2));
            const double tail = peak(a.l, at(0.2) + 1, at(0.7));
            note("gated RMS before note-off " + std::to_string(before) + ", mid-release (0.145-0.155 s) " +
                 std::to_string(mid) + " (ratio " + std::to_string(mid / before) + "), last 10 ms " +
                 std::to_string(late) + ", peak after release " + std::to_string(tail));
            check("gated: half way through the release the RMS is half (0.50 +- 0.02)",
                  std::fabs(mid / before - 0.5) <= 0.02);
            check("gated: the release ends at note-off + release time (silent from 0.2 s)",
                  tail == 0.0 && late < 0.1 * before);
        }
        // loop: noise sample, loop [1000, 3000): the output repeats every 2000 samples.
        {
            Instrument inst;
            inst.prepare(kRate);
            Program prog;
            Zone z = fullZone(PlayMode::Loop, 60);
            z.layers[0].loopStart = 1000;
            z.layers[0].loopEnd = 3000;
            oneLayer(inst, prog, z, Sample::mono(kRate, noise(5000, 7)));
            const Stereo a = render(inst, {on(0, 60)}, at(1.0));
            const double period = autocorrPeriod(a.l, at(0.2), at(0.9), 1500, 2500);
            const double level = rms(a.l, at(0.8), at(1.0));
            note("loop: measured period " + std::to_string(period) + " samples (loop length 2000), RMS at 0.8-1.0 s " +
                 std::to_string(level));
            check("loop: period equals the loop length +- 1 sample", std::fabs(period - 2000.0) <= 1.0);
            check("loop: still sounding after 1 s (20x the sample's own length)", level > 0.1 && inst.activeVoices() == 1);
            // Pitched up an octave, the loop period halves.
            inst.reset();
            const Stereo b = render(inst, {on(0, 72)}, at(1.0));
            const double p2 = autocorrPeriod(b.l, at(0.2), at(0.9), 700, 1300);
            note("loop on key 72: period " + std::to_string(p2) + " samples");
            check("loop on key 72: period 1000 +- 1 sample", std::fabs(p2 - 1000.0) <= 1.0);
        }
    }

    // ---------------------------------------------------------------- C2.4 ---
    group("C2.4 constant-power pan of a mono layer");
    {
        std::vector<float> dc(at(0.5), 1.0f);
        const auto s = Sample::mono(kRate, dc);
        auto levels = [&](double pan, double& l, double& r) {
            Instrument inst;
            inst.prepare(kRate);
            Program prog;
            Zone z = fullZone(PlayMode::Gated, 60);
            z.layers[0].pan = pan;
            oneLayer(inst, prog, z, s);
            const Stereo a = render(inst, {on(0, 60)}, at(0.3));
            l = rms(a.l, at(0.1), at(0.2));
            r = rms(a.r, at(0.1), at(0.2));
        };
        double l = 0, r = 0;
        levels(0.0, l, r);
        note("centre: L " + std::to_string(db(l)) + " dB, R " + std::to_string(db(r)) + " dB");
        check("centre: -3.0 +- 0.1 dB per side", std::fabs(db(l) + 3.0) <= 0.1 && std::fabs(db(r) + 3.0) <= 0.1);
        levels(-1.0, l, r);
        note("hard left: L " + std::to_string(db(l)) + " dB, R " + (r == 0.0 ? std::string("exactly 0 (-inf dB)") : std::to_string(db(r)) + " dB"));
        check("hard left: right below -120 dB", db(r) < -120.0);
        check("hard left: left at 0.0 +- 0.1 dB", std::fabs(db(l)) <= 0.1);
        levels(1.0, l, r);
        check("hard right: left below -120 dB, right at 0.0 +- 0.1 dB", db(l) < -120.0 && std::fabs(db(r)) <= 0.1);
        // A stereo layer's pan is a balance: centre leaves both channels at full level.
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        oneLayer(inst, prog, fullZone(PlayMode::Gated, 60), Sample::stereo(kRate, dc, std::vector<float>(dc.size(), 0.5f)));
        const Stereo a = render(inst, {on(0, 60)}, at(0.3));
        check("a stereo layer at centre keeps both channels (balance: L 1.0, R 0.5)",
              std::fabs(rms(a.l, at(0.1), at(0.2)) - 1.0) < 1e-4 && std::fabs(rms(a.r, at(0.1), at(0.2)) - 0.5) < 1e-4);
    }

    // ---------------------------------------------------------------- C2.5 ---
    group("C2.5 reset, the same notes, the same bytes");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = fullZone(PlayMode::Loop, 60);
        z.layers[0].loopStart = 2000;
        z.lfoToPitch = 0.3;
        z.lfoToPan = 0.5;
        z.release = 0.2;
        oneLayer(inst, prog, z, Sample::mono(44100.0, noise(30000, 3)));
        const std::vector<Note> notes = {on(0, 60, 100), on(1000, 64, 90), off(9000, 60), on(12345, 67, 30),
                                         off(30000, 64), off(40000, 67), on(41000, 48)};
        const Stereo a = render(inst, notes, 60000);
        inst.reset();
        const Stereo b = render(inst, notes, 60000);
        Instrument other;
        other.prepare(kRate);
        oneLayer(other, prog, z, Sample::mono(44100.0, noise(30000, 3)));
        const Stereo c = render(other, notes, 60000);
        const Stereo d = render(other, {}, 1);  // keep `other` busy, then compare a fresh block size:
        (void)d;
        other.reset();
        const Stereo e = render(other, notes, 60000, 37);
        auto same = [](const Stereo& x, const Stereo& y) {
            return x.l.size() == y.l.size() &&
                   std::memcmp(x.l.data(), y.l.data(), x.l.size() * sizeof(float)) == 0 &&
                   std::memcmp(x.r.data(), y.r.data(), x.r.size() * sizeof(float)) == 0;
        };
        check("two runs after reset are byte-identical (cmp)", same(a, b));
        check("a second instance renders the same bytes", same(a, c));
        check("the bytes do not depend on the block size (256 vs 37)", same(a, e));
        check("...and the render is not silence", rms(a.l, 0, 60000) > 0.01);
    }

    // ------------------------------------------------------------------ R3 ---
    group("R3 no allocation in process()");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = fullZone(PlayMode::Loop, 60);
        z.layers[0].loopStart = 100;
        oneLayer(inst, prog, z, Sample::mono(kRate, noise(4000, 9)));
        std::vector<Note> notes;
        for (int i = 0; i < 400; ++i) {
            notes.push_back(on(i * 600, 36 + (i * 7) % 48, 1 + (i * 13) % 127));
            if (i >= 40) notes.push_back(off(i * 600 + 1, 36 + ((i - 40) * 7) % 48));
        }
        std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.frame < b.frame; });
        std::vector<Event> ev(notes.size());
        std::vector<float> l(256), r(256);
        const long long before = gNews.load();
        size_t next = 0;
        int maxVoices = 0;
        for (int64_t b = 0; b < 400 * 600 + 48000; b += 256) {
            int ne = 0;
            while (next < notes.size() && notes[next].frame < b + 256) {
                ev[static_cast<size_t>(ne)] = Event{static_cast<int32_t>(notes[next].frame - b), notes[next].type,
                                                    static_cast<uint8_t>(notes[next].key),
                                                    static_cast<uint8_t>(notes[next].value)};
                ++ne;
                ++next;
            }
            inst.process(b, ev.data(), ne, l.data(), r.data(), 256);
            maxVoices = std::max(maxVoices, inst.activeVoices());
        }
        const long long news = gNews.load() - before;
        note(std::to_string(400 * 600 + 48000) + " frames, 400 notes, up to " + std::to_string(maxVoices) +
             " voices (stealing at 32): operator new called " + std::to_string(news) + " times");
        check("zero allocations in process()", news == 0);
        check("the run really stole voices (more notes held than the pool of 32)", maxVoices > 32);
    }

    return summary("voice_tests");
}
