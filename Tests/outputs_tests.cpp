// outputs_tests.cpp - C6: outputs (DESIGN §3.1).
//
// C6.1 a zone on aux 3 with aux 3 routed: its signal is on aux 3 and absent
//      from main (below -120 dB).
// C6.2 the same zone with aux 3 not routed plays through main, and the main
//      render equals the render of the program with that zone set to main
//      (cmp).
// C6.3 each output's own bus drive affects only that output (busDrive is
//      main's, busDrive1..8 aux 1..8's; likewise outputLevel, outputLevel1..8).
// Plus: a routing change under a sounding voice leaves the voice on the
// output it started on, and only later notes follow it (DESIGN §13, settled
// 2026-11-01); a voice whose output the host stops handing a buffer for is
// dropped there, not moved and not written through null; R3 (no allocation
// in process() or setRoutedOutputs() across routing changes, buffer layouts
// and every output's bus); R8 (byte-identical across reset and block sizes
// with routing changes mid-render); and, with --bench, what the outputs cost.
//
// Everything is measured on audio through the public API (R6), with the
// nine-output process() qbx's outBuses layout maps onto.

#include "nd_edit.h"
#include "nd_instrument.h"
#include "render_util.h"
#include "test_util.h"
#include "alloc_counter.h"

#include <chrono>
#include <cstring>

using namespace nd;
using namespace ndtest;

namespace {

constexpr double kRate = 48000.0;
constexpr int kOuts = Instrument::kNumOutputs;
constexpr uint32_t kAux3 = 1u << 2;  // bit k = aux k + 1

std::string fmt(const char* f, double v) {
    char b[256];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

// Every output's two channels.
struct Outs {
    std::vector<float> l[kOuts], r[kOuts];
    void resize(int64_t frames) {
        for (int b = 0; b < kOuts; ++b) {
            l[b].assign(static_cast<size_t>(frames), 0.0f);
            r[b].assign(static_cast<size_t>(frames), 0.0f);
        }
    }
};

// A routing change the host makes between two process() calls, at `frame`:
// the mask, and from then on the aux outputs it hands a null buffer for
// (bit k = aux k + 1, on top of the layout's).
struct Change {
    int64_t frame;
    uint32_t mask;
    uint32_t absent = 0;
};

// How the host hands the outputs over: `numBuses` entries of the bus array,
// and the aux outputs in `absent` (bit k = aux k + 1) given a null entry.
// numBuses 0 means the two-pointer process() (main only).
struct Layout {
    int numBuses = kOuts;
    uint32_t absent = 0;
};

// Renders `frames` frames in blocks of `block`, each block also ending at the
// next routing change, which is applied before the block that starts there.
// Allocates nothing (the caller sizes `out` and reserves `ev`).
void renderOutsInto(Instrument& inst, const std::vector<Note>& notes, int64_t frames, Outs& out,
                    std::vector<Event>& ev, int block, const std::vector<Change>& changes, Layout layout) {
    size_t next = 0, nextChange = 0;
    uint32_t absent = layout.absent;
    for (int64_t b = 0; b < frames;) {
        while (nextChange < changes.size() && changes[nextChange].frame <= b) {
            inst.setRoutedOutputs(changes[nextChange].mask);
            absent = layout.absent | changes[nextChange].absent;
            ++nextChange;
        }
        int64_t end = std::min<int64_t>(b + block, frames);
        if (nextChange < changes.size()) end = std::min(end, changes[nextChange].frame);
        const int n = static_cast<int>(end - b);
        ev.clear();
        while (next < notes.size() && notes[next].frame < end) {
            Event e;
            e.offset = static_cast<int32_t>(notes[next].frame - b);
            e.type = notes[next].type;
            e.key = static_cast<uint8_t>(notes[next].key);
            e.value = static_cast<uint8_t>(notes[next].value);
            ev.push_back(e);
            ++next;
        }
        if (layout.numBuses == 0) {
            inst.process(b, ev.data(), static_cast<int>(ev.size()), out.l[0].data() + b, out.r[0].data() + b, n);
        } else {
            float* ch[kOuts][2];
            float* const* buses[kOuts];
            for (int k = 0; k < kOuts; ++k) {
                ch[k][0] = out.l[k].data() + b;
                ch[k][1] = out.r[k].data() + b;
                buses[k] = (k > 0 && (absent & (1u << (k - 1)))) ? nullptr : ch[k];
            }
            inst.process(b, ev.data(), static_cast<int>(ev.size()), buses, layout.numBuses, n);
        }
        b = end;
    }
}

Outs renderOuts(Instrument& inst, const std::vector<Note>& notes, int64_t frames, int block = 256,
                const std::vector<Change>& changes = {}, Layout layout = {}) {
    Outs out;
    out.resize(frames);
    std::vector<Event> ev;
    ev.reserve(notes.size());
    renderOutsInto(inst, notes, frames, out, ev, block, changes, layout);
    return out;
}

bool sameCh(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = SIZE_MAX) {
    to = std::min({to, a.size(), b.size()});
    return a.size() == b.size() && std::memcmp(a.data() + from, b.data() + from, (to - from) * sizeof(float)) == 0;
}

// Output b of x equals output c of y, both channels, byte for byte.
bool sameOut(const Outs& x, int b, const Outs& y, int c, size_t from = 0, size_t to = SIZE_MAX) {
    return sameCh(x.l[b], y.l[c], from, to) && sameCh(x.r[b], y.r[c], from, to);
}

bool silent(const Outs& x, int b, size_t from = 0, size_t to = SIZE_MAX) {
    return peak(x.l[b], from, to) == 0.0 && peak(x.r[b], from, to) == 0.0;
}

// Amplitude of a sinusoid at freq in v[from, to) (Goertzel; whole periods).
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

// A one-key looping zone playing its sample at its own pitch, velocity-
// insensitive, centred.
Zone keyZone(int key, int output) {
    Zone z = fullZone(PlayMode::Loop, key);
    z.keyLo = z.keyHi = key;
    z.keyTrack = false;
    z.velToLevel = 0.0;
    z.output = output;
    return z;
}

// Zone A: key 60, main. Zone B: key 62, output `outB`. Their samples are
// installTwo()'s.
Program twoZones(int outB, double busDrive = 0.0) {
    Program p;
    p.busDrive = busDrive;
    addZone(p, keyZone(60, 0));
    addZone(p, keyZone(62, outB));
    return p;
}

void installTwo(Instrument& inst, const Program& p, double freqB, double amp) {
    install(inst, p);
    inst.setSample(p.zones[0].layers[0].id, Sample::mono(kRate, sine(1000.0, kRate, 1.0, amp)));
    inst.setSample(p.zones[1].layers[0].id, Sample::mono(kRate, sine(freqB, kRate, 1.0, amp)));
}

// Zone A plays a 1 kHz sine, zone B a `freqB` sine, both mono at `amp`.
// A fresh instrument with program p (samples as installTwo), routed `mask`.
struct Rig {
    Instrument inst;
    Rig(const Program& p, uint32_t mask, double freqB = 1500.0, double amp = 0.4) {
        inst.prepare(kRate);
        installTwo(inst, p, freqB, amp);
        inst.setRoutedOutputs(mask);
        inst.reset();
    }
};

// ns per output frame of one process() call shape, best of 5.
template <typename F>
double timeNs(F&& block, int blocks) {
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) block(b);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / (blocks * 256.0));
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("=== NassauDigital core: outputs (C6) ===\n");
    const bool bench = (argc > 1 && std::string(argv[1]) == "--bench");

