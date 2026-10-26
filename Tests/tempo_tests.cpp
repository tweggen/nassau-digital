// tempo_tests.cpp - C7: tempo modes by resampling (DESIGN §5, `tempo-loop`,
// `tempo-oneshot`, method `resample`). Every test signal is generated here.
//
// C7.1 a 2-beat loop at 120 BPM played at 100 BPM measures a period of
//      2 * 60 / 100 s +- 1 ms, and its pitch falls by 12 * log2(120 / 100)
//      semitones +- 1 cent.
// C7.2 a tempo change mid-loop takes effect without a click: no sample step
//      larger than the loop's own largest step.
// Also: tempo-oneshot lasts its length scaled by the tempo and ignores
//      note-off; the stretch method and beats = 0 leave the rate alone; the
//      bytes are deterministic with tempo changes; no allocation in process().
//
// The test loop: 1 s at 48 kHz (2 beats at 120 BPM), a 1 kHz sine (exactly
// 1000 cycles, so the loop's seam is continuous) under a raised-cosine swell
// once per loop. The carrier gives the pitch (zero crossings), the swell
// makes the output periodic with the loop period and nothing shorter
// (autocorrelation), and the waveform is smooth, so any jump of the read
// position shows as a step far above the loop's own.

#include "nd_edit.h"
#include "nd_instrument.h"
#include "render_util.h"
#include "test_util.h"
#include "alloc_counter.h"

#include <cstring>

using namespace nd;
using namespace ndtest;

namespace {

constexpr double kRate = 48000.0;

size_t at(double seconds) { return static_cast<size_t>(std::llround(seconds * kRate)); }

// The 2-beat loop described above, `seconds` long at `rate`.
std::vector<float> swellLoop(double rate, double seconds) {
    const size_t n = static_cast<size_t>(std::llround(seconds * rate));
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        v[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * t) *
                                  (0.6 + 0.4 * std::cos(2.0 * kPi * t / seconds)));
    }
    return v;
}

// One zone, one layer of `beats` beats, the given sample.
void tempoLayer(Instrument& inst, PlayMode mode, TempoMethod method, double beats, std::shared_ptr<const Sample> s) {
    Program prog;
    Zone z = fullZone(mode, 60);
    z.tempoMethod = method;
    z.release = 0.05;
    z.layers[0].beats = beats;
    addZone(prog, z);
    install(inst, prog);
    inst.setSample(prog.zones[0].layers[0].id, std::move(s));
}

double maxStep(const std::vector<float>& v, size_t from, size_t to) {
    double m = 0.0;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, v.size()); ++i)
        m = std::max(m, std::fabs(static_cast<double>(v[i]) - static_cast<double>(v[i - 1])));
    return m;
}

// Renders `frames` frames in blocks of `block`, setting the tempo to
// tempos[k] at the start of the block that begins at changes[k] (each a
// multiple of `block`). Allocates nothing once `out` is sized.
void renderWithTempo(Instrument& inst, const std::vector<Note>& notes, int64_t frames, Stereo& out,
                     std::vector<Event>& ev, const std::vector<int64_t>& changes, const std::vector<double>& tempos,
                     int block) {
    size_t next = 0, nextChange = 0;
    for (int64_t b = 0; b < frames; b += block) {
        while (nextChange < changes.size() && changes[nextChange] <= b) inst.setTempo(tempos[nextChange++]);
        const int n = static_cast<int>(std::min<int64_t>(block, frames - b));
        ev.clear();
        while (next < notes.size() && notes[next].frame < b + n) {
            Event e;
            e.offset = static_cast<int32_t>(notes[next].frame - b);
            e.type = notes[next].type;
            e.key = static_cast<uint8_t>(notes[next].key);
            e.value = static_cast<uint8_t>(notes[next].value);
            ev.push_back(e);
            ++next;
        }
        inst.process(b, ev.data(), static_cast<int>(ev.size()), out.l.data() + b, out.r.data() + b, n);
    }
}

Stereo renderTempo(Instrument& inst, const std::vector<Note>& notes, int64_t frames, const std::vector<int64_t>& changes,
                   const std::vector<double>& tempos, int block = 256) {
    Stereo out;
    out.l.assign(static_cast<size_t>(frames), 0.0f);
    out.r.assign(static_cast<size_t>(frames), 0.0f);
    std::vector<Event> ev;
    ev.reserve(notes.size());
    renderWithTempo(inst, notes, frames, out, ev, changes, tempos, block);
    return out;
}

