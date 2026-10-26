// overlay_tests.cpp - C8: libraries, a read-only base with your edits on top
// (DESIGN §8, §12).
//
// C8.1 base v3 + an overlay that edits zone 17: what plays equals a program
//      with the edit made directly (cmp).
// C8.2 base v4 drops zone 17: the edit is kept, reported as orphaned, and
//      nothing else changes.
// C8.3 flatten yields a program that renders byte-identical to base + overlay
//      and refers to no base.
// Also: the overlay format round-trips byte-identically with unknown
//      attributes and verbs; edits that cannot apply are reported and leave
//      the program as if they were not there.
//
// The library: three zones on keys 36, 38 and 40 (ids 5, 9, 17), each with
// one layer (ids 6, 10, 18) playing its own sine. Version 4 is version 3
// without zone 17. The overlay edits zone 17 (attack, tune, level) and layer
// 6 (tune), and carries a verb from a newer writer.

#include "nd_edit.h"
#include "nd_instrument.h"
#include "nd_overlay.h"
#include "nd_program_xml.h"
#include "render_util.h"
#include "test_util.h"

#include <cstring>

using namespace nd;
using namespace ndtest;

namespace {

constexpr double kRate = 48000.0;

const char* kBaseV3 = R"(<NassauDigitalProgram version="1" name="Library Kit" polyphony="32" nextId="19">
  <zone id="5" keyLo="36" keyHi="36" rootKey="36" mode="gated" release="0.05">
    <layer id="6" src="audio/a.wav"/>
  </zone>
  <zone id="9" keyLo="38" keyHi="38" rootKey="38" mode="gated" release="0.05">
    <layer id="10" src="audio/b.wav"/>
  </zone>
  <zone id="17" keyLo="40" keyHi="40" rootKey="40" mode="gated" release="0.05">
    <layer id="18" src="audio/c.wav"/>
  </zone>
</NassauDigitalProgram>
)";

const char* kBaseV4 = R"(<NassauDigitalProgram version="1" name="Library Kit" polyphony="32" nextId="19">
  <zone id="5" keyLo="36" keyHi="36" rootKey="36" mode="gated" release="0.05">
    <layer id="6" src="audio/a.wav"/>
  </zone>
  <zone id="9" keyLo="38" keyHi="38" rootKey="38" mode="gated" release="0.05">
    <layer id="10" src="audio/b.wav"/>
  </zone>
</NassauDigitalProgram>
)";

const char* kOverlay = R"(<NassauDigitalOverlay version="1" base="Library/Kit.ndp" baseVersion="3" colour="teal">
  <set-zone zone="17" attack="0.25" tune="-2" level="-6"/>
  <set-layer layer="6" tune="3"/>
  <set-swing zone="9" amount="0.6"/>
</NassauDigitalOverlay>
)";

Program parsed(const char* text) {
    const ProgramParseResult r = parseProgram(text);
    if (!r.ok) note("did not parse: " + r.error);
    return r.program;
}

// Renders a program with each layer's sine installed: three notes, held.
Stereo play(const Program& prog) {
    Instrument inst;
    inst.prepare(kRate);
    install(inst, prog);
    inst.setSample(6, Sample::mono(kRate, sine(220.0, kRate, 1.0)));
    inst.setSample(10, Sample::mono(kRate, sine(330.0, kRate, 1.0)));
    inst.setSample(18, Sample::mono(kRate, sine(440.0, kRate, 1.0)));
    return render(inst, {on(0, 36), on(4800, 38), on(9600, 40), off(30000, 36), off(30000, 38), off(30000, 40)},
                  48000);
}

bool same(const Stereo& x, const Stereo& y) {
    return x.l.size() == y.l.size() && std::memcmp(x.l.data(), y.l.data(), x.l.size() * sizeof(float)) == 0 &&
           std::memcmp(x.r.data(), y.r.data(), x.r.size() * sizeof(float)) == 0;
}

// The overlay's edits made by hand, through the ordinary edit calls.
bool editZone17(Program& p) {
    return setZones(p, ZoneScope::zones({17}),
                    {ParamEdit::absolute("attack", "0.25"), ParamEdit::absolute("tune", "-2"),
                     ParamEdit::absolute("level", "-6")})
        .ok;
}
bool editLayer6(Program& p) { return setLayers(p, {6}, {ParamEdit::absolute("tune", "3")}).ok; }