    // ---------------------------------------------------------------- C6.1 ---
    group("C6.1 a zone on aux 3 with aux 3 routed plays on aux 3 and not on main");
    {
        // B alone: aux 3 carries exactly what main carries when B is set to
        // main, and main is silent.
        Rig routed(twoZones(3), kAux3);
        const Outs o = renderOuts(routed.inst, {on(0, 62)}, 24000);
        Rig onMain(twoZones(0), kAux3);
        const Outs ref = renderOuts(onMain.inst, {on(0, 62)}, 24000);
        const double refRms = rms(ref.l[0], 0, 24000);
        const double mainDb = db(std::max(peak(o.l[0], 0, 24000), peak(o.r[0], 0, 24000)) / refRms);
        bool othersSilent = true;
        for (int b = 1; b < kOuts; ++b)
            if (b != 3) othersSilent = othersSilent && silent(o, b);
        note(fmt("aux 3 RMS %.6f", rms(o.l[3], 0, 24000)) + fmt(" (B on main: %.6f); ", refRms) +
             "main's peak " + (mainDb < -300 ? std::string("exactly 0") : fmt("%.1f dB", mainDb)) + " re that RMS");
        check("aux 3 carries the zone's signal: equal to its render on main (cmp, both channels)", sameOut(o, 3, ref, 0));
        check("main is below -120 dB (it is exact silence)", mainDb < -120.0);
        check("the other aux outputs are silent", othersSilent);
        check("...and the signal is not silence", refRms > 0.1);
    }
    {
        // Both zones: main carries A and none of B.
        Rig routed(twoZones(3), kAux3);
        const Outs o = renderOuts(routed.inst, {on(0, 60), on(0, 62)}, 24000);
        Rig aOnly(twoZones(3), kAux3);
        const Outs a = renderOuts(aOnly.inst, {on(0, 60)}, 24000);
        const size_t from = 4800, to = 4800 + 9600;  // whole periods of 1 and 1.5 kHz
        const double bOnAux = toneAmp(o.l[3], from, to, 1500.0);
        const double bOnMain = db(toneAmp(o.l[0], from, to, 1500.0) / bOnAux);
        note(fmt("both keys: 1.5 kHz (B) on main %.1f dB re B on aux 3; ", bOnMain) +
             fmt("1 kHz (A) on aux 3 %.1f dB re A on main", db(toneAmp(o.l[3], from, to, 1000.0) / toneAmp(o.l[0], from, to, 1000.0))));
        check("both keys: B is below -120 dB on main", bOnMain < -120.0);
        check("both keys: main is exactly A's render alone (cmp)", sameOut(o, 0, a, 0));
    }

