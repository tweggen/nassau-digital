// overlay_tests.cpp - C8: libraries, a read-only base with your edits on top
// (DESIGN §8, §12).
//
// C8.1 base v3 + an overlay that edits zone 17: what plays equals a program
//      with the edit made directly (cmp).
// C8.2 base v4 drops zone 17: the edit is kept, reported as orphaned, and
//      nothing else changes.
// C8.3 flatten yields a program that renders byte-identical to base + overlay
//      and refers to no base.
// Previous values (decided 2026-10-27, DESIGN §8):
//      recording an edit, applying it and undoing it gives back the program
//      and the render from before (cmp); a base that changed a value under an
//      edit (v5) still plays the overlay's value and reports the edit drifted
//      with was / now; an ALL edit is expanded into one set-zone per zone,
//      each with its own previous value.
// Also: the overlay format round-trips byte-identically with unknown
//      attributes and verbs; edits that cannot apply are reported and leave
//      the program as if they were not there.
// The author's decisions of 2026-11-01 (DESIGN §8, §13):
//      libraryVersion: an overlay records its base's version; on another
//      version it still applies and reports the mismatch (baseDrift, the
//      shape drift uses); zone lists ("5 9 17") are expanded into one
//      set-zone per zone, and a missing zone orphans only its own; set-program
//      edits program attributes with `.was` and drift; add-zone / add-layer
//      take ids from 1000000 on, which a newer base adding zones never
//      collides with; hide-zone hides a base zone and hide-layer a layer,
//      flatten drops them; rebase accepts a new base (baseVersion and the
//      drifted .was refreshed, undoable); every new
//      edit kind is undone exactly (cmp) and orphaned like the others.
// The author's decision of 2026-11-04 (QBX-278): set-pad reassigns one pad
//      with key.was (row, default 36 + n, or "none"), drift reported, never
//      orphaned, refreshed by rebase, flattened into the <Pads> table, undone
//      exactly (the table's rows and order).
//
// The library: three zones on keys 36, 38 and 40 (ids 5, 9, 17; tune 1, -3
// and the default 0), each with one layer (ids 6, 10, 18) playing its own
// sine. Version 4 is version 3 without zone 17; version 5 is version 3 with
// zone 17's attack changed. The overlay edits zone 17 (attack, tune, level)
// and layer 6 (tune), each value with the one it replaces, and carries a verb
// from a newer writer. Zone 17's attack is not written in the base, so its
// `.was` is the default, 0.

#include "nd_edit.h"
#include "nd_instrument.h"
#include "nd_overlay.h"
#include "nd_program_xml.h"
#include "render_util.h"
#include "test_util.h"

#include <cstring>
#include <set>

using namespace nd;
using namespace ndtest;

namespace {

constexpr double kRate = 48000.0;

const char* kBaseV3 = R"(<NassauDigitalProgram version="1" name="Library Kit" polyphony="32" nextId="19">
  <zone id="5" keyLo="36" keyHi="36" rootKey="36" mode="gated" release="0.05" tune="1">
    <layer id="6" src="audio/a.wav"/>
  </zone>
  <zone id="9" keyLo="38" keyHi="38" rootKey="38" mode="gated" release="0.05" tune="-3">
    <layer id="10" src="audio/b.wav"/>
  </zone>
  <zone id="17" keyLo="40" keyHi="40" rootKey="40" mode="gated" release="0.05">
    <layer id="18" src="audio/c.wav"/>
  </zone>
</NassauDigitalProgram>
)";

const char* kBaseV5 = R"(<NassauDigitalProgram version="1" name="Library Kit" polyphony="32" nextId="19">
  <zone id="5" keyLo="36" keyHi="36" rootKey="36" mode="gated" release="0.05" tune="1">
    <layer id="6" src="audio/a.wav"/>
  </zone>
  <zone id="9" keyLo="38" keyHi="38" rootKey="38" mode="gated" release="0.05" tune="-3">
    <layer id="10" src="audio/b.wav"/>
  </zone>
  <zone id="17" keyLo="40" keyHi="40" rootKey="40" mode="gated" release="0.05" attack="0.05">
    <layer id="18" src="audio/c.wav"/>
  </zone>
</NassauDigitalProgram>
)";

const char* kBaseV4 = R"(<NassauDigitalProgram version="1" name="Library Kit" polyphony="32" nextId="19">
  <zone id="5" keyLo="36" keyHi="36" rootKey="36" mode="gated" release="0.05" tune="1">
    <layer id="6" src="audio/a.wav"/>
  </zone>
  <zone id="9" keyLo="38" keyHi="38" rootKey="38" mode="gated" release="0.05" tune="-3">
    <layer id="10" src="audio/b.wav"/>
  </zone>
</NassauDigitalProgram>
)";

const char* kOverlay = R"(<NassauDigitalOverlay version="1" base="Library/Kit.ndp" baseVersion="3" colour="teal">
  <set-zone zone="17" attack="0.25" attack.was="0" tune="-2" tune.was="0" level="-6" level.was="0"/>
  <set-layer layer="6" tune="3" tune.was="0"/>
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

// The same with a fourth key (42) and the samples of the layers an overlay
// adds (ids 1000001, 1000002, 1000003).
Stereo playMore(const Program& prog) {
    Instrument inst;
    inst.prepare(kRate);
    install(inst, prog);
    inst.setSample(6, Sample::mono(kRate, sine(220.0, kRate, 1.0)));
    inst.setSample(10, Sample::mono(kRate, sine(330.0, kRate, 1.0)));
    inst.setSample(18, Sample::mono(kRate, sine(440.0, kRate, 1.0)));
    inst.setSample(1000001, Sample::mono(kRate, sine(550.0, kRate, 1.0)));
    inst.setSample(1000002, Sample::mono(kRate, sine(660.0, kRate, 1.0)));
    inst.setSample(1000003, Sample::mono(kRate, sine(770.0, kRate, 1.0)));
    return render(inst,
                  {on(0, 36), on(4800, 38), on(9600, 40), on(14400, 42), off(30000, 36), off(30000, 38),
                   off(30000, 40), off(30000, 42)},
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
        case OverlayEditState::Drifted: return "drifted";
    }
    return "?";
}