const char* stateName(OverlayEditState s) {
    switch (s) {
        case OverlayEditState::Applied: return "applied";
        case OverlayEditState::Orphaned: return "orphaned";
        case OverlayEditState::Refused: return "refused";
        case OverlayEditState::Unknown: return "unknown";
    }
    return "?";
}

std::string describe(const OverlayResult& r) {
    std::string s;
    for (const auto& e : r.edits) {
        if (!s.empty()) s += ", ";
        s += "#" + std::to_string(e.index) + " " + stateName(e.state);
        if (e.state == OverlayEditState::Orphaned) s += " (id " + std::to_string(e.missingId) + ")";
    }
    return s;
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: libraries, base and overlay (C8) ===\n");

    const Program v3 = parsed(kBaseV3);
    const Program v4 = parsed(kBaseV4);
    const OverlayParseResult op = parseOverlay(kOverlay);

    group("The overlay format (DESIGN §12)");
    {
        check("the overlay parses", op.ok);
        if (!op.ok) note(op.error);
        const Overlay& o = op.overlay;
        check("base, baseVersion and three edits are read", o.base == "Library/Kit.ndp" && o.baseVersion == "3" &&
                                                                o.edits.size() == 3);
        const std::string written = writeOverlay(o);
        const OverlayParseResult again = parseOverlay(written);
        check("parse -> write -> parse is the identity", again.ok && again.overlay == o);
        check("write is byte-stable", again.ok && writeOverlay(again.overlay) == written);
        check("an unknown root attribute and an unknown verb survive the round trip",
              written.find("colour=\"teal\"") != std::string::npos &&
                  written.find("<set-swing zone=\"9\" amount=\"0.6\"/>") != std::string::npos);
        check("another root element is refused", !parseOverlay(kBaseV3).ok);
    }

    // ---------------------------------------------------------------- C8.1 ---
    group("C8.1 base v3 + an overlay that edits zone 17 plays what the edit made directly plays (cmp)");
    {
        const Program v3Before = v3;
        const OverlayResult r = applyOverlay(v3, op.overlay);
        note("edits: " + describe(r));
        Program direct = v3;
        const bool editsOk = editZone17(direct) && editLayer6(direct);
        check("the direct edits apply", editsOk);
        check("zone 17's and layer 6's edits are applied, the newer writer's verb is skipped as unknown",
              r.edits.size() == 3 && r.edits[0].state == OverlayEditState::Applied &&
                  r.edits[1].state == OverlayEditState::Applied && r.edits[2].state == OverlayEditState::Unknown);
        check("the applied program equals the directly edited one (structural)", r.program == direct);
        const Stereo a = play(r.program), b = play(direct), base = play(v3);
        const double diff = rms(a.l, 9600, 30000) > 0 ? std::fabs(rms(a.l, 9600, 30000) - rms(base.l, 9600, 30000)) : 0;
        note("RMS from key 40's note-on: base alone " + std::to_string(rms(base.l, 9600, 30000)) + ", base + overlay " +
             std::to_string(rms(a.l, 9600, 30000)) + ", direct edit " + std::to_string(rms(b.l, 9600, 30000)));
        check("base + overlay renders byte-identical to the direct edit (cmp)", same(a, b));
        check("...and the edit is audible: the base alone renders differently", !same(a, base) && diff > 1e-3);
        check("the base is not written", v3 == v3Before);
    }

    // ---------------------------------------------------------------- C8.2 ---
    group("C8.2 base v4 drops zone 17: the edit is kept, reported as orphaned, nothing else changes");
    {
        const std::string overlayText = writeOverlay(op.overlay);
        const OverlayResult r = applyOverlay(v4, op.overlay);
        note("edits on v4: " + describe(r));
        const std::vector<size_t> orphans = r.orphaned();
        check("exactly the zone-17 edit is reported as orphaned, naming id 17",
              orphans.size() == 1 && orphans[0] == 0 && r.edits[0].missingId == 17);
        check("the other edits are as on v3 (layer 6 applied, the unknown verb unknown)",
              r.edits.size() == 3 && r.edits[1].state == OverlayEditState::Applied &&
                  r.edits[2].state == OverlayEditState::Unknown);
        Program direct = v4;
        editLayer6(direct);
        check("nothing else changes: v4 + overlay equals v4 with the layer-6 edit made directly (structural)",
              r.program == direct);
        check("...and renders byte-identical to it (cmp)", same(play(r.program), play(direct)));
        check("the edit is kept: the overlay writes the same bytes after the apply",
              writeOverlay(op.overlay) == overlayText && op.overlay.edits[0].name == "set-zone");
        const OverlayResult back = applyOverlay(v3, op.overlay);
        check("a base that has zone 17 again gets the kept edit applied", back.orphaned().empty() &&
                                                                          back.edits[0].state == OverlayEditState::Applied);
        // A dropped layer orphans a set-layer the same way.
        Program v5 = v3;
        removeLayer(v5, 6);
        const OverlayResult l = applyOverlay(v5, op.overlay);
        check("a dropped layer orphans its set-layer (id 6), and zone 17's edit still applies",
              l.orphaned().size() == 1 && l.orphaned()[0] == 1 && l.edits[1].missingId == 6 &&
                  l.edits[0].state == OverlayEditState::Applied);
    }

    // ---------------------------------------------------------------- C8.3 ---
    group("C8.3 flatten renders byte-identical to base + overlay and refers to no base");
    {
        const OverlayResult played = applyOverlay(v3, op.overlay);
        const OverlayResult flat = flatten(v3, op.overlay);
        const std::string text = writeProgram(flat.program);
        const ProgramParseResult reread = parseProgram(text);
        check("the flattened program is written and read back as an ordinary program", reread.ok);
        check("it refers to no base: no overlay element, no base or baseVersion attribute, not the library's path",
              text.find(kOverlayElement) == std::string::npos && text.find("base=") == std::string::npos &&
                  text.find("baseVersion") == std::string::npos && text.find("Library/Kit.ndp") == std::string::npos);
        check("read back, it equals base + overlay (structural)", reread.ok && reread.program == played.program);
        check("read back, it renders byte-identical to base + overlay (cmp)",
              reread.ok && same(play(reread.program), play(played.program)));
        note("flatten's report: " + describe(flat));
        const OverlayResult flat4 = flatten(v4, op.overlay);
        check("flattening onto v4 reports the orphaned edit too (it is not silently lost)",
              flat4.orphaned().size() == 1 && flat4.orphaned()[0] == 0);
    }

    group("Edits that cannot apply are reported and change nothing");
    {
        struct Case {
            const char* verb;
            OverlayEditState expect;
        };
        const Case cases[] = {
            {R"(<set-zone zone="9" tune="+2"/>)", OverlayEditState::Refused},        // relative: not defined here
            {R"(<set-zone zone="9" tune="500"/>)", OverlayEditState::Refused},       // out of range
            {R"(<set-zone zone="9" wobble="1"/>)", OverlayEditState::Refused},       // unknown parameter
            {R"(<set-zone zone="nine" tune="1"/>)", OverlayEditState::Refused},      // malformed address
            {R"(<set-layer tune="1"/>)", OverlayEditState::Refused},                 // no address
            {R"(<set-zone zone="9" tune="1" mode="tape"/>)", OverlayEditState::Refused},  // all-or-nothing
            {R"(<set-zone zone="99" tune="1"/>)", OverlayEditState::Orphaned},
        };
        bool allAsExpected = true, allUnchanged = true;
        for (const Case& c : cases) {
            const std::string text = std::string("<NassauDigitalOverlay version=\"1\">") + c.verb +
                                     "</NassauDigitalOverlay>";
            const OverlayParseResult o = parseOverlay(text);
            if (!o.ok) {
                allAsExpected = false;
                continue;
            }
            const OverlayResult r = applyOverlay(v3, o.overlay);
            const bool asExpected = r.edits.size() == 1 && r.edits[0].state == c.expect;
            allAsExpected = allAsExpected && asExpected;
            allUnchanged = allUnchanged && r.program == v3;
            note(std::string(c.verb) + " -> " + stateName(r.edits.empty() ? OverlayEditState::Applied : r.edits[0].state) +
                 (r.edits.empty() || r.edits[0].error.empty() ? "" : ": " + r.edits[0].error));
        }
        check("each is reported as expected (refused or orphaned)", allAsExpected);
        check("none of them changes the program", allUnchanged);
        const OverlayParseResult all = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><set-zone zone="all" level="-3"/></NassauDigitalOverlay>)");
        const OverlayResult r = applyOverlay(v4, all.overlay);
        Program direct = v4;
        setZones(direct, ZoneScope::everyZone(), {ParamEdit::absolute("level", "-3")});
        check("zone=\"all\" reaches every zone of the base it is applied to", all.ok && r.program == direct &&
                                                                               r.edits[0].state == OverlayEditState::Applied);
    }

    return summary("overlay_tests");
}