    // ---------------------------------------------------------------- C6.2 ---
    group("C6.2 with aux 3 not routed the zone plays through main, equal to the zone set to main (cmp)");
    {
        // A and B interleaved (A, B, A, B in note-on order), B oversampled by
        // its own drive, main's bus drive on and the output level off 0 dB,
        // so the fallback has to go into main's sum in order, before main's
        // bus and its level.
        auto prog = [](int outB) {
            Program p = twoZones(outB, 0.5);
            p.outputLevel = -3.0;
            p.zones[1].drive = 0.3;
            p.zones[1].filter = FilterType::Lp12;
            p.zones[1].cutoff = 3000.0;
            return p;
        };
        const std::vector<Note> notes = {on(0, 60), on(500, 62), on(9000, 60, 90), on(9100, 62, 80),
                                         off(20000, 60), off(21000, 62)};
        Rig refRig(prog(0), 0);
        const Outs ref = renderOuts(refRig.inst, notes, 30000);
        Rig noB(prog(0), 0);
        std::vector<Note> aNotes;
        for (const Note& n : notes)
            if (n.key == 60) aNotes.push_back(n);
        const Outs a = renderOuts(noB.inst, aNotes, 30000);

        struct Variant {
            const char* name;
            uint32_t mask;
            Layout layout;
        } variants[] = {
            {"mask 0 (nothing routed), every buffer handed in", 0u, {}},
            {"mask routes every aux but aux 3", 0xFFu & ~kAux3, {}},
            {"aux 3 in the mask but given no buffer (null entry)", kAux3, {kOuts, kAux3}},
            {"aux 3 in the mask, bus array of main only (numBuses 1)", kAux3, {1, 0}},
            {"aux 3 in the mask, the two-pointer process()", kAux3, {0, 0}},
        };
        for (const Variant& v : variants) {
            Rig rig(prog(3), v.mask);
            const Outs o = renderOuts(rig.inst, notes, 30000, 256, {}, v.layout);
            const bool aux3Quiet = (v.layout.numBuses == kOuts && !(v.layout.absent & kAux3)) ? silent(o, 3) : true;
            check(std::string(v.name) + ": main == the zone set to main (cmp)" +
                      ((v.layout.numBuses == kOuts && !(v.layout.absent & kAux3)) ? ", aux 3 silent" : ""),
                  sameOut(o, 0, ref, 0) && aux3Quiet);
        }
        double diff = 0;
        for (size_t i = 0; i < 30000; ++i) diff = std::max(diff, std::fabs(double(ref.l[0][i]) - a.l[0][i]));
        note(fmt("the zone matters to that render: main without B's notes differs by up to %.3f", diff));
        check("...and B is audible in it (main without B's notes differs by more than 0.05)", diff > 0.05);
    }