std::string describe(const OverlayResult& r) {
    std::string s;
    for (const auto& e : r.edits) {
        if (!s.empty()) s += ", ";
        s += "#" + std::to_string(e.index) + " " + stateName(e.state);
        if (e.state == OverlayEditState::Orphaned) s += " (id " + std::to_string(e.missingId) + ")";
        for (const auto& d : e.drift) s += " (" + d.attr + " was " + d.was + ", now " + d.now + ")";
    }
    return s;
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: libraries, base and overlay (C8) ===\n");

    const Program v3 = parsed(kBaseV3);
    const Program v4 = parsed(kBaseV4);
    const Program v5 = parsed(kBaseV5);
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
            {R"(<set-zone zone="9" tune="+2" tune.was="-3"/>)", OverlayEditState::Refused},  // a stored relative value
            {R"(<set-zone zone="9" tune="1" tune.was="+0"/>)", OverlayEditState::Refused},   // ...in .was too
            {R"(<set-zone zone="9" tune="1"/>)", OverlayEditState::Refused},                 // no .was
            {R"(<set-zone zone="9" tune.was="-3"/>)", OverlayEditState::Refused},            // .was alone
            {R"(<set-zone zone="9" tune="500" tune.was="-3"/>)", OverlayEditState::Refused}, // out of range
            {R"(<set-zone zone="9" tune="1" tune.was="x"/>)", OverlayEditState::Refused},    // malformed .was
            {R"(<set-zone zone="9" wobble="1" wobble.was="0"/>)", OverlayEditState::Refused},  // unknown parameter
            {R"(<set-zone zone="nine" tune="1" tune.was="0"/>)", OverlayEditState::Refused}, // malformed address
            {R"(<set-layer tune="1" tune.was="0"/>)", OverlayEditState::Refused},            // no address
            {R"(<set-zone zone="all" tune="1" tune.was="0"/>)", OverlayEditState::Refused},  // needs expansion
            {R"(<set-zone zone="9" tune="1" tune.was="-3" mode="tape" mode.was="gated"/>)",
             OverlayEditState::Refused},                                                     // all-or-nothing
            {R"(<set-zone zone="99" tune="1" tune.was="0"/>)", OverlayEditState::Orphaned},
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
    }

    // --------------------------------------------------- record and undo ---
    group("Previous values: record an edit, apply it, undo it: the program and render from before (cmp)");
    {
        Overlay o = op.overlay;
        Program current = applyOverlay(v3, o).program;
        const Program before = current;
        const std::string textBefore = writeOverlay(o);
        const Stereo r0 = play(current);
        // A relative edit on zone 17, whose tune the overlay already set to -2.
        const RecordedEdit rec = recordZoneEdit(o, current, ZoneScope::zones({17}),
                                                {ParamEdit::by("tune", 2.0), ParamEdit::absolute("release", "0.2")});
        const std::string verb = rec.ok && rec.count == 1 ? writeXml(o.edits.back()) : std::string();
        note("recorded: " + verb.substr(0, verb.size() ? verb.size() - 1 : 0));
        check("one set-zone, the relative +2 resolved to absolute values beside the replaced ones",
              rec.ok && rec.count == 1 &&
                  verb == "<set-zone zone=\"17\" tune=\"0\" tune.was=\"-2\" release=\"0.2\" release.was=\"0.05\"/>\n");
        const OverlayResult applied = applyOverlay(v3, o);
        note("applied: " + describe(applied));
        check("applied to the base it was made on: what the edit made, nothing drifted",
              applied.program == current && applied.drifted().empty() && applied.edits.size() == 4);
        const Stereo r1 = play(applied.program);
        note("RMS from key 40's note-on: before " + std::to_string(rms(r0.l, 9600, 48000)) + ", with the edit " +
             std::to_string(rms(r1.l, 9600, 48000)));
        check("...and it is audible", !same(r0, r1));
        Overlay o2 = o;
        Program c2 = current;
        const RecordedEdit later = recordLayerEdit(o2, c2, {10}, {ParamEdit::absolute("level", "-1")});
        check("an edit that is not the last one is not undone", later.ok && !undoRecordedEdit(o2, c2, rec));
        const bool undone = undoRecordedEdit(o, current, rec);
        check("the undo applies", undone);
        check("undo: the overlay writes the same bytes as before the edit", writeOverlay(o) == textBefore);
        check("undo: the program the editor holds equals the one before (the .was values written back)", current == before);
        const OverlayResult again = applyOverlay(v3, o);
        check("undo: base + overlay equals the program before (structural)", again.program == before);
        check("undo: and renders byte-identical to before (cmp)", same(play(again.program), r0));
    }

    // ----------------------------------------------------------- drift ---
    group("Previous values: base v5 changes zone 17's attack under the edit: applied, reported drifted");
    {
        const OverlayResult r = applyOverlay(v5, op.overlay);
        note("edits on v5: " + describe(r));
        const std::vector<size_t> d = r.drifted();
        check("exactly edit #0 is reported drifted, attack only, was 0 (the default, the base wrote none) now 0.05",
              d.size() == 1 && d[0] == 0 && r.edits[0].drift.size() == 1 &&
                  r.edits[0].drift[0] == OverlayDrift{"attack", "0", "0.05"});
        Program direct = v5;
        editZone17(direct);
        editLayer6(direct);
        check("the overlay's attack plays: v5 + overlay equals v5 with the edits made directly (structural)",
              r.program == direct && r.program.findZone(17)->attack == 0.25);
        const Stereo a = play(r.program), b = play(direct), base5 = play(v5);
        check("...and renders byte-identical to it (cmp)", same(a, b));
        check("...which differs from v5 alone", !same(a, base5));
        // The comparison is on the value as the format writes it, not on the text.
        const OverlayParseResult same0 = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><set-zone zone="17" attack="0.25" attack.was="0.000"/>)"
            R"(<set-zone zone="5" tune="2" tune.was="1.0e0"/></NassauDigitalOverlay>)");
        const OverlayResult s0 = applyOverlay(v3, same0.overlay);
        check("\"0.000\" and \"1.0e0\" match 0 and 1: no drift from spelling",
              same0.ok && s0.drifted().empty() && s0.edits[0].state == OverlayEditState::Applied &&
                  s0.edits[1].state == OverlayEditState::Applied);
        const OverlayParseResult tiny = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><set-zone zone="5" tune="2" tune.was="1.0000000000000002"/></NassauDigitalOverlay>)");
        const OverlayResult t0 = applyOverlay(v3, tiny.overlay);
        check("a .was one ulp from the value is drift (exact, no tolerance)", tiny.ok && t0.drifted().size() == 1);
    }

    // ----------------------------------------------------- ALL expansion ---
    group("Previous values: an ALL edit becomes one set-zone per zone, each with its own .was");
    {
        Overlay o;
        Program current = v3;
        const RecordedEdit rec = recordZoneEdit(o, current, ZoneScope::everyZone(), {ParamEdit::by("tune", 2.0)});
        std::string verbs;
        for (const XmlNode& v : o.edits) verbs += writeXml(v);
        note("recorded:\n" + verbs);
        check("three set-zones, zone 5: 1 -> 3, zone 9: -3 -> -1, zone 17: 0 -> 2",
              rec.ok && rec.count == 3 &&
                  verbs == "<set-zone zone=\"5\" tune=\"3\" tune.was=\"1\"/>\n"
                           "<set-zone zone=\"9\" tune=\"-1\" tune.was=\"-3\"/>\n"
                           "<set-zone zone=\"17\" tune=\"2\" tune.was=\"0\"/>\n");
        Program direct = v3;
        setZones(direct, ZoneScope::everyZone(), {ParamEdit::by("tune", 2.0)});
        const OverlayResult r = applyOverlay(v3, o);
        check("applied on v3: equals the ALL edit made directly, nothing drifted",
              r.program == direct && current == direct && r.drifted().empty());
        check("...and renders byte-identical to it (cmp)", same(play(r.program), play(direct)));
        // A zone a later base adds is not covered.
        Program v6 = v3;
        Zone z20 = v3.zones[0];
        z20.keyLo = z20.keyHi = z20.rootKey = 42;
        z20.tune = 5.0;
        const uint32_t id20 = addZone(v6, z20);
        const OverlayResult r6 = applyOverlay(v6, o);
        check("a zone a later base adds keeps its own tune (5), the three expanded edits apply",
              r6.program.findZone(id20) && r6.program.findZone(id20)->tune == 5.0 && r6.drifted().empty() &&
                  r6.orphaned().empty() && r6.program.findZone(9)->tune == -1.0);
        const bool undone = undoRecordedEdit(o, current, rec);
        check("undo of the ALL edit: empty overlay, v3 again", undone && o.edits.empty() && current == v3);
    }

    // ================================ the author's decisions of 2026-11-01 ===

    // ------------------------------------------------------ libraryVersion ---
    group("2026-11-01 libraryVersion: the overlay records its base's; another version still applies, reported");
    {
        Program lib3 = v3, lib31 = v3;
        lib3.libraryVersion = "3";
        lib31.libraryVersion = "3.1";  // the same content, another version: isolates the report
        Overlay o;
        Program current = lib3;
        const RecordedEdit rec = recordZoneEdit(o, current, ZoneScope::zones({9}), {ParamEdit::absolute("tune", "2")});
        check("the first recorded edit records the base's libraryVersion as baseVersion (\"3\")",
              rec.ok && rec.stampedBaseVersion && o.baseVersion == "3");
        const RecordedEdit rec2 = recordLayerEdit(o, current, {6}, {ParamEdit::absolute("level", "-2")});
        check("a later edit does not re-stamp it", rec2.ok && !rec2.stampedBaseVersion && o.baseVersion == "3");
        const OverlayResult same3 = applyOverlay(lib3, o);
        check("on the version it was made on: no mismatch, every edit applied",
              !same3.versionMismatch() && same3.drifted().empty() && same3.orphaned().empty() &&
                  same3.program == current);
        const OverlayResult on31 = applyOverlay(lib31, o);
        note("on 3.1: baseDrift " + (on31.baseDrift.empty() ? std::string("none")
                                                              : on31.baseDrift[0].attr + " was " + on31.baseDrift[0].was +
                                                                    ", now " + on31.baseDrift[0].now) +
             "; edits: " + describe(on31));
        check("on 3.1: the mismatch is reported as {libraryVersion, was 3, now 3.1}",
              on31.baseDrift.size() == 1 && on31.baseDrift[0] == OverlayDrift{"libraryVersion", "3", "3.1"});
        Program expect31 = current;
        expect31.libraryVersion = "3.1";
        check("...and the overlay still applies: every edit applied, the program is 3.1 with the edits (structural)",
              on31.edits.size() == 2 && on31.edits[0].state == OverlayEditState::Applied &&
                  on31.edits[1].state == OverlayEditState::Applied && on31.program == expect31);
        check("...and renders byte-identical to the edits on 3 (cmp)", same(play(on31.program), play(same3.program)));
        const OverlayResult onNone = applyOverlay(v3, o);
        check("on a base without a libraryVersion: reported, was 3, now empty",
              onNone.baseDrift.size() == 1 && onNone.baseDrift[0] == OverlayDrift{"libraryVersion", "3", ""});
        Overlay unversioned = o;
        unversioned.baseVersion.clear();
        check("an overlay without a baseVersion is not compared", !applyOverlay(lib31, unversioned).versionMismatch());
        const std::string text = writeOverlay(o);
        const OverlayParseResult back = parseOverlay(text);
        check("baseVersion=\"3\" is written and reads back, the bytes stable",
              text.find(" baseVersion=\"3\"") != std::string::npos && back.ok && back.overlay == o &&
                  writeOverlay(back.overlay) == text);
        const OverlayResult flat = flatten(lib3, o);
        check("flatten: a program of your own carries no libraryVersion (written: none)",
              flat.program.libraryVersion.empty() &&
                  writeProgram(flat.program).find("libraryVersion") == std::string::npos);
        Program c2 = current;
        Overlay o2 = o;
        const RecordedEdit lv = recordProgramEdit(o2, c2, {ParamEdit::absolute("libraryVersion", "4")});
        const RecordedEdit fv = recordProgramEdit(o2, c2, {ParamEdit::absolute("version", "2")});
        check("an overlay does not edit libraryVersion or version: recordProgramEdit refuses, nothing changes",
              !lv.ok && !fv.ok && o2 == o && c2 == current);
        const OverlayParseResult stored = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><set-program libraryVersion="4" libraryVersion.was="3"/>)"
            R"(<set-program version="2" version.was="1"/></NassauDigitalOverlay>)");
        const OverlayResult sr = applyOverlay(lib3, stored.overlay);
        check("...and a stored set-program of either is refused, the program unchanged",
              stored.ok && sr.edits.size() == 2 && sr.edits[0].state == OverlayEditState::Refused &&
                  sr.edits[1].state == OverlayEditState::Refused && sr.program == lib3);
        check("undo of the edits: the first one's undo takes the baseVersion back out (an empty overlay again)",
              undoRecordedEdit(o, current, rec2) && undoRecordedEdit(o, current, rec) && o == Overlay{} &&
                  current == lib3);
    }

    // ---------------------------------------------------------- zone lists ---
    group("2026-11-01 zone lists: zone=\"5 9 17\" becomes one set-zone per zone; a missing zone orphans only its own");
    {
        ZoneScope scope;
        std::string err;
        const bool parsedList = parseZoneScope("5 9 17", scope, err);
        Overlay o;
        Program current = v3;
        const RecordedEdit rec = parsedList ? recordZoneEdit(o, current, scope, {ParamEdit::by("tune", 2.0)})
                                            : RecordedEdit{};
        std::string verbs;
        for (const XmlNode& v : o.edits) verbs += writeXml(v);
        note("recorded:\n" + verbs);
        check("three set-zones, each with its own .was: 5: 1 -> 3, 9: -3 -> -1, 17: 0 -> 2",
              rec.ok && rec.count == 3 &&
                  verbs == "<set-zone zone=\"5\" tune=\"3\" tune.was=\"1\"/>\n"
                           "<set-zone zone=\"9\" tune=\"-1\" tune.was=\"-3\"/>\n"
                           "<set-zone zone=\"17\" tune=\"2\" tune.was=\"0\"/>\n");
        Program direct = v3;
        setZones(direct, ZoneScope::zones({5, 9, 17}), {ParamEdit::by("tune", 2.0)});
        const OverlayResult r3 = applyOverlay(v3, o);
        check("on v3: equals the list edit made directly, nothing drifted (structural and cmp)",
              r3.program == direct && current == direct && r3.drifted().empty() &&
                  same(play(r3.program), play(direct)));
        const OverlayResult r4 = applyOverlay(v4, o);
        note("on v4: " + describe(r4));
        check("on v4 (no zone 17): exactly edit #2 is orphaned (id 17), #0 and #1 apply",
              r4.orphaned() == std::vector<size_t>({2}) && r4.edits[2].missingId == 17 &&
                  r4.edits[0].state == OverlayEditState::Applied && r4.edits[1].state == OverlayEditState::Applied);
        Program direct4 = v4;
        setZones(direct4, ZoneScope::zones({5, 9}), {ParamEdit::by("tune", 2.0)});
        check("...and the result is v4 with zones 5 and 9 edited (structural and cmp)",
              r4.program == direct4 && same(play(r4.program), play(direct4)));
        Overlay o2 = o;
        Program c2 = current;
        const RecordedEdit missing = recordZoneEdit(o2, c2, ZoneScope::zones({5, 99}), {ParamEdit::by("tune", 1.0)});
        check("recording a list that names a zone the program lacks (99) is refused whole, nothing changes",
              !missing.ok && o2 == o && c2 == current);
        const OverlayParseResult stored = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><set-zone zone="5 9" tune="1" tune.was="0"/></NassauDigitalOverlay>)");
        const OverlayResult sr = applyOverlay(v3, stored.overlay);
        check("a stored zone list is refused (needs expansion), the program unchanged",
              stored.ok && sr.edits.size() == 1 && sr.edits[0].state == OverlayEditState::Refused &&
                  sr.edits[0].error.find("expansion") != std::string::npos && sr.program == v3);
        check("undo: the overlay empty, v3 again", undoRecordedEdit(o, current, rec) && o.edits.empty() && current == v3);
    }

    // --------------------------------------------------------- set-program ---
    group("2026-11-01 set-program (the author's decision): program attributes with .was, drift reported");
    {
        Overlay o;
        Program current = v3;
        const std::string textBefore = writeOverlay(o);
        const Stereo r0 = play(v3);
        const RecordedEdit rec = recordProgramEdit(
            o, current, {ParamEdit::absolute("outputLevel", "-6"), ParamEdit::by("masterTune", 0.5),
                         ParamEdit::absolute("busDrive2", "0.3")});
        const std::string verb = rec.ok && rec.count == 1 ? writeXml(o.edits.back()) : std::string();
        note("recorded: " + verb.substr(0, verb.size() ? verb.size() - 1 : 0));
        check("one set-program, no address, each value beside its .was, the relative +0.5 resolved",
              verb == "<set-program outputLevel=\"-6\" outputLevel.was=\"0\" masterTune=\"0.5\" masterTune.was=\"0\" "
                      "busDrive2=\"0.3\" busDrive2.was=\"0\"/>\n");
        const OverlayResult r = applyOverlay(v3, o);
        Program direct = v3;
        setProgramParams(direct, {ParamEdit::absolute("outputLevel", "-6"), ParamEdit::absolute("masterTune", "0.5"),
                                  ParamEdit::absolute("busDrive2", "0.3")});
        check("applied on v3: equals the edit made directly, nothing drifted",
              r.program == direct && current == direct && r.drifted().empty() &&
                  r.edits[0].state == OverlayEditState::Applied);
        const Stereo r1 = play(r.program);
        note("RMS over the render: v3 " + std::to_string(rms(r0.l, 0, 48000)) + ", with set-program " +
             std::to_string(rms(r1.l, 0, 48000)));
        check("...renders byte-identical to it (cmp) and differs from v3 alone",
              same(r1, play(direct)) && !same(r1, r0));
        Program v8 = v3;
        v8.outputLevel = -3.0;  // a newer base changed the level under the edit
        const OverlayResult d = applyOverlay(v8, o);
        note("on v8: " + describe(d));
        check("on a base whose outputLevel is -3: drifted, outputLevel only, was 0 now -3",
              d.drifted() == std::vector<size_t>({0}) && d.edits[0].drift.size() == 1 &&
                  d.edits[0].drift[0] == OverlayDrift{"outputLevel", "0", "-3"});
        Program direct8 = v8;
        setProgramParams(direct8, {ParamEdit::absolute("outputLevel", "-6"), ParamEdit::absolute("masterTune", "0.5"),
                                   ParamEdit::absolute("busDrive2", "0.3")});
        check("...still applied: the overlay's -6 plays (structural and cmp)",
              d.program == direct8 && d.program.outputLevel == -6.0 && same(play(d.program), play(direct8)));
        Program c2 = current;
        Overlay o2 = o;
        const RecordedEdit poly = recordProgramEdit(o2, c2, {ParamEdit::by("polyphony", 8.0)});
        check("a relative polyphony is refused, nothing changes", !poly.ok && o2 == o && c2 == current);
        const OverlayParseResult bad = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><set-program outputLevel="-1"/>)"
            R"(<set-program zone="5" outputLevel="-1" outputLevel.was="0"/>)"
            R"(<set-program polyphony="12" polyphony.was="32"/></NassauDigitalOverlay>)");
        const OverlayResult br = applyOverlay(v3, bad.overlay);
        check("stored set-programs without .was, with an address, or out of range are refused; never orphaned",
              bad.ok && br.edits.size() == 3 && br.edits[0].state == OverlayEditState::Refused &&
                  br.edits[1].state == OverlayEditState::Refused && br.edits[2].state == OverlayEditState::Refused &&
                  br.program == v3);
        check("undo: the same overlay bytes, the program before, and the render before (cmp)",
              undoRecordedEdit(o, current, rec) && writeOverlay(o) == textBefore && current == v3 &&
                  same(play(applyOverlay(v3, o).program), r0));
    }

    // ---------------------------------------------- add-zone and add-layer ---
    group("2026-11-01 structural: add-zone / add-layer take ids from 1000000; a newer base never collides");
    {
        Overlay o;
        Program current = v3;
        const std::string textBefore = writeOverlay(o);
        Zone nz;
        nz.keyLo = nz.keyHi = nz.rootKey = 42;
        nz.release = 0.05;
        nz.extras.attributes.emplace_back("futureZoneAttr", "kept");
        Layer nl;
        nl.src = "audio/d.wav";
        nl.id = 4;  // ignored: the overlay hands out its own
        nz.layers.push_back(nl);
        const RecordedEdit addZ = recordAddZone(o, current, nz);
        note("recorded:\n" + (o.edits.size() >= 2 ? writeXml(o.edits[0]) + writeXml(o.edits[1]) : std::string("-")));
        check("add-zone zone=1000000 then add-layer layer=1000001 zone=1000000; the overlay's nextId 1000002",
              addZ.ok && addZ.id == 1000000 && addZ.count == 2 && o.edits[0].name == "add-zone" &&
                  *o.edits[0].attribute("zone") == "1000000" && o.edits[1].name == "add-layer" &&
                  *o.edits[1].attribute("layer") == "1000001" && *o.edits[1].attribute("zone") == "1000000" &&
                  o.nextId == 1000002);
        Layer extra;
        extra.src = "audio/e.wav";
        extra.velLo = 64;
        const RecordedEdit addL = recordAddLayer(o, current, 9, extra);
        check("add-layer into a BASE zone (9) takes 1000002", addL.ok && addL.id == 1000002 && o.nextId == 1000003 &&
                                                                  current.findLayer(1000002).first->id == 9);
        // The same made by hand.
        Program direct = v3;
        Zone dz = nz;
        dz.id = 1000000;
        dz.layers[0].id = 1000001;
        direct.zones.push_back(dz);
        Layer dl = extra;
        dl.id = 1000002;
        direct.findZone(9)->layers.push_back(dl);
        const OverlayResult r = applyOverlay(v3, o);
        note("applied: " + describe(r));
        check("applied on v3: equals the zone and layer added by hand, the unknown attribute kept (structural)",
              r.program == direct && current == direct && r.program.findZone(1000000)->extras.attributes.size() == 1);
        const Stereo a = playMore(r.program), b = playMore(direct), base = playMore(v3);
        note("RMS from key 42's note-on: v3 " + std::to_string(rms(base.l, 14400, 30000)) + ", with the added zone " +
             std::to_string(rms(a.l, 14400, 30000)));
        check("...renders byte-identical to it (cmp), and the added zone sounds", same(a, b) && !same(a, base));
        check("the base keeps its nextId (19): the overlay's ids are not the program's",
              r.program.nextId == v3.nextId && v3.nextId == 19);

        // A newer base adds zones of its own after the overlay was made.
        Program v7 = v3;
        const uint32_t n1 = addZone(v7, v3.zones[0]);
        const uint32_t n2 = addZone(v7, v3.zones[1]);
        const OverlayResult r7 = applyOverlay(v7, o);
        note("v7 adds zones " + std::to_string(n1) + ", " + std::to_string(n2) + "; overlay on v7: " + describe(r7));
        bool unique = true;
        std::set<uint32_t> seen;
        for (const Zone& z : r7.program.zones) {
            unique = unique && seen.insert(z.id).second;
            for (const Layer& l : z.layers) unique = unique && seen.insert(l.id).second;
        }
        check("v7's new zones take 19.. (below the range) and every overlay edit still applies, all ids distinct",
              n1 == 19 && n2 == 21 && r7.orphaned().empty() && unique && r7.edits.size() == 3 &&
                  r7.edits[0].state == OverlayEditState::Applied && r7.edits[1].state == OverlayEditState::Applied &&
                  r7.edits[2].state == OverlayEditState::Applied && r7.program.zones.size() == 6);
        Program v9 = v3;
        v9.nextId = kOverlayIdFirst - 1;
        const uint32_t lastBase = addZone(v9, Zone{});
        check("a base can grow to 999999 and no further: the next addZone is refused, so it never takes 1000000",
              lastBase == kOverlayIdFirst - 1 && addZone(v9, Zone{}) == 0 && applyOverlay(v9, o).orphaned().empty());

        // A base that does carry overlay ids (a flattened program used as a
        // base): an add naming one is refused, not merged.
        const Program flatBase = flatten(v3, o).program;
        const OverlayResult fr = applyOverlay(flatBase, o);
        note("the overlay on its own flattened program: " + describe(fr) +
             (fr.edits.empty() ? "" : " -- " + fr.edits[0].error));
        check("its adds collide and are refused (\"already in the base\"); the base is what plays",
              fr.edits.size() == 3 && fr.edits[0].state == OverlayEditState::Refused &&
                  fr.edits[0].error.find("already in the base") != std::string::npos &&
                  fr.edits[1].state == OverlayEditState::Refused && fr.edits[2].state == OverlayEditState::Refused &&
                  fr.program == flatBase);
        Overlay fresh;
        Program fc = flatBase;
        const RecordedEdit past = recordAddZone(fresh, fc, nz);
        check("a new overlay on that program hands out ids past the ones it carries (1000003)",
              past.ok && past.id == 1000003);

        // Stored adds that cannot apply.
        const OverlayParseResult bad = parseOverlay(
            R"(<NassauDigitalOverlay version="1">)"
            R"(<add-zone zone="17" keyLo="1"/>)"                                   // below the range (and the base's)
            R"(<add-zone zone="50" keyLo="1"/>)"                                   // below the range, a free id
            R"(<add-zone zone="1000010" attack="1" attack.was="0"/>)"              // a .was on an add
            R"(<add-zone zone="1000011" mode="tape"/>)"                            // malformed value
            R"(<add-layer layer="1000012" zone="99"/>)"                            // no zone 99: orphaned
            R"(<add-layer layer="1000013" zone="9"/><add-layer layer="1000014" zone="9"/>)"
            R"(<add-layer layer="1000015" zone="9"/><add-layer layer="1000016" zone="9"/>)"  // the 5th: full
            R"(<add-layer layer="10" zone="5"/>)"                                  // below the range
            R"(</NassauDigitalOverlay>)");
        const OverlayResult br = applyOverlay(v3, bad.overlay);
        note("stored adds: " + describe(br));
        std::vector<OverlayEditState> want = {OverlayEditState::Refused, OverlayEditState::Refused,
                                              OverlayEditState::Refused, OverlayEditState::Refused,
                                              OverlayEditState::Orphaned,
                                              OverlayEditState::Applied, OverlayEditState::Applied,
                                              OverlayEditState::Applied, OverlayEditState::Refused,
                                              OverlayEditState::Refused};
        bool asWanted = bad.ok && br.edits.size() == want.size();
        for (size_t i = 0; asWanted && i < want.size(); ++i) asWanted = br.edits[i].state == want[i];
        check("refused: ids below the range (the base's 17, a free 50), a .was, a bad value, a fifth layer, a layer id below the range; "
              "orphaned: add-layer to a missing zone (99)",
              asWanted && br.edits[4].missingId == 99 && br.program.findZone(9)->layers.size() == 4 &&
                  !br.program.findZone(50));

        // Undo, newest first: back to v3, the overlay's edits empty, its nextId kept.
        check("undo add-layer then add-zone: v3 again, render cmp, no edits left",
              undoRecordedEdit(o, current, addL) && undoRecordedEdit(o, current, addZ) && current == v3 &&
                  o.edits.empty() && same(playMore(applyOverlay(v3, o).program), base));
        check("...the overlay's nextId stays 1000003 (ids are never reused), so its bytes differ only by it",
              o.nextId == 1000003 && [&] {
                  Overlay x = o;
                  x.nextId = 0;
                  return writeOverlay(x) == textBefore;
              }());
        const RecordedEdit again = recordAddZone(o, current, nz);
        check("the next add takes 1000003, not an id handed out before", again.ok && again.id == 1000003);
    }

    // ----------------------------------------------------------- hide-zone ---
    group("2026-11-01 structural: hide-zone hides a base zone (the base keeps it); undoable; flatten drops it");
    {
        Overlay o;
        Program current = v3;
        const RecordedEdit hide = recordHideZone(o, current, 9);
        const std::string verb = hide.ok ? writeXml(o.edits.back()) : std::string();
        check("recorded as <hide-zone zone=\"9\"/>", hide.ok && verb == "<hide-zone zone=\"9\"/>\n");
        Program direct = v3;
        removeZone(direct, 9);
        const OverlayResult r = applyOverlay(v3, o);
        check("what plays has no zone 9 (nor its layer 10); the base still has both",
              r.program == direct && current == direct && !r.program.findZone(9) && !r.program.findLayer(10).second &&
                  v3.findZone(9) && v3.findLayer(10).second);
        const Stereo a = play(r.program), base = play(v3);
        note("RMS from key 38's note-on to key 40's: v3 " + std::to_string(rms(base.l, 4800, 9600)) + ", hidden " +
             std::to_string(rms(a.l, 4800, 9600)) + " (key 36 still sounding)");
        check("renders byte-identical to the zone removed by hand (cmp), and differs from v3",
              same(a, play(direct)) && !same(a, base));
        check("an edit of the hidden zone is refused while it is hidden",
              [&] {
                  Overlay x = o;
                  Program c = current;
                  return !recordZoneEdit(x, c, ZoneScope::zones({9}), {ParamEdit::absolute("tune", "0")}).ok;
              }());
        const OverlayResult flat = flatten(v3, o);
        const std::string text = writeProgram(flat.program);
        const ProgramParseResult reread = parseProgram(text);
        check("flatten drops it: the written program has no zone 9 and no layer 10, equals what plays, renders cmp",
              reread.ok && text.find("<zone id=\"9\"") == std::string::npos &&
                  text.find("<layer id=\"10\"") == std::string::npos && reread.program == r.program &&
                  same(play(reread.program), a));
        const OverlayResult on4 = applyOverlay(v4, [] {
            Overlay h;
            XmlNode v;
            v.name = "hide-zone";
            v.attributes.emplace_back("zone", "17");
            h.edits.push_back(v);
            return h;
        }());
        check("hiding a zone the base has dropped (17 on v4) is orphaned (id 17), nothing changes",
              on4.orphaned() == std::vector<size_t>({0}) && on4.edits[0].missingId == 17 && on4.program == v4);
        const OverlayParseResult bad = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><hide-zone zone="all"/><hide-zone zone="5 9"/>)"
            R"(<hide-zone zone="5" tune="0"/><hide-zone/></NassauDigitalOverlay>)");
        const OverlayResult br = applyOverlay(v3, bad.overlay);
        bool allRefused = bad.ok && br.edits.size() == 4;
        for (const auto& e : br.edits) allRefused = allRefused && e.state == OverlayEditState::Refused;
        check("refused: hide-zone of all, of a list, with another attribute, without a zone; nothing hidden",
              allRefused && br.program == v3);
        check("undo: zone 9 back where it was (second), v3 again, the overlay empty, render cmp",
              undoRecordedEdit(o, current, hide) && current == v3 && current.zones[1].id == 9 && o.edits.empty() &&
                  same(play(applyOverlay(v3, o).program), base));
    }

    // ---------------------------------------------------------- hide-layer ---
    group("2026-11-01 (the author, on #24) hide-layer: hides one layer of a base zone; undoable; flatten drops it");
    {
        Overlay o;
        Program current = v3;
        const RecordedEdit hide = recordHideLayer(o, current, 6);
        const std::string verb = hide.ok ? writeXml(o.edits.back()) : std::string();
        check("recorded as <hide-layer layer=\"6\"/>", hide.ok && verb == "<hide-layer layer=\"6\"/>\n");
        Program direct = v3;
        removeLayer(direct, 6);
        const OverlayResult r = applyOverlay(v3, o);
        check("what plays has zone 5 without layer 6; the base still has the layer",
              r.program == direct && current == direct && r.program.findZone(5) && !r.program.findLayer(6).second &&
                  v3.findLayer(6).second);
        const Stereo a = play(r.program), base = play(v3);
        note("RMS from key 36's note-on to key 38's: v3 " + std::to_string(rms(base.l, 0, 4800)) + ", hidden " +
             std::to_string(rms(a.l, 0, 4800)));
        check("renders byte-identical to the layer removed by hand (cmp), and differs from v3",
              same(a, play(direct)) && !same(a, base));
        check("an edit of the hidden layer is refused while it is hidden", [&] {
            Overlay x = o;
            Program c = current;
            return !recordLayerEdit(x, c, {6}, {ParamEdit::absolute("tune", "0")}).ok;
        }());
        const OverlayResult flat = flatten(v3, o);
        const std::string text = writeProgram(flat.program);
        const ProgramParseResult reread = parseProgram(text);
        check("flatten drops it: the written program has zone 5 but no layer 6, equals what plays, renders cmp",
              reread.ok && text.find("<zone id=\"5\"") != std::string::npos &&
                  text.find("<layer id=\"6\"") == std::string::npos && reread.program == r.program &&
                  same(play(reread.program), a));
        Program noLayer = v3;
        removeLayer(noLayer, 6);
        const OverlayResult onNoLayer = applyOverlay(noLayer, o);
        Overlay h18;
        Program c18 = v3;
        recordHideLayer(h18, c18, 18);
        const OverlayResult onV4 = applyOverlay(v4, h18);
        check("orphaned (naming the layer) on a base without the layer (6) and on one without its zone (18 on v4)",
              onNoLayer.orphaned() == std::vector<size_t>({0}) && onNoLayer.edits[0].missingId == 6 &&
                  onNoLayer.program == noLayer && onV4.orphaned() == std::vector<size_t>({0}) &&
                  onV4.edits[0].missingId == 18 && onV4.program == v4);
        const OverlayParseResult bad = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><hide-layer layer="all"/><hide-layer layer="6 10"/>)"
            R"(<hide-layer layer="6" zone="5"/><hide-layer/></NassauDigitalOverlay>)");
        const OverlayResult br = applyOverlay(v3, bad.overlay);
        bool allRefused = bad.ok && br.edits.size() == 4;
        for (const auto& e : br.edits) allRefused = allRefused && e.state == OverlayEditState::Refused;
        check("refused: hide-layer of all, of a list, with another attribute, without a layer; nothing hidden",
              allRefused && br.program == v3);
        // A zone with two layers: the hidden one comes back in its place.
        Program two = v3;
        Layer second;
        second.src = "audio/f.wav";
        const uint32_t l2 = addLayer(two, 5, second);
        Overlay o2;
        Program c2 = two;
        const RecordedEdit h2 = recordHideLayer(o2, c2, 6);
        check("on a zone with two layers, hiding the first leaves the second",
              h2.ok && c2.findZone(5)->layers.size() == 1 && c2.findZone(5)->layers[0].id == l2);
        check("undo: the layer back where it was (first), the program again, the overlay empty",
              undoRecordedEdit(o2, c2, h2) && c2 == two && c2.findZone(5)->layers[0].id == 6 && o2.edits.empty());
        check("undo on v3: v3 again, the overlay empty, render cmp",
              undoRecordedEdit(o, current, hide) && current == v3 && o.edits.empty() &&
                  same(play(applyOverlay(v3, o).program), base));
    }

    // -------------------------------------------------------------- rebase ---
    group("2026-11-01 (the author, on #24) rebase: accept a new base; baseVersion and the drifted .was refreshed");
    {
        Program lib3 = v3;
        lib3.libraryVersion = "3";
        // Version 4: zone 17's attack changed (0.05), the main level changed
        // (-3), zone 9 dropped.
        Program lib4 = v5;
        lib4.libraryVersion = "4";
        lib4.outputLevel = -3.0;
        removeZone(lib4, 9);
        Overlay o;
        Program current = lib3;
        const bool made =
            recordZoneEdit(o, current, ZoneScope::zones({17}), {ParamEdit::absolute("attack", "0.25")}).ok &&  // #0
            recordLayerEdit(o, current, {6}, {ParamEdit::absolute("tune", "3")}).ok &&                        // #1
            recordProgramEdit(o, current, {ParamEdit::absolute("outputLevel", "-6")}).ok &&                   // #2
            recordZoneEdit(o, current, ZoneScope::zones({9}), {ParamEdit::absolute("tune", "1")}).ok &&       // #3
            recordZoneEdit(o, current, ZoneScope::zones({17}), {ParamEdit::absolute("attack", "0.5")}).ok;    // #4
        XmlNode future;
        future.name = "set-swing";
        future.attributes.emplace_back("zone", "17");
        o.edits.push_back(future);  // #5, a newer writer's
        // #6, written by another hand over #4 with a stale .was: it drifts on
        // top of #4's 0.5, so its refreshed .was must be 0.5 (the value under
        // it, earlier edits applied), not the bare base's 0.05. #7 applies
        // without drift but spells its .was "0.0": left as written.
        const OverlayParseResult extra = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><set-zone zone="17" attack="0.75" attack.was="0.3"/>)"
            R"(<set-layer layer="18" tune="1" tune.was="0.0"/></NassauDigitalOverlay>)");
        for (const XmlNode& v : extra.overlay.edits) o.edits.push_back(v);
        const OverlayResult pre = applyOverlay(lib4, o);
        note("on version 4 before the rebase: " + describe(pre) + "; version " +
             (pre.versionMismatch() ? pre.baseDrift[0].was + " -> " + pre.baseDrift[0].now : std::string("same")));
        check("before: version 3 vs 4 reported, #0, #2 and #6 drifted, #3 orphaned (zone 9), #4 applied (0.25 under it)",
              made && extra.ok && pre.versionMismatch() && pre.drifted() == std::vector<size_t>({0, 2, 6}) &&
                  pre.orphaned() == std::vector<size_t>({3}) && pre.edits[4].state == OverlayEditState::Applied);
        const Overlay before = o;
        const Program currentBefore = current;
        const RebasedOverlay rb = rebaseOverlay(o, current, lib4);
        check("rebase: baseVersion 3 -> 4, #0, #2 and #6 refreshed, #3 still orphaned",
              rb.ok && o.baseVersion == "4" && rb.fromVersion == "3" && rb.toVersion == "4" &&
                  rb.refreshed == std::vector<size_t>({0, 2, 6}) && rb.orphaned == std::vector<size_t>({3}));
        check("the refreshed .was are the new base's values: attack.was=\"0.05\", outputLevel.was=\"-3\"",
              writeXml(o.edits[0]) == "<set-zone zone=\"17\" attack=\"0.25\" attack.was=\"0.05\"/>\n" &&
                  writeXml(o.edits[2]) == "<set-program outputLevel=\"-6\" outputLevel.was=\"-3\"/>\n");
        check("a chained edit's .was is the value under it, earlier edits applied: #6 attack.was=\"0.5\" (not 0.05)",
              writeXml(o.edits[6]) == "<set-zone zone=\"17\" attack=\"0.75\" attack.was=\"0.5\"/>\n");
        bool untouched = true;
        for (size_t i : {1, 3, 4, 5, 7}) untouched = untouched && o.edits[i] == before.edits[i];
        check("every other edit is byte for byte as it was (the applied ones, \"0.0\" included, the orphan, the unknown)",
              untouched && o.edits.size() == before.edits.size());
        const OverlayResult post = applyOverlay(lib4, o);
        note("on version 4 after the rebase: " + describe(post));
        check("after: no version mismatch, no drift; the orphan is still orphaned",
              !post.versionMismatch() && post.drifted().empty() && post.orphaned() == std::vector<size_t>({3}));
        check("what plays does not change: equal to version 4 + the overlay before the rebase (structural and cmp)",
              post.program == pre.program && current == pre.program && same(play(post.program), play(pre.program)));
        const std::string text = writeOverlay(o);
        const OverlayParseResult back = parseOverlay(text);
        check("the rebased overlay round-trips byte-stable", back.ok && back.overlay == o && writeOverlay(back.overlay) == text);
        Overlay again = o;
        Program cAgain = current;
        const RebasedOverlay rb2 = rebaseOverlay(again, cAgain, lib4);
        check("rebasing again onto the same base changes nothing (idempotent)",
              rb2.ok && rb2.refreshed.empty() && again == o && cAgain == current);
        Overlay later = o;
        Program cLater = current;
        recordZoneEdit(later, cLater, ZoneScope::zones({5}), {ParamEdit::absolute("tune", "0")});
        check("an undo of the rebase after a further edit is refused, nothing changed",
              !undoRebase(later, cLater, rb) && later.edits.size() == o.edits.size() + 1);
        check("undo of the rebase: the overlay's bytes and what played before (version 3) again",
              undoRebase(o, current, rb) && writeOverlay(o) == writeOverlay(before) && current == currentBefore &&
                  applyOverlay(lib4, o).drifted() == std::vector<size_t>({0, 2, 6}));
    }

    // ------------------------------------------------------------- set-pad ---
    group("2026-11-04 (the author, QBX-278) set-pad: a pad reassignment with key.was, set-program's semantics");
    {
        // The library's table: pad 2 has a row ON its default key (38), pad 5
        // plays the kick (36). Version 4 moved pad 5 to 42, gave pad 1 a row
        // (47) and dropped pad 2's row.
        Program lib3 = v3;
        lib3.libraryVersion = "3";
        lib3.pads = {PadRow{2, 38, {}}, PadRow{5, 36, {}}};
        Program lib4 = lib3;
        lib4.libraryVersion = "4";
        lib4.pads = {PadRow{1, 47, {}}, PadRow{5, 42, {}}};
        Overlay o;
        Program current = lib3;
        const std::string textBefore = writeOverlay(o);
        const RecordedEdit e0 = recordPadEdit(o, current, 5, 40);   // #0: a base row
        const RecordedEdit e1 = recordPadEdit(o, current, 1, 50);   // #1: no row, the default 37
        const RecordedEdit e2 = recordPadEdit(o, current, 93, 60);  // #2: no row, no default
        const bool made = e0.ok && e1.ok && e2.ok && o.edits.size() == 3;
        const std::string v0 = made ? writeXml(o.edits[0]) : "", v1 = made ? writeXml(o.edits[1]) : "",
                          v2 = made ? writeXml(o.edits[2]) : "";
        note("recorded: " + v0.substr(0, v0.size() ? v0.size() - 1 : 0) + " " +
             v1.substr(0, v1.size() ? v1.size() - 1 : 0) + " " + v2.substr(0, v2.size() ? v2.size() - 1 : 0));
        check("key.was is the row's key, else the default 36 + n, else \"none\" (pads 92..95)",
              v0 == "<set-pad n=\"5\" key=\"40\" key.was=\"36\"/>\n" &&
                  v1 == "<set-pad n=\"1\" key=\"50\" key.was=\"37\"/>\n" &&
                  v2 == "<set-pad n=\"93\" key=\"60\" key.was=\"none\"/>\n");
        check("the first edit records the base's version", o.baseVersion == "3" && e0.stampedBaseVersion);
        Program direct = lib3;
        setPadKey(direct, 5, 40);
        setPadKey(direct, 1, 50);
        setPadKey(direct, 93, 60);
        const OverlayResult r = applyOverlay(lib3, o);
        note("on version 3: " + describe(r));
        check("applied on version 3: equals the reassignments made directly, nothing drifted; the base untouched",
              r.program == direct && current == direct && r.drifted().empty() && r.orphaned().empty() &&
                  r.edits[0].state == OverlayEditState::Applied && r.edits[2].state == OverlayEditState::Applied &&
                  lib3.padKey(5) == 36);
        const std::string text = writeOverlay(o);
        const OverlayParseResult back = parseOverlay(text);
        check("the overlay round-trips byte-stable", back.ok && back.overlay == o && writeOverlay(back.overlay) == text);

        const OverlayResult d = applyOverlay(lib4, o);
        note("on version 4: " + describe(d));
        check("on version 4 (pad 5 moved to 42, pad 1 given 47): #0 and #1 drifted with was / now; #2 applied",
              d.drifted() == std::vector<size_t>({0, 1}) && d.edits[0].drift.size() == 1 &&
                  d.edits[0].drift[0] == OverlayDrift{"key", "36", "42"} && d.edits[1].drift.size() == 1 &&
                  d.edits[1].drift[0] == OverlayDrift{"key", "37", "47"} &&
                  d.edits[2].state == OverlayEditState::Applied && d.versionMismatch());
        check("...still applied: the overlay's keys play (pad 5 on 40, pad 1 on 50, pad 93 on 60)",
              d.program.padKey(5) == 40 && d.program.padKey(1) == 50 && d.program.padKey(93) == 60);
        Program lib6 = lib3;
        lib6.pads.push_back(PadRow{93, 61, {}});  // a newer base gives pad 93 a key of its own
        const OverlayResult d6 = applyOverlay(lib6, o);
        check("a base that adds a row for a pad the overlay set from \"none\": drifted (was none, now 61); 60 plays",
              d6.drifted() == std::vector<size_t>({2}) && d6.edits[2].drift.size() == 1 &&
                  d6.edits[2].drift[0] == OverlayDrift{"key", "none", "61"} && d6.program.padKey(93) == 60);
        Program bare = v3;  // no table at all: a set-pad is never orphaned
        const OverlayResult rb0 = applyOverlay(bare, o);
        check("a set-pad is never orphaned (a base with no <Pads> table at all)",
              rb0.orphaned().empty() && rb0.program.padKey(5) == 40);

        Overlay ro = o;
        Program rc = current;
        const RebasedOverlay rb = rebaseOverlay(ro, rc, lib4);
        check("rebase onto version 4: #0 and #1 refreshed to key.was=\"42\" / \"47\"; #2 left byte for byte",
              rb.ok && rb.refreshed == std::vector<size_t>({0, 1}) && ro.baseVersion == "4" &&
                  writeXml(ro.edits[0]) == "<set-pad n=\"5\" key=\"40\" key.was=\"42\"/>\n" &&
                  writeXml(ro.edits[1]) == "<set-pad n=\"1\" key=\"50\" key.was=\"47\"/>\n" && ro.edits[2] == o.edits[2]);
        const OverlayResult post = applyOverlay(lib4, ro);
        check("after the rebase: no drift, no version report; what plays does not change",
              post.drifted().empty() && !post.versionMismatch() && post.program == d.program && rc == d.program);
        check("undo of the rebase: the overlay as before", undoRebase(ro, rc, rb) && ro == o && rc == current);

        const OverlayResult f = flatten(lib4, o);
        const std::string flat = writeProgram(f.program);
        check("flatten writes the resulting <Pads> table: 1 -> 50, 5 -> 40, 93 -> 60, and no libraryVersion",
              flat.find("<Pad n=\"1\" key=\"50\"/>") != std::string::npos &&
                  flat.find("<Pad n=\"5\" key=\"40\"/>") != std::string::npos &&
                  flat.find("<Pad n=\"93\" key=\"60\"/>") != std::string::npos && f.program.libraryVersion.empty() &&
                  parseProgram(flat).ok && parseProgram(flat).program == f.program);
        Overlay od = o;
        Program cd = current;
        const RecordedEdit toDefault = recordPadEdit(od, cd, 5, 41);  // 36 + 5: pad 5's default
        check("a set-pad to the pad's default key leaves no row in the flattened table",
              toDefault.ok && flatten(lib3, od).program.pads.size() == 3 &&
                  writeProgram(flatten(lib3, od).program).find("<Pad n=\"5\"") == std::string::npos);

        Overlay ou = o;
        Program cu = current;
        const std::string uBefore = writeOverlay(ou);
        const RecordedEdit e3 = recordPadEdit(ou, cu, 2, 44);  // pad 2's row is ON its default
        check("undo of a set-pad over a row on its default key: the overlay bytes and the table, row and order, as before",
              e3.ok && undoRecordedEdit(ou, cu, e3) && writeOverlay(ou) == uBefore && cu == current &&
                  cu.pads.size() == current.pads.size());
        check("undo of all three, last first: the empty overlay, version 3 as it was, baseVersion cleared",
              undoRecordedEdit(o, current, e2) && undoRecordedEdit(o, current, e1) &&
                  undoRecordedEdit(o, current, e0) && writeOverlay(o) == textBefore && current == lib3 &&
                  o.baseVersion.empty());
        check("undo of an edit that is not the last is refused", !undoRecordedEdit(o, current, e1));

        Overlay ob;
        Program cb = lib3;
        check("recording n = 96, n = -1, key = 128 or key = -1 is refused, nothing changes",
              !recordPadEdit(ob, cb, 96, 40).ok && !recordPadEdit(ob, cb, -1, 40).ok &&
                  !recordPadEdit(ob, cb, 3, 128).ok && !recordPadEdit(ob, cb, 93, -1).ok && ob.edits.empty() &&
                  cb == lib3);
        const OverlayParseResult bad = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><set-pad n="96" key="40" key.was="none"/>)"
            R"(<set-pad n="3" key="128" key.was="39"/><set-pad n="3" key="40"/>)"
            R"(<set-pad n="3" key="40" key.was="x"/><set-pad n="3" key="40" key.was="39" zone="5"/>)"
            R"(<set-pad key="40" key.was="39"/><set-pad n="3" key="+2" key.was="39"/></NassauDigitalOverlay>)");
        const OverlayResult br = applyOverlay(lib3, bad.overlay);
        bool allRefused = bad.ok && br.edits.size() == 7;
        for (const auto& e : br.edits) allRefused = allRefused && e.state == OverlayEditState::Refused;
        note("refusals: " + (br.edits.empty() ? std::string() : br.edits[0].error + " | " + br.edits[4].error));
        check("stored set-pads out of range, without key.was, with a bad key.was, an extra attribute, no n or a "
              "relative key are refused; the program is the base",
              allRefused && br.program == lib3);
    }

    // ------------------------------------------------------------ hide-pad ---
    group("2026-11-04 (the author, QBX-278) hide-pad: hides one base <Pads> row; undoable; flatten drops it");
    {
        // The library: pad 2 on 50, pad 5 on 36, pad 93 on 70 (no default:
        // it exists only by its row). Version 2 dropped pad 93's row.
        Program lib1 = v3;
        lib1.libraryVersion = "1";
        lib1.pads = {PadRow{2, 50, {}}, PadRow{5, 36, {}}, PadRow{93, 70, {}}};
        Program lib2 = lib1;
        lib2.libraryVersion = "2";
        lib2.pads = {PadRow{2, 50, {}}, PadRow{5, 36, {}}};
        Overlay o;
        Program current = lib1;
        const std::string textBefore = writeOverlay(o);
        const RecordedEdit h5 = recordHidePad(o, current, lib1, 5);    // #0
        const RecordedEdit h93 = recordHidePad(o, current, lib1, 93);  // #1
        const bool made = h5.ok && h93.ok && o.edits.size() == 2;
        check("recorded as <hide-pad n=\"5\"/> and <hide-pad n=\"93\"/>",
              made && writeXml(o.edits[0]) == "<hide-pad n=\"5\"/>\n" && writeXml(o.edits[1]) == "<hide-pad n=\"93\"/>\n");
        const OverlayResult r = applyOverlay(lib1, o);
        note("on version 1: " + describe(r));
        check("what plays: pad 5 back on its default 41, pad 93 has no key; pad 2 keeps its row; the base keeps all three",
              r.program.padKey(5) == 41 && r.program.padKey(93) == -1 && r.program.padKey(2) == 50 &&
                  r.program.pads.size() == 1 && current == r.program && lib1.pads.size() == 3 &&
                  r.drifted().empty() && r.orphaned().empty());
        const std::string text = writeOverlay(o);
        const OverlayParseResult back = parseOverlay(text);
        check("the overlay round-trips byte-stable", back.ok && back.overlay == o && writeOverlay(back.overlay) == text);
        const OverlayResult f = flatten(lib1, o);
        check("flatten drops the hidden rows: only pad 2's row is written",
              f.program.pads.size() == 1 && f.program.pads[0].n == 2 &&
                  writeProgram(f.program).find("<Pad n=\"5\"") == std::string::npos &&
                  writeProgram(f.program).find("<Pad n=\"93\"") == std::string::npos);
        const OverlayResult r2 = applyOverlay(lib2, o);
        note("on version 2: " + describe(r2));
        check("on version 2 (no row for pad 93): #1 orphaned, missingId 93; #0 applies; nothing else changes",
              r2.orphaned() == std::vector<size_t>({1}) && r2.edits[1].missingId == 93 &&
                  r2.edits[0].state == OverlayEditState::Applied && r2.program.padKey(5) == 41 &&
                  r2.program.padKey(2) == 50);
        Overlay ro = o;
        Program rc = applyOverlay(lib2, o).program;
        const RebasedOverlay rb = rebaseOverlay(ro, rc, lib2);
        check("rebase onto version 2: the orphan stays an orphan, byte for byte", rb.ok &&
              rb.orphaned == std::vector<size_t>({1}) && ro.edits == o.edits && applyOverlay(lib2, ro).orphaned() ==
                                                                                     std::vector<size_t>({1}));
        // set-pad after the hide: key.was is what plays then, the default.
        Overlay os = o;
        Program cs = current;
        const RecordedEdit sp = recordPadEdit(os, cs, 5, 44);
        check("a set-pad after the hide records key.was=\"41\" (the default the hide left)",
              sp.ok && writeXml(os.edits.back()) == "<set-pad n=\"5\" key=\"44\" key.was=\"41\"/>\n" && cs.padKey(5) == 44);
        Overlay ob = o;
        Program cb = current;
        check("refused, changing nothing: a pad with no base row, a row already hidden, n = 96",
              !recordHidePad(ob, cb, lib1, 1).ok && !recordHidePad(ob, cb, lib1, 5).ok &&
                  !recordHidePad(ob, cb, lib1, 96).ok && ob == o && cb == current);
        note("refusals: " + recordHidePad(ob, cb, lib1, 1).error + " | " + recordHidePad(ob, cb, lib1, 5).error);
        Overlay od = Overlay{};
        Program cdef = lib1;
        recordPadEdit(od, cdef, 2, 38);  // set-pad to the default removes the row from what plays
        const RecordedEdit hd = recordHidePad(od, cdef, lib1, 2);
        check("refused, saying why: a base row the overlay already put on its default key, or already hidden",
              !hd.ok && od.edits.size() == 1 && hd.error.find("does not play now") != std::string::npos &&
                  recordHidePad(ob, cb, lib1, 5).error.find("already hidden") != std::string::npos);
        Overlay on = Overlay{};
        Program cn = lib1;
        recordPadEdit(on, cn, 1, 60);  // a row the OVERLAY made, the base has none
        const RecordedEdit hn = recordHidePad(on, cn, lib1, 1);
        check("refused: a row only the overlay made (the base has none for the pad); a hide is of a base row",
              !hn.ok && on.edits.size() == 1 && cn.padKey(1) == 60 &&
                  hn.error.find("no row in the base") != std::string::npos);
        check("undo of both, last first: the rows back where they were (order kept), the empty overlay",
              undoRecordedEdit(o, current, h93) && current.pads.size() == 2 && undoRecordedEdit(o, current, h5) &&
                  current == lib1 && current.pads[1].n == 5 && writeOverlay(o) == textBefore && o.baseVersion.empty());
        const OverlayParseResult bad = parseOverlay(
            R"(<NassauDigitalOverlay version="1"><hide-pad n="96"/><hide-pad/><hide-pad n="5" key="1"/>)"
            R"(<hide-pad n="+5"/><hide-pad n="x"/></NassauDigitalOverlay>)");
        const OverlayResult br = applyOverlay(lib1, bad.overlay);
        bool allRefused = bad.ok && br.edits.size() == 5;
        for (const auto& e : br.edits) allRefused = allRefused && e.state == OverlayEditState::Refused;
        check("stored hide-pads out of range, without n, with another attribute, relative or not a number are "
              "refused; the program is the base",
              allRefused && br.program == lib1);
    }

    // --------------------------------------------- the format, all verbs ---
    group("2026-11-01 the overlay format with every verb: byte-stable; orphans of every kind; flatten proceeds");
    {
        Overlay o;
        Program current = v3;
        current.libraryVersion = "3";
        Zone nz;
        nz.keyLo = nz.keyHi = nz.rootKey = 42;
        nz.layers.resize(1);
        nz.layers[0].src = "audio/d.wav";
        const bool made =
            recordZoneEdit(o, current, ZoneScope::zones({17}), {ParamEdit::absolute("attack", "0.25")}).ok &&
            recordLayerEdit(o, current, {18}, {ParamEdit::absolute("tune", "1")}).ok &&
            recordProgramEdit(o, current, {ParamEdit::absolute("outputLevel", "-1")}).ok &&
            recordAddZone(o, current, nz).ok && recordAddLayer(o, current, 17, Layer{}).ok &&
            recordHideZone(o, current, 5).ok && recordHideLayer(o, current, 10).ok;
        o.attributes.emplace_back("colour", "teal");
        XmlNode future;
        future.name = "set-swing";
        future.attributes.emplace_back("zone", "9");
        o.edits.push_back(future);
        const std::string text = writeOverlay(o);
        const OverlayParseResult back = parseOverlay(text);
        check("every kind recorded; parse -> write -> parse is the identity and the bytes are stable",
              made && back.ok && back.overlay == o && writeOverlay(back.overlay) == text &&
                  text.find(" nextId=\"1000003\"") != std::string::npos);
        check("the overlay's nextId: below 1000000 or malformed is refused on read",
              !parseOverlay(R"(<NassauDigitalOverlay version="1" nextId="5"/>)").ok &&
                  !parseOverlay(R"(<NassauDigitalOverlay version="1" nextId="x"/>)").ok);
        const OverlayResult r4 = applyOverlay(v4, o);
        note("on v4 (no zone 17): " + describe(r4));
        // #0 set-zone 17, #1 set-layer 18, #2 set-program, #3 add-zone, #4 add-layer (into the added zone),
        // #5 add-layer into 17, #6 hide-zone 5, #7 hide-layer 10, #8 set-swing.
        check("on v4: exactly the edits naming zone 17 or layer 18 are orphaned (#0, #1, #5), the rest apply",
              r4.orphaned() == std::vector<size_t>({0, 1, 5}) && r4.edits[5].missingId == 17 &&
                  r4.edits[2].state == OverlayEditState::Applied && r4.edits[3].state == OverlayEditState::Applied &&
                  r4.edits[4].state == OverlayEditState::Applied && r4.edits[6].state == OverlayEditState::Applied &&
                  r4.edits[7].state == OverlayEditState::Applied && r4.edits[8].state == OverlayEditState::Unknown);
        check("...and v4 (no libraryVersion) against baseVersion 3 is reported too", r4.versionMismatch());
        const OverlayResult f4 = flatten(v4, o);
        const ProgramParseResult rf = parseProgram(writeProgram(f4.program));
        check("flatten on v4 proceeds: the orphans are reported and dropped; zones 9 (layer 10 hidden) and 1000000, "
              "outputLevel -1",
              f4.orphaned() == r4.orphaned() && rf.ok && rf.program.zones.size() == 2 && rf.program.findZone(9) &&
                  rf.program.findZone(9)->layers.empty() &&
                  rf.program.findZone(1000000) && rf.program.outputLevel == -1.0 && rf.program == f4.program);
    }

    return summary("overlay_tests");
}