// The last frame (+1) louder than `floor`, from the end.
size_t lastAbove(const std::vector<float>& v, double floor) {
    for (size_t i = v.size(); i > 0; --i)
        if (std::fabs(v[i - 1]) > floor) return i;
    return 0;
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: tempo modes by resampling (C7) ===\n");

    // ---------------------------------------------------------------- C7.1 ---
    group("C7.1 a 2-beat loop at 120 BPM played at 100 BPM: period 1.2 s +- 1 ms, pitch -3.1564 st +- 1 cent");
    {
        const double expectSemis = -12.0 * std::log2(120.0 / 100.0);
        // At 48 kHz, and once more from a 44.1 kHz sample (the rate ratio is
        // in the read rate too, and the layer tempo is in seconds, not frames).
        for (double srcRate : {48000.0, 44100.0}) {
            Instrument inst;
            inst.prepare(kRate);
            tempoLayer(inst, PlayMode::TempoLoop, TempoMethod::Resample, 2.0, Sample::mono(srcRate, swellLoop(srcRate, 1.0)));
            inst.setTempo(100.0);
            const Stereo a = render(inst, {on(0, 60)}, at(6.0));
            // Lags 1.15..1.25 s around the expected 1.2 s; window 0.5..5.9 s.
            const double period = autocorrPeriod(a.l, at(0.5), at(5.9), static_cast<int>(at(1.15)), static_cast<int>(at(1.25)));
            const double periodMs = period / kRate * 1000.0;
            const double f = zeroCrossingF0(a.l, at(0.5), at(5.9), kRate);
            const double semis = 12.0 * std::log2(f / 1000.0);
            const double cents = (semis - expectSemis) * 100.0;
            const std::string tag = srcRate == 48000.0 ? "48 kHz sample" : "44.1 kHz sample";
            note(tag + ": period " + std::to_string(period) + " samples = " + std::to_string(periodMs) +
                 " ms (expected 1200 ms); carrier " + std::to_string(f) + " Hz = " + std::to_string(semis) +
                 " st (expected " + std::to_string(expectSemis) + "), error " + std::to_string(cents) + " cent");
            check(tag + ": period 1200 ms +- 1 ms", std::fabs(periodMs - 1200.0) <= 1.0);
            check(tag + ": pitch falls by 12*log2(1.2) semitones +- 1 cent", std::fabs(cents) <= 1.0);
            check(tag + ": still sounding after 6 s (a loop)", inst.activeVoices() == 1 && rms(a.l, at(5.0), at(6.0)) > 0.1);
        }
        // At the loop's own tempo it plays at its own rate: period 1 s, 1 kHz.
        {
            Instrument inst;
            inst.prepare(kRate);
            tempoLayer(inst, PlayMode::TempoLoop, TempoMethod::Resample, 2.0, Sample::mono(kRate, swellLoop(kRate, 1.0)));
            inst.setTempo(120.0);
            const Stereo a = render(inst, {on(0, 60)}, at(4.0));
            const double period = autocorrPeriod(a.l, at(0.5), at(3.9), static_cast<int>(at(0.95)), static_cast<int>(at(1.05)));
            const double f = zeroCrossingF0(a.l, at(0.5), at(3.9), kRate);
            note("at 120 BPM: period " + std::to_string(period / kRate * 1000.0) + " ms, carrier " + std::to_string(f) + " Hz");
            check("at the loop's own tempo: period 1000 ms +- 1 ms and 1000 Hz +- 1 cent",
                  std::fabs(period / kRate * 1000.0 - 1000.0) <= 1.0 && std::fabs(1200.0 * std::log2(f / 1000.0)) <= 1.0);
        }
        // The stretch method is handed a buffer already rendered to the tempo
        // (DESIGN §5), and beats = 0 means the length is unknown: both play at
        // the sample's own rate whatever the tempo.
        for (int variant = 0; variant < 2; ++variant) {
            Instrument inst;
            inst.prepare(kRate);
            tempoLayer(inst, PlayMode::TempoLoop, variant == 0 ? TempoMethod::Stretch : TempoMethod::Resample,
                       variant == 0 ? 2.0 : 0.0, Sample::mono(kRate, swellLoop(kRate, 1.0)));
            inst.setTempo(100.0);
            const Stereo a = render(inst, {on(0, 60)}, at(2.0));
            const double f = zeroCrossingF0(a.l, at(0.2), at(1.9), kRate);
            note(std::string(variant == 0 ? "stretch, beats 2" : "resample, beats 0") + " at 100 BPM: carrier " +
                 std::to_string(f) + " Hz");
            check(std::string(variant == 0 ? "stretch" : "beats = 0") + ": the rate is the sample's own (1000 Hz +- 1 cent)",
                  std::fabs(1200.0 * std::log2(f / 1000.0)) <= 1.0);
        }
        // tempo-oneshot: once, at the song tempo, ignoring note-off.
        {
            Instrument inst;
            inst.prepare(kRate);
            tempoLayer(inst, PlayMode::TempoOneShot, TempoMethod::Resample, 2.0, Sample::mono(kRate, swellLoop(kRate, 1.0)));
            inst.setTempo(100.0);
            const Stereo a = render(inst, {on(0, 60), off(at(0.1), 60)}, at(2.0));
            const double endMs = static_cast<double>(lastAbove(a.l, 1e-4)) / kRate * 1000.0;
            const double f = zeroCrossingF0(a.l, at(0.2), at(1.1), kRate);
            const double cents = (12.0 * std::log2(f / 1000.0) - expectSemis) * 100.0;
            note("tempo-oneshot at 100 BPM: last sound at " + std::to_string(endMs) + " ms (expected 1200 ms), pitch error " + std::to_string(cents) + " cent, voices after " + std::to_string(inst.activeVoices()));
            check("tempo-oneshot: plays once for 1200 ms +- 1 ms despite the note-off at 100 ms",
                  std::fabs(endMs - 1200.0) <= 1.0 && inst.activeVoices() == 0);
            check("tempo-oneshot: its pitch falls by 12*log2(1.2) semitones +- 1 cent", std::fabs(cents) <= 1.0);
        }
    }

    // ---------------------------------------------------------------- C7.2 ---
    group("C7.2 a tempo change mid-loop takes effect without a click (no step above the loop's own largest)");
    {
        const auto loop = Sample::mono(kRate, swellLoop(kRate, 1.0));
        const int64_t change = 30208;  // mid-loop (0.63 s), a block boundary (118 x 256)
        // Up through the loop's own tempo and down again.
        const double pairs[][2] = {{100.0, 140.0}, {140.0, 100.0}, {90.0, 160.0}};
        for (const auto& pr : pairs) {
            const double t0 = pr[0], t1 = pr[1];
            Instrument inst;
            inst.prepare(kRate);
            tempoLayer(inst, PlayMode::TempoLoop, TempoMethod::Resample, 2.0, loop);
            inst.setTempo(t0);
            const Stereo a = renderTempo(inst, {on(0, 60)}, at(4.0), {change}, {t1});
            // The loop's own largest step: the same loop played steadily at
            // each tempo, over a whole loop period (every phase of it). The
            // whole render's largest step is printed beside it: it agrees to
            // within where the discrete samples fall on the waveform's
            // steepest point, so it is not the comparison.
            double own = 0.0;
            for (double t : {t0, t1}) {
                Instrument ref;
                ref.prepare(kRate);
                tempoLayer(ref, PlayMode::TempoLoop, TempoMethod::Resample, 2.0, loop);
                ref.setTempo(t);
                const Stereo r = render(ref, {on(0, 60)}, at(3.0));
                own = std::max(own, maxStep(r.l, at(0.5), at(0.5 + 120.0 / t + 0.01)));
            }
            const size_t c = static_cast<size_t>(change);
            const double around = maxStep(a.l, c - at(0.01), c + at(0.01));
            const double whole = maxStep(a.l, 1, a.l.size());
            // It took effect: the carrier after the change is 1000 * t1 / 120 Hz.
            const double fAfter = zeroCrossingF0(a.l, c + at(0.05), at(3.9), kRate);
            const double centsAfter = 1200.0 * std::log2(fAfter / (1000.0 * t1 / 120.0));
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "%.0f -> %.0f BPM at frame %lld: largest step within 10 ms of the change %.6f, in the whole "
                          "render %.6f; the loop's own largest step %.6f; after: %.4f Hz (%+.4f cent)",
                          t0, t1, static_cast<long long>(change), around, whole, own, fAfter, centsAfter);
            note(buf);
            std::snprintf(buf, sizeof buf, "%.0f -> %.0f BPM: no step larger than the loop's own largest", t0, t1);
            check(buf, around <= own);
            std::snprintf(buf, sizeof buf, "%.0f -> %.0f BPM: the new tempo takes effect (pitch +- 1 cent)", t0, t1);
            check(buf, std::fabs(centsAfter) <= 1.0);
        }
    }

    // --------------------------------------------------------- determinism ---
    group("R8 tempo changes: reset, the same notes and tempos, the same bytes; block size 256 vs 37");
    {
        Instrument inst;
        inst.prepare(kRate);
        tempoLayer(inst, PlayMode::TempoLoop, TempoMethod::Resample, 2.0, Sample::mono(44100.0, noise(44100, 5)));
        // Changes at frames that are block boundaries for both 256 and 37.
        const std::vector<int64_t> changes = {9472, 18944, 47360};
        const std::vector<double> tempos = {133.0, 87.5, 120.0};
        const std::vector<Note> notes = {on(0, 60), on(5000, 64), off(30000, 60), on(40000, 55), off(70000, 64)};
        inst.setTempo(100.0);
        const Stereo a = renderTempo(inst, notes, 90000, changes, tempos);
        inst.reset();
        inst.setTempo(100.0);
        const Stereo b = renderTempo(inst, notes, 90000, changes, tempos);
        inst.reset();
        inst.setTempo(100.0);
        const Stereo c = renderTempo(inst, notes, 90000, changes, tempos, 37);
        auto same = [](const Stereo& x, const Stereo& y) {
            return std::memcmp(x.l.data(), y.l.data(), x.l.size() * sizeof(float)) == 0 &&
                   std::memcmp(x.r.data(), y.r.data(), x.r.size() * sizeof(float)) == 0;
        };
        check("two runs after reset are byte-identical (cmp)", same(a, b));
        check("block size 37 renders the same bytes as 256", same(a, c));
        check("...and the render is not silence", rms(a.l, 0, 90000) > 0.01);
    }

    // ------------------------------------------------------------------ R3 ---
    group("R3 no allocation in process() or setTempo() with tempo voices");
    {
        Instrument inst;
        inst.prepare(kRate);
        tempoLayer(inst, PlayMode::TempoLoop, TempoMethod::Resample, 4.0, Sample::mono(kRate, noise(24000, 9)));
        std::vector<Note> notes;
        for (int i = 0; i < 200; ++i) {
            notes.push_back(on(i * 1000, 36 + (i * 7) % 48, 1 + (i * 13) % 127));
            if (i >= 40) notes.push_back(off(i * 1000 + 1, 36 + ((i - 40) * 7) % 48));
        }
        std::sort(notes.begin(), notes.end(), [](const Note& x, const Note& y) { return x.frame < y.frame; });
        std::vector<int64_t> changes;
        std::vector<double> tempos;
        for (int k = 0; k < 100; ++k) {
            changes.push_back(k * 2048);
            tempos.push_back(70.0 + (k * 37) % 110);
        }
        const int64_t frames = 200 * 1000 + 48000;
        Stereo out;
        out.l.assign(static_cast<size_t>(frames), 0.0f);
        out.r.assign(static_cast<size_t>(frames), 0.0f);
        std::vector<Event> ev;
        ev.reserve(notes.size());
        int maxVoices = 0;
        const long long before = newCount().load();
        renderWithTempo(inst, notes, frames, out, ev, changes, tempos, 256);
        maxVoices = inst.activeVoices();
        const long long news = newCount().load() - before;
        note(std::to_string(frames) + " frames, 200 notes, 100 tempo changes (" + std::to_string(maxVoices) +
             " voices at the end): operator new called " + std::to_string(news) + " times");
        check("zero allocations", news == 0);
        check("...and the run made sound", rms(out.l, 0, out.l.size()) > 0.01);
    }

    return summary("tempo_tests");
}