    // ---------------------------------------------------------------- C6.3 ---
    group("C6.3 each output's bus drive affects only that output (busDrive main's, busDrive3 aux 3's)");
    {
        // Main's bus at 0.2, aux 3's at 0.7, and aux 5's at 0.9 with nothing
        // on aux 5. What is gated: each output's sum goes through its own
        // amount and no other, and an amount moves nothing but its output.
        const double mainDrive = 0.2, aux3Drive = 0.7;
        auto prog = [&](int outB, double dMain, double d3) {
            Program p = twoZones(outB, dMain);
            p.auxBusDrive[2] = d3;   // busDrive3
            p.auxBusDrive[4] = 0.9;  // busDrive5: nothing plays there
            return p;
        };
        Rig both(prog(3, mainDrive, aux3Drive), kAux3);
        const Outs o = renderOuts(both.inst, {on(0, 60), on(0, 62)}, 48000);
        const bool aux3Bus = both.inst.kitBusOversampling(3), mainBus = both.inst.kitBusOversampling(0);
        bool othersRest = true;
        for (int b = 1; b < kOuts; ++b)
            if (b != 3) othersRest = othersRest && !both.inst.kitBusOversampling(b);
        // A alone on main at main's amount; B alone on MAIN at aux 3's amount.
        Rig aRig(prog(0, mainDrive, 0.0), 0);
        const Outs a = renderOuts(aRig.inst, {on(0, 60)}, 48000);
        Rig bRig(prog(0, aux3Drive, 0.0), 0);
        const Outs bm = renderOuts(bRig.inst, {on(0, 62)}, 48000);
        // B alone on main at MAIN's amount: what aux 3 would carry if it took
        // main's amount (the C6 build before 2026-11-01).
        Rig bMainDrive(prog(0, mainDrive, 0.0), 0);
        const Outs bWrong = renderOuts(bMainDrive.inst, {on(0, 62)}, 48000);
        // Each amount moved alone: the other output stays byte for byte.
        Rig mainMoved(prog(3, 0.45, aux3Drive), kAux3);
        const Outs om = renderOuts(mainMoved.inst, {on(0, 60), on(0, 62)}, 48000);
        Rig auxMoved(prog(3, mainDrive, 0.35), kAux3);
        const Outs oa = renderOuts(auxMoved.inst, {on(0, 60), on(0, 62)}, 48000);
        Rig shared(prog(3, mainDrive, aux3Drive), 0);  // aux 3 unrouted: both on main's bus
        const Outs s = renderOuts(shared.inst, {on(0, 60), on(0, 62)}, 48000);
        Rig sharedRef(prog(0, mainDrive, 0.0), 0);
        const Outs sr = renderOuts(sharedRef.inst, {on(0, 60), on(0, 62)}, 48000);

        const size_t from = 9600, to = 9600 + 38400;  // whole periods of 500 Hz, 1, 1.5 and 2 kHz
        auto im = [&](const Outs& x, int b, double ref) {
            const double r = toneAmp(x.l[b], from, to, ref);
            return std::max(db(toneAmp(x.l[b], from, to, 500.0) / r), db(toneAmp(x.l[b], from, to, 2000.0) / r));
        };
        auto maxDiff = [](const std::vector<float>& x, const std::vector<float>& y) {
            double d = 0;
            for (size_t i = 0; i < x.size(); ++i) d = std::max(d, std::fabs(double(x[i]) - y[i]));
            return d;
        };
        const double imMain = im(o, 0, 1000.0), imAux = im(o, 3, 1500.0), imShared = im(s, 0, 1000.0);
        const double wrongBy = maxDiff(o.l[3], bWrong.l[0]), movedMain = maxDiff(om.l[0], o.l[0]),
                     movedAux = maxDiff(oa.l[3], o.l[3]);
        note(fmt("busDrive %.1f, ", mainDrive) + fmt("busDrive3 %.1f, busDrive5 0.9; ", aux3Drive) +
             fmt("A on main, B on aux 3: intermodulation (500 Hz, 2 kHz) on main %.1f dB, ", imMain) +
             fmt("on aux 3 %.1f dB; ", imAux) + fmt("both on main's bus (aux 3 unrouted) %.1f dB", imShared));
        note(fmt("aux 3 against B driven at main's amount instead: up to %.4f apart; ", wrongBy) +
             fmt("busDrive 0.2 -> 0.45 moves main by up to %.4f, ", movedMain) +
             fmt("busDrive3 0.7 -> 0.35 moves aux 3 by up to %.4f", movedAux));
        check("main is exactly A driven alone at busDrive (cmp): B's output does not reach main's bus", sameOut(o, 0, a, 0));
        check("aux 3 is exactly B driven at busDrive3 (== B alone on main with busDrive 0.7, cmp)", sameOut(o, 3, bm, 0));
        check("...which is not B driven at main's amount (differs by more than 0.01)", wrongBy > 0.01);
        check("busDrive moved alone: aux 3 byte-identical, main changed",
              sameOut(om, 3, o, 3) && movedMain > 0.01);
        check("busDrive3 moved alone: main byte-identical, aux 3 changed",
              sameOut(oa, 0, o, 0) && movedAux > 0.01);
        check("no intermodulation across outputs (below -120 dB on main and on aux 3)", imMain < -120.0 && imAux < -120.0);
        check("...where one shared bus would intermodulate (above -60 dB)", imShared > -60.0);
        check("aux 3 unrouted: B falls back to main and takes main's amount (== both on main, cmp)", sameOut(s, 0, sr, 0));
        check("main's and aux 3's buses run their drive; the others (aux 5 at 0.9 with nothing on it) are at rest",
              mainBus && aux3Bus && othersRest);

        // A missing busDrive<k> is 0: aux 3 at busDrive3 0 is B undriven,
        // while main's bus is on.
        Rig dry(prog(3, mainDrive, 0.0), kAux3);
        const Outs d = renderOuts(dry.inst, {on(0, 60), on(0, 62)}, 48000);
        Rig dryB(prog(0, 0.0, 0.0), 0);
        const Outs dbm = renderOuts(dryB.inst, {on(0, 62)}, 48000);
        check("busDrive3 0 (the default): aux 3 is B undriven (== B alone on main, no bus drive, cmp) while main's bus drives",
              sameOut(d, 3, dbm, 0) && dry.inst.kitBusOversampling(0));
    }
    group("C6 each output's own level (outputLevel main's, outputLevel3 aux 3's)");
    {
        auto prog = [](int outB, double lMain, double l3) {
            Program p = twoZones(outB, 0.0);
            p.outputLevel = lMain;
            p.auxOutputLevel[2] = l3;  // outputLevel3
            return p;
        };
        Rig both(prog(3, -3.0, -9.0), kAux3);
        const Outs o = renderOuts(both.inst, {on(0, 60), on(0, 62)}, 24000);
        Rig aRef(prog(0, -3.0, 0.0), 0);
        const Outs a = renderOuts(aRef.inst, {on(0, 60)}, 24000);
        Rig bRef(prog(0, -9.0, 0.0), 0);
        const Outs b = renderOuts(bRef.inst, {on(0, 62)}, 24000);
        Rig b0(prog(0, 0.0, 0.0), 0);
        const Outs bz = renderOuts(b0.inst, {on(0, 62)}, 24000);
        Rig aux3Moved(prog(3, -3.0, 4.0), kAux3);
        const Outs om = renderOuts(aux3Moved.inst, {on(0, 60), on(0, 62)}, 24000);
        Rig fallback(prog(3, -3.0, -9.0), 0);
        const Outs fb = renderOuts(fallback.inst, {on(0, 62)}, 24000);
        Rig fbRef(prog(0, -3.0, 0.0), 0);
        const Outs fr = renderOuts(fbRef.inst, {on(0, 62)}, 24000);
        const double ratio = db(rms(o.l[3], 0, 24000) / rms(bz.l[0], 0, 24000));
        note(fmt("outputLevel -3, outputLevel3 -9: aux 3 RMS %.2f dB re B at 0 dB", ratio));
        check("main is A at outputLevel (cmp); aux 3 is B at outputLevel3 (== B on main at -9 dB, cmp)",
              sameOut(o, 0, a, 0) && sameOut(o, 3, b, 0));
        check("aux 3 is 9 dB down (+- 0.01 dB)", std::fabs(ratio + 9.0) < 0.01);
        check("outputLevel3 moved alone: main byte-identical", sameOut(om, 0, o, 0) && !sameOut(om, 3, o, 3));
        check("aux 3 unrouted: B falls back to main at main's level (cmp)", sameOut(fb, 0, fr, 0));
        check("outputLevel3 0 dB (the default): aux 3 is B as on main at 0 dB (cmp)", [&] {
            Rig r(prog(3, -3.0, 0.0), kAux3);
            const Outs x = renderOuts(r.inst, {on(0, 62)}, 24000);
            return sameOut(x, 3, bz, 0);
        }());
    }

