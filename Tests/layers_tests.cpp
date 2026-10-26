// layers_tests.cpp - C3: layers, velocity, round-robin, voices (DESIGN §2,
// §2.2, §4.5).
//
// C3.1 two layers overlapping by velFade crossfade with equal power (summed
//      RMS within +-0.5 dB across the fade); velFade 0 switches.
// C3.2 a velocity inside the zone but in no layer's window is silent.
// C3.3 round-robin: same note, same position, same layer; 64 hits on one key
//      spread 15..28 over 3 layers; no repeat of a key's layer within a page.
// C3.4 a page rendered alone equals the same page after its predecessor.
// C3.5 choke: group 1 silenced within the choke fade, group 2 untouched.
// C3.6 polyphony 8..256: the (P+1)th note steals the oldest; no allocation
//      after prepare.

#include "nd_edit.h"
#include "nd_instrument.h"
#include "render_util.h"
#include "test_util.h"
#include "alloc_counter.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

using namespace nd;
using namespace ndtest;


namespace {

constexpr double kRate = 48000.0;
size_t at(double seconds) { return static_cast<size_t>(std::llround(seconds * kRate)); }

Layer layer(int lo, int hi, int fade = 0, int rr = 0) {
    Layer l;
    l.velLo = lo;
    l.velHi = hi;
    l.velFade = fade;
    l.rrGroup = rr;
    return l;
}

// Goertzel amplitude of `freq` in v[from, to).
double toneAmp(const std::vector<float>& v, size_t from, size_t to, double freq) {
    const double w = 2.0 * kPi * freq / kRate, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < to; ++i) {
        const double s0 = v[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

bool sameBytes(const std::vector<float>& a, size_t aFrom, const std::vector<float>& b, size_t bFrom, size_t n) {
    return std::memcmp(a.data() + aFrom, b.data() + bFrom, n * sizeof(float)) == 0;
}

// A zone with three round-robin layers playing DC at 0.1, 0.2 and 0.4, so the
// output level says which one played. Oneshot, 200 frames each.
struct RrRig {
    Instrument inst;
    Program prog;
    RrRig() {
        inst.prepare(kRate);
        Zone z = fullZone(PlayMode::OneShot, 60);
        z.keyTrack = false;
        z.velToLevel = 0.0;
        z.layers = {layer(0, 127, 0, 1), layer(0, 127, 0, 1), layer(0, 127, 0, 1)};
        z.layers[0].pan = z.layers[1].pan = z.layers[2].pan = -1.0;  // all on the left, gain 1
        addZone(prog, z);
        install(inst, prog);
        const float lv[3] = {0.1f, 0.2f, 0.4f};
        for (int j = 0; j < 3; ++j)
            inst.setSample(prog.zones[0].layers[static_cast<size_t>(j)].id,
                           Sample::mono(kRate, std::vector<float>(200, lv[j])));
    }
};

// Which of the RrRig's layers sounds at frame i (0, 1, 2; -1 for none/other).
int whichLayer(const std::vector<float>& l, size_t i) {
    const float v = l[i];
    for (int j = 0; j < 3; ++j)
        if (std::fabs(v - 0.1f * static_cast<float>(1 << j)) < 1e-4f) return j;
    return -1;
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: layers, velocity, round-robin, voices (C3) ===\n");

    // ---------------------------------------------------------------- C3.1 ---
    group("C3.1 velocity crossfade: equal power across the fade; velFade 0 switches");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = fullZone(PlayMode::Gated, 60);
        z.velToLevel = 0.0;  // isolate the crossfade from velocity -> level
        z.layers = {layer(0, 70, 10), layer(60, 127, 10)};
        addZone(prog, z);
        install(inst, prog);
        inst.setSample(prog.zones[0].layers[0].id, Sample::mono(kRate, sine(500.0, kRate, 0.5)));
        inst.setSample(prog.zones[0].layers[1].id, Sample::mono(kRate, sine(1250.0, kRate, 0.5)));
        double ref = 0, lo = 1e9, hi = -1e9;
        std::string table;
        for (int v = 50; v <= 80; ++v) {
            inst.reset();
            const Stereo a = render(inst, {on(0, 60, v)}, at(0.2));
            const double r = rms(a.l, at(0.05), at(0.15));
            if (v == 50) ref = r;
            const double d = db(r / ref);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
            if (v % 2 == 0 && v >= 58 && v <= 72) {
                char b[64];
                std::snprintf(b, sizeof b, "v%d %+.3f  ", v, d);
                table += b;
            }
        }
        note("summed RMS relative to velocity 50 (dB): " + table);
        note("range across velocities 50..80: " + std::to_string(lo) + " .. " + std::to_string(hi) + " dB");
        check("summed RMS across the fade within +-0.5 dB", lo >= -0.5 && hi <= 0.5);
        inst.reset();
        const Stereo mid = render(inst, {on(0, 60, 65)}, at(0.2));
        const double a500 = toneAmp(mid.l, at(0.05), at(0.15), 500.0), a1250 = toneAmp(mid.l, at(0.05), at(0.15), 1250.0);
        note("velocity 65 (mid-fade): 500 Hz layer " + std::to_string(a500) + ", 1250 Hz layer " + std::to_string(a1250));
        check("mid-fade both layers sound at equal level (cos = sin at the middle)", std::fabs(a500 / a1250 - 1.0) < 1e-3);

        // Adjacent windows, fade 0: a hard switch between 64 and 65.
        Program p2;
        Zone z2 = z;
        z2.layers = {layer(0, 64, 0), layer(65, 127, 0)};
        addZone(p2, z2);
        install(inst, p2);
        inst.setSample(p2.zones[0].layers[0].id, Sample::mono(kRate, sine(500.0, kRate, 0.5)));
        inst.setSample(p2.zones[0].layers[1].id, Sample::mono(kRate, sine(1250.0, kRate, 0.5)));
        inst.reset();
        const Stereo s64 = render(inst, {on(0, 60, 64)}, at(0.2));
        inst.reset();
        const Stereo s65 = render(inst, {on(0, 60, 65)}, at(0.2));
        const double a64 = toneAmp(s64.l, at(0.05), at(0.15), 500.0), b64 = toneAmp(s64.l, at(0.05), at(0.15), 1250.0);
        const double a65 = toneAmp(s65.l, at(0.05), at(0.15), 500.0), b65 = toneAmp(s65.l, at(0.05), at(0.15), 1250.0);
        note("velFade 0: at 64 the layers measure " + std::to_string(a64) + " / " + std::to_string(b64) + ", at 65 " +
             std::to_string(a65) + " / " + std::to_string(b65));
        check("velFade 0 switches: 64 plays only the low layer, 65 only the high one, each at full level",
              db(b64 / a64) < -100.0 && db(a65 / b65) < -100.0 && std::fabs(a64 / b65 - 1.0) < 1e-3);
    }

    // ---------------------------------------------------------------- C3.2 ---
    group("C3.2 a velocity in the zone but in no layer's window is silent");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        Zone z = fullZone(PlayMode::Gated, 60);
        z.layers = {layer(0, 50), layer(80, 127)};
        addZone(prog, z);
        install(inst, prog);
        for (const Layer& l : prog.zones[0].layers) inst.setSample(l.id, Sample::mono(kRate, sine(500.0, kRate, 0.5)));
        const Stereo gap = render(inst, {on(0, 60, 65)}, at(0.2));
        inst.reset();
        const Stereo hit = render(inst, {on(0, 60, 90)}, at(0.2));
        const double g = std::max(peak(gap.l, 0, gap.l.size()), peak(gap.r, 0, gap.r.size()));
        note("velocity 65 (gap): peak " + (g == 0.0 ? std::string("exactly 0") : std::to_string(db(g)) + " dB") +
             "; velocity 90: RMS " + std::to_string(db(rms(hit.l, at(0.05), at(0.15)))) + " dB");
        check("the gap is silent below -120 dB", db(g) < -120.0);
        check("...while a velocity inside a window sounds", rms(hit.l, at(0.05), at(0.15)) > 0.1);
    }

    // ---------------------------------------------------------------- C3.3 ---
    group("C3.3 round-robin by position");
    {
        RrRig rig;
        // 64 hits on one key at distinct positions, all in one page.
        auto hits = [](int64_t spacing, int64_t offset) {
            std::vector<Note> n;
            for (int i = 0; i < 64; ++i) n.push_back(on(offset + i * spacing, 60));
            return n;
        };
        const int64_t spacings[] = {1000, 1237, 4800};
        const int64_t offsets[] = {0, 333, 96000};
        bool allSpread = true, noRepeat = true, allIdentified = true;
        for (int c = 0; c < 3; ++c) {
            const auto notes = hits(spacings[c], offsets[c]);
            rig.inst.reset();
            const Stereo a = render(rig.inst, notes, offsets[c] + 64 * spacings[c] + 400 - offsets[c], 256, offsets[c]);
            int count[3] = {0, 0, 0};
            int prev = -2;
            for (const Note& n : notes) {
                const int j = whichLayer(a.l, static_cast<size_t>(n.frame - offsets[c] + 50));
                if (j < 0) {
                    allIdentified = false;
                    continue;
                }
                ++count[j];
                if (j == prev) noRepeat = false;
                prev = j;
            }
            note("spacing " + std::to_string(spacings[c]) + ", first hit at " + std::to_string(offsets[c]) +
                 ": layers played " + std::to_string(count[0]) + " / " + std::to_string(count[1]) + " / " +
                 std::to_string(count[2]) + " times");
            for (int j = 0; j < 3; ++j) allSpread = allSpread && count[j] >= 15 && count[j] <= 28;
        }
        check("every hit plays exactly one of the three layers", allIdentified);
        check("over 64 hits each of 3 layers plays between 15 and 28 times (three spacings)", allSpread);
        check("no two consecutive hits of the key in one page play the same layer", noRepeat);

        // The same note at the same position plays the same layer: two runs,
        // and a third run where the note is the only one.
        rig.inst.reset();
        const auto notes = hits(1000, 0);
        const Stereo r1 = render(rig.inst, notes, 64 * 1000 + 400);
        rig.inst.reset();
        const Stereo r2 = render(rig.inst, notes, 64 * 1000 + 400);
        check("two runs of the same notes: the same layer on every hit, byte-identical (cmp)",
              r1.l.size() == r2.l.size() && sameBytes(r1.l, 0, r2.l, 0, r1.l.size()));
        // A note whose predecessor (same key, same page) played a different
        // layer than the hash picks plays the hash's choice when it is alone.
        int sameAlone = 0, tested = 0;
        for (int i = 0; i < 64; ++i) {
            rig.inst.reset();
            const Stereo alone = render(rig.inst, {on(i * 1000, 60)}, 400, 256, i * 1000);
            const int ja = whichLayer(alone.l, 50), jr = whichLayer(r1.l, static_cast<size_t>(i * 1000 + 50));
            ++tested;
            if (ja == jr) ++sameAlone;
        }
        note("a hit rendered alone at its position plays the same layer as in the sequence for " +
             std::to_string(sameAlone) + "/" + std::to_string(tested) + " hits (the rest are the no-repeat rule moving on)");
        check("the choice is a function of the position: most hits agree when rendered alone", sameAlone >= 40);
    }

    // ---------------------------------------------------------------- C3.4 ---
    group("C3.4 a page rendered alone equals the same page rendered after its predecessor");
    {
        RrRig rig;
        const int64_t page = 48000;
        // The predecessor's last hit must play the very layer the page's first
        // hit picks when the page is rendered alone: only then would a memory
        // leaking across the page boundary move the page's first hit (the
        // no-repeat rule) and show in the bytes. Search for such a position.
        std::vector<Note> pageNotes;
        for (int i = 0; i < 40; ++i) pageNotes.push_back(on(page + i * 1200, 60));
        rig.inst.reset();
        const Stereo alone = render(rig.inst, pageNotes, page, 256, page);
        const int firstAlone = whichLayer(alone.l, 50);
        std::vector<Note> notes;
        int64_t lastPred = -1;
        Stereo pred;
        for (int64_t cand = page - 1000; cand < page - 400 && lastPred < 0; cand += 7) {
            notes.clear();
            for (int i = 0; i < 39; ++i) notes.push_back(on(i * 1200, 60));
            notes.push_back(on(cand, 60));
            rig.inst.reset();
            rig.inst.startPage();
            pred = render(rig.inst, notes, page, 256, 0);
            if (whichLayer(pred.l, static_cast<size_t>(cand + 50)) == firstAlone) lastPred = cand;
        }
        for (const Note& n : pageNotes) notes.push_back(n);
        // After its predecessor: the same instance, startPage() at the page.
        rig.inst.reset();
        rig.inst.startPage();
        pred = render(rig.inst, notes, page, 256, 0);
        rig.inst.startPage();
        const Stereo after = render(rig.inst, notes, page, 256, page);
        int distinct[3] = {0, 0, 0};
        for (int i = 0; i < 40; ++i) {
            const int j = whichLayer(alone.l, static_cast<size_t>(i * 1200 + 50));
            if (j >= 0) ++distinct[j];
        }
        note("round-robin in the page: layers " + std::to_string(distinct[0]) + " / " + std::to_string(distinct[1]) +
             " / " + std::to_string(distinct[2]) + "; the predecessor's last hit (frame " + std::to_string(lastPred) +
             ") played layer " + std::to_string(lastPred < 0 ? -1 : whichLayer(pred.l, static_cast<size_t>(lastPred + 50))) +
             ", the page's first hit alone plays " + std::to_string(firstAlone));
        check("the predecessor ends on the layer the page starts with (so the page boundary is tested)", lastPred >= 0);
        check("round-robin is in use in the page (all three layers play)", distinct[0] > 0 && distinct[1] > 0 && distinct[2] > 0);
        check("page alone == page after its predecessor, byte for byte (cmp)", sameBytes(alone.l, 0, after.l, 0, page) &&
                                                                                 sameBytes(alone.r, 0, after.r, 0, page));
    }

    // ---------------------------------------------------------------- C3.5 ---
    group("C3.5 choke: group 1 silenced within the choke fade, group 2 untouched");
    {
        auto rig = [](Instrument& inst, Program& prog) {
            inst.prepare(kRate);
            prog = Program{};
            const int keys[3] = {36, 37, 38}, groups[3] = {1, 1, 2};
            const double freqs[3] = {500.0, 700.0, 900.0};
            for (int i = 0; i < 3; ++i) {
                Zone z = fullZone(PlayMode::Gated, keys[i]);
                z.keyLo = z.keyHi = keys[i];
                z.choke = groups[i];
                addZone(prog, z);
            }
            install(inst, prog);
            for (int i = 0; i < 3; ++i)
                inst.setSample(prog.zones[static_cast<size_t>(i)].layers[0].id, Sample::mono(kRate, sine(freqs[i], kRate, 1.0)));
        };
        const size_t t = at(0.3), fade = static_cast<size_t>(std::llround(Instrument::kCutFadeSeconds * kRate));
        Instrument a, b;
        Program pa, pb;
        rig(a, pa);
        rig(b, pb);
        // a: kick-like A (group 1) and C (group 2) sounding, then B (group 1) at t.
        const Stereo full = render(a, {on(0, 36), on(0, 38), on(static_cast<int64_t>(t), 37)}, at(0.6));
        // b: the same without A.
        const Stereo noA = render(b, {on(0, 38), on(static_cast<int64_t>(t), 37)}, at(0.6));
        size_t firstEqual = full.l.size();
        for (size_t i = full.l.size(); i-- > t;) {
            if (full.l[i] != noA.l[i] || full.r[i] != noA.r[i]) break;
            firstEqual = i;
        }
        note("A's contribution ends " + std::to_string(static_cast<long>(firstEqual) - static_cast<long>(t)) +
             " samples after the choking note (choke fade " + std::to_string(fade) + " samples = 5 ms)");
        check("group 1 is silenced within the choke fade (after it the render equals one without A, cmp)",
              firstEqual <= t + fade);
        check("...and not before the choking note (A still sounds up to it)", firstEqual > t);
        const double cBefore = toneAmp(full.l, at(0.1), at(0.29), 900.0);
        const double cAfter = toneAmp(full.l, at(0.35), at(0.55), 900.0);
        note("group 2 (900 Hz) amplitude before the choke " + std::to_string(cBefore) + ", after " + std::to_string(cAfter));
        check("group 2 untouched: same amplitude before and after (+- 0.1 %)", std::fabs(cAfter / cBefore - 1.0) < 1e-3);
        check("...and still sounding at the end", a.activeVoices() == 2);
    }

    // ---------------------------------------------------------------- C3.6 ---
    group("C3.6 polyphony 8..256: the (P+1)th note steals the oldest; no allocation after prepare");
    {
        // DC samples: the first note at velocity 127 (gain 1), the others at
        // velocity 1 (gain 1/127), so the level says whether the oldest went.
        Instrument inst;
        inst.prepare(kRate);
        const auto dc = Sample::mono(kRate, std::vector<float>(at(1.0), 1.0f));
        bool allSteal = true;
        long long allocs = 0;
        std::string table;
        for (int poly : kPolyphonyChoices) {
            Program prog;
            prog.polyphony = poly;
            Zone z = fullZone(PlayMode::Gated, 60);
            z.keyTrack = false;
            z.layers[0].pan = -1.0;
            addZone(prog, z);
            install(inst, prog);
            inst.setSample(prog.zones[0].layers[0].id, dc);
            inst.reset();
            std::vector<Note> notes;
            for (int i = 0; i < poly; ++i) notes.push_back(on(i * 10, i % 128, i == 0 ? 127 : 1));
            const int64_t tSteal = poly * 10 + 100;
            notes.push_back(on(tSteal, 5, 1));
            Stereo out;
            out.l.assign(static_cast<size_t>(tSteal + 2000), 0.0f);
            out.r.assign(out.l.size(), 0.0f);
            std::vector<Event> ev;
            ev.reserve(notes.size());
            const long long before = newCount().load();
            renderInto(inst, notes, tSteal + 2000, out, ev);
            allocs += newCount().load() - before;
            const double held = out.l[static_cast<size_t>(tSteal - 1)];
            const double after = out.l[static_cast<size_t>(tSteal + 1000)];
            const double expectHeld = 1.0 + (poly - 1) / 127.0, expectAfter = poly / 127.0;
            const bool ok = std::fabs(held - expectHeld) < 1e-4 && std::fabs(after - expectAfter) < 1e-4;
            allSteal = allSteal && ok;
            char b[96];
            std::snprintf(b, sizeof b, "P=%d %.4f->%.4f  ", poly, held, after);
            table += b;
            if (poly == 8)
                check("the 9th note on an 8-voice pool steals the oldest (level 1+7/127 -> 8/127)", ok);
        }
        note("level held -> after the (P+1)th note: " + table);
        check("at every polyphony 8, 16, 32, 64, 128, 256 the (P+1)th note steals the oldest", allSteal);
        note("operator new calls inside process() over all six runs: " + std::to_string(allocs));
        check("no allocation after prepare (process() with stealing at every polyphony)", allocs == 0);
    }

    return summary("layers_tests");
}