    // ----------------------------------------------------- routing changes ---
    group("Routing changes under a sounding voice: it keeps its output, later notes follow (DESIGN §13)");
    {
        // B (key 62, a 220 Hz sine, 0.3) and C (key 64, the same 220 Hz
        // sine) both on aux 3. B starts at 0; the routing changes at c1;
        // C starts at c1 + 1000, after it. Bus drives on everywhere they
        // matter (main 0.3, aux 3 0.5), so a voice that kept its output also
        // kept its bus.
        const size_t c1 = 12055;
        const int64_t cOn = int64_t(c1) + 1000;
        auto prog = [](int outB, int outC) {
            Program p = twoZones(outB, 0.3);
            p.auxBusDrive[2] = 0.5;
            p.outputLevel = -1.0;
            p.auxOutputLevel[2] = -2.0;
            addZone(p, keyZone(64, outC));
            return p;
        };
        auto make = [&](int outB, int outC, uint32_t mask) {
            auto r = std::make_unique<Instrument>();
            r->prepare(kRate);
            const Program p = prog(outB, outC);
            installTwo(*r, p, 220.0, 0.3);
            r->setSample(p.zones[2].layers[0].id, Sample::mono(kRate, sine(220.0, kRate, 1.0, 0.3)));
            r->setRoutedOutputs(mask);
            r->reset();
            return r;
        };
        const std::vector<Note> notes = {on(0, 62), on(cOn, 64)};
        const int64_t frames = 36000;
        {
            // Routed, then unrouted at c1.
            auto inst = make(3, 3, kAux3);
            const Outs o = renderOuts(*inst, notes, frames, 256, {{int64_t(c1), 0}});
            auto bRef = make(3, 3, kAux3);  // the unchanged render: B alone, aux 3 routed throughout
            const Outs rb = renderOuts(*bRef, {on(0, 62)}, frames);
            auto cRef = make(3, 0, 0);      // C set to main (what a fallback must equal)
            const Outs rc = renderOuts(*cRef, {on(cOn, 64)}, frames);
            note(fmt("aux 3 RMS after the change %.4f", rms(o.l[3], c1, size_t(frames))) +
                 fmt(" (unchanged render %.4f)", rms(rb.l[3], c1, size_t(frames))));
            check("unrouted under B: aux 3 is byte-identical to the unchanged render, throughout (cmp)", sameOut(o, 3, rb, 3));
            check("...B never reaches main: main is silent until C, and exactly C set to main after (cmp)",
                  silent(o, 0, 0, size_t(cOn)) && sameOut(o, 0, rc, 0));
            check("...and B sounds on aux 3 after the change (RMS above 0.1)", rms(o.l[3], c1, size_t(frames)) > 0.1);
        }
        {
            // The reverse: unrouted, then routed at c1.
            auto inst = make(3, 3, 0);
            const Outs o = renderOuts(*inst, notes, frames, 256, {{int64_t(c1), kAux3}});
            auto bRef = make(0, 0, 0);       // B set to main
            const Outs rb = renderOuts(*bRef, {on(0, 62)}, frames);
            auto cRef = make(3, 3, kAux3);   // C on aux 3, routed throughout
            const Outs rc = renderOuts(*cRef, {on(cOn, 64)}, frames);
            check("routed under B: main is byte-identical to B fallen back to main, throughout (cmp)", sameOut(o, 0, rb, 0));
            check("...B never reaches aux 3: aux 3 is exactly C routed (cmp), silent before C",
                  sameOut(o, 3, rc, 3) && silent(o, 3, 0, size_t(cOn)));
            check("...and C did sound on aux 3 (RMS above 0.1)", rms(o.l[3], size_t(cOn), size_t(frames)) > 0.1);
        }
        {
            // The host stops handing aux 3 a buffer under B ([c1, c2), the
            // mask unchanged), then hands it back. The voice's samples of
            // those calls are dropped, nothing reaches main, and B carries on
            // where it would have been. Bus drives 0 here so the return can be
            // compared byte for byte (a driven aux bus restarts from rest).
            // The gap is not a whole number of the tone's periods (12100
            // frames = 55.46 periods), so a voice that paused in it would come
            // back out of phase.
            const size_t c2 = 24155;
            auto make0 = [&](uint32_t mask) {
                auto r = std::make_unique<Instrument>();
                r->prepare(kRate);
                Program p = twoZones(3, 0.0);
                installTwo(*r, p, 220.0, 0.3);
                r->setRoutedOutputs(mask);
                r->reset();
                return r;
            };
            auto inst = make0(kAux3);
            const Outs o = renderOuts(*inst, {on(0, 62)}, frames, 256,
                                      {{int64_t(c1), kAux3, kAux3}, {int64_t(c2), kAux3, 0}});
            auto ref = make0(kAux3);
            const Outs r = renderOuts(*ref, {on(0, 62)}, frames);
            check("aux 3's buffer withdrawn under B: no crash, main silent throughout (B is not moved to main)",
                  silent(o, 0));
            check("...nothing written to aux 3 while it had no buffer (the host's memory untouched)", silent(o, 3, c1, c2));
            check("...and aux 3 is the unchanged render before and after (cmp): B ran on in the gap",
                  sameOut(o, 3, r, 3, 0, c1) && sameOut(o, 3, r, 3, c2, size_t(frames)));
            // The same while unrouted (a voice kept from before the change),
            // with the bus drives on: still nothing anywhere else.
            auto inst2 = make(3, 3, kAux3);
            const Outs o2 = renderOuts(*inst2, {on(0, 62)}, frames, 256,
                                       {{int64_t(c1), 0}, {int64_t(c1) + 3000, 0, kAux3}, {int64_t(c2), 0, 0}});
            check("unrouted, then its buffer withdrawn, then back: main stays silent, aux 3 silent in the gap",
                  silent(o2, 0) && silent(o2, 3, c1 + 3000, c2) && rms(o2.l[3], c2, size_t(frames)) > 0.1);
        }
    }

    // ------------------------------------------------------------------ R8 ---
    group("R8 reset, the same notes, the same bytes, on every output, with routing changes and a withdrawn buffer");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        prog.busDrive = 0.4;
        prog.auxBusDrive[0] = 0.3;     // busDrive1
        prog.auxBusDrive[2] = 0.6;     // busDrive3
        prog.auxBusDrive[7] = 0.2;     // busDrive8
        prog.auxOutputLevel[2] = -6.0; // outputLevel3
        const int outs[] = {0, 1, 3, 8};
        for (int t = 0; t < 4; ++t) {
            Zone z = fullZone(PlayMode::Loop, 60);
            z.keyLo = 36 + 12 * t;
            z.keyHi = z.keyLo + 11;
            z.output = outs[t];
            z.drive = (t % 2) ? 0.6 : 0.0;
            z.cutoff = 2500.0;
            z.resonance = 0.4;
            z.lfoToPan = 0.5;
            z.release = 0.08;
            addZone(prog, z);
        }
        install(inst, prog);
        const auto smp = Sample::stereo(44100.0, noise(30000, 3), noise(30000, 4));
        for (const Zone& z : prog.zones) inst.setSample(z.layers[0].id, smp);
        const std::vector<Note> notes = {on(0, 40), on(100, 52), on(300, 64), on(700, 76), off(9000, 52),
                                         on(12345, 55), on(12400, 79), off(30000, 40), on(31000, 66), off(40000, 76)};
        // Changes at odd frames as well: the renderer ends a block at each, in
        // every block size, as a host calling setRoutedOutputs() there would.
        // From 18944 to 28416 aux 8's buffer is withdrawn under its voice.
        const std::vector<Change> changes = {{0, 0x85u}, {9472, 0x01u}, {9600, 0xFFu}, {18944, 0x04u, 0x80u},
                                             {28416, 0x00u}, {28500, 0x80u}, {41111, 0xFFu}};
        auto run = [&](int block) {
            inst.setRoutedOutputs(0);
            inst.reset();
            return renderOuts(inst, notes, 50000, block, changes);
        };
        const Outs a = run(256), b = run(256), c = run(37), d = run(1000);
        bool ab = true, ac = true, ad = true, loud = true;
        for (int k = 0; k < kOuts; ++k) {
            ab = ab && sameOut(a, k, b, k);
            ac = ac && sameOut(a, k, c, k);
            ad = ad && sameOut(a, k, d, k);
        }
        for (int k : {0, 1, 3, 8}) loud = loud && rms(a.l[k], 0, 50000) > 0.001;
        check("two runs after reset are byte-identical on all nine outputs (cmp)", ab);
        check("the bytes do not depend on the block size (256 vs 37 vs 1000)", ac && ad);
        check("...and main, aux 1, aux 3 and aux 8 all carried sound", loud);
    }

    // ------------------------------------------------------------------ R3 ---
    group("R3 no allocation in process() or setRoutedOutputs(): routing changes, every output's bus");
    {
        Instrument inst;
        inst.prepare(kRate);
        Program prog;
        prog.polyphony = 16;
        const int outs[] = {0, 1, 3, 8};
        for (int t = 0; t < 4; ++t) {
            Zone z = fullZone(PlayMode::Loop, 60);
            z.keyLo = 36 + 12 * t;
            z.keyHi = z.keyLo + 11;
            z.layers[0].loopStart = 100;
            z.output = outs[t];
            z.drive = (t % 2) ? 0.7 : 0.0;
            addZone(prog, z);
        }
        const auto plain = std::make_shared<const Program>(prog);
        Program busOn = prog;
        busOn.busDrive = 0.5;
        for (int k = 0; k < kNumAuxOutputs; ++k) {
            busOn.auxBusDrive[k] = 0.1 * (k + 1);
            busOn.auxOutputLevel[k] = -1.0 * k;
        }
        const auto driven = std::make_shared<const Program>(busOn);
        inst.setProgram(driven);
        for (const Zone& z : prog.zones) inst.setSample(z.layers[0].id, Sample::mono(kRate, noise(4000, 9)));
        Outs out;
        out.resize(256);
        std::vector<Event> ev(4);
        const uint32_t masks[] = {0x00u, 0x85u, 0xFFu, 0x04u, 0x01u, 0x80u};
        const Layout layouts[] = {{kOuts, 0}, {kOuts, 0x04u}, {1, 0}, {kOuts, 0x81u}, {0, 0}};
        long long total = 0;
        int changesMade = 0;
        const long long before = newCount().load();
        for (int b = 0; b < 3000; ++b) {
            // Program swaps run between blocks, on the host's side, and
            // allocate (the compiled program): counted apart, as in C5.
            const long long pause = newCount().load();
            if (b % 400 == 0) inst.setProgram(driven);
            if (b % 400 == 200) inst.setProgram(plain);
            total -= newCount().load() - pause;
            if (b % 7 == 0) {
                inst.setRoutedOutputs(masks[(b / 7) % 6]);
                ++changesMade;
            }
            int ne = 0;
            if (b % 3 == 0) ev[static_cast<size_t>(ne++)] = Event{0, Event::NoteOn, static_cast<uint8_t>(36 + (b * 7) % 48), 100};
            if (b % 5 == 0) ev[static_cast<size_t>(ne++)] = Event{10, Event::NoteOff, static_cast<uint8_t>(36 + ((b - 20) * 7 + 480) % 48), 0};
            const Layout& lay = layouts[(b / 11) % 5];
            if (lay.numBuses == 0) {
                inst.process(int64_t(b) * 256, ev.data(), ne, out.l[0].data(), out.r[0].data(), 256);
            } else {
                float* ch[kOuts][2];
                float* const* buses[kOuts];
                for (int k = 0; k < kOuts; ++k) {
                    ch[k][0] = out.l[k].data();
                    ch[k][1] = out.r[k].data();
                    buses[k] = (k > 0 && (lay.absent & (1u << (k - 1)))) ? nullptr : ch[k];
                }
                inst.process(int64_t(b) * 256, ev.data(), ne, buses, lay.numBuses, 256);
            }
        }
        total += newCount().load() - before;
        note("3000 blocks, " + std::to_string(changesMade) + " routing changes over six masks, five buffer layouts "
             "(all nine, aux 3 null, main only, aux 1 and aux 8 null, two-pointer), the bus drive on and off, stealing: "
             "operator new " + std::to_string(total) + " times");
        check("zero allocations in process() and setRoutedOutputs()", total == 0);
    }

    // ---------------------------------------------------------------- cost ---
    group("Cost, ns per output frame (Release; one voice: 1 layer, stereo, 44.1 kHz source, +7 st, looping)");
    {
#ifdef NDEBUG
        const bool release = true;
#else
        const bool release = false;
#endif
        if (!release) note("NOTE: not a Release build; these numbers are not the ones to record.");
        if (!bench) {
            note("run `outputs_tests --bench` for the numbers (skipped in the suite: timing is not a pass/fail matter here)");
        } else {
            // Each case: the instrument with all nine buffers handed in, minus
            // the same calls with nothing sounding (as fx_tests' costNs).
            auto caseNs = [](int output, uint32_t mask, double busDrive, bool voice) {
                Instrument inst;
                inst.prepare(48000.0);
                Program prog;
                prog.busDrive = busDrive;
                for (double& d : prog.auxBusDrive) d = busDrive;  // every output at the same amount, as before 2026-11-01
                Zone z = fullZone(PlayMode::Loop, 60);
                z.output = output;
                addZone(prog, z);
                install(inst, prog);
                const auto src = noise(44100 * 4, 11);
                inst.setSample(prog.zones[0].layers[0].id, Sample::stereo(44100.0, src, noise(src.size(), 12)));
                inst.setRoutedOutputs(mask);
                Outs out;
                out.resize(256);
                float* ch[kOuts][2];
                float* const* buses[kOuts];
                for (int k = 0; k < kOuts; ++k) {
                    ch[k][0] = out.l[k].data();
                    ch[k][1] = out.r[k].data();
                    buses[k] = ch[k];
                }
                inst.reset();
                Event ev{0, Event::NoteOn, 67, 100};
                inst.process(0, &ev, voice ? 1 : 0, buses, kOuts, 256);
                return timeNs([&](int b) { inst.process(int64_t(b + 1) * 256, nullptr, 0, buses, kOuts, 256); },
                              static_cast<int>(10.0 * 48000.0 / 256));
            };
            const double idle = caseNs(0, 0, 0.0, false);
            note(fmt("process() with nine outputs and nothing sounding: %.1f ns", idle));
            note(fmt("one voice on main, nothing routed: %.1f ns", caseNs(0, 0, 0.0, true) - idle));
            note(fmt("one voice on aux 3, aux 3 routed: %.1f ns", caseNs(3, kAux3, 0.0, true) - idle));
            note(fmt("one voice on aux 3, not routed (falls back to main): %.1f ns", caseNs(3, 0, 0.0, true) - idle));
            const double busIdle = caseNs(0, 0, 0.5, false) - idle;
            note(fmt("busDrive 0.5, nothing sounding, nothing routed (main's bus alone): %.1f ns", busIdle));
            note(fmt("busDrive 0.5, nothing sounding, every aux routed (nine buses): %.1f ns", caseNs(0, 0xFF, 0.5, false) - idle));
            note(fmt("one voice on aux 3, aux 3 routed, every bus at 0.5: %.1f ns", caseNs(3, kAux3, 0.5, true) - idle));
        }
    }

    return summary("outputs_tests");
}
