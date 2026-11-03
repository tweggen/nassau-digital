// pads_tests.cpp - the program-level <Pads> table (DESIGN §7; §13, settled
// 2026-11-03, pads; qbx QBX-277).
//
//   P1 parse, write and round-trip the table; a program without one writes
//      none (every program written before stays byte-identical).
//   P2 validation: n 0..127, key 0..127, no duplicate n, a single table; each
//      refused with a reason that names it.
//   P3 a pad's key: its row, else 36 + n, else none (n >= 92).
//   P4 setPadKey: only the key changes, a default key removes the row, rows
//      added in order of n, refusals change nothing, prior key undoes.
//   P5 a shared key: padsOnKey, and the selection rule (the selected pad stays
//      when it is one of them, else the lowest-numbered one).
//   P6 the zones covering a key: a pad's zone is the zone covering its key, so
//      several pads share one zone (the tom zone).
//   P7 the zone attribute `pad` is still read and written, and means nothing.

#include "nd_edit.h"
#include "nd_overlay.h"
#include "nd_program.h"
#include "nd_program_xml.h"
#include "test_util.h"

#include <string>
#include <vector>

using namespace nd;
using ndtest::check;
using ndtest::group;
using ndtest::note;

namespace {

const char* const kTomKit =
    "<NassauDigitalProgram version=\"1\" name=\"Toms\">\n"
    "  <zone id=\"1\" keyLo=\"36\" keyHi=\"36\" rootKey=\"36\"><layer id=\"2\" src=\"kick\"/></zone>\n"
    "  <zone id=\"3\" keyLo=\"45\" keyHi=\"50\" rootKey=\"48\"><layer id=\"4\" src=\"tom\"/></zone>\n"
    "  <Pads>\n"
    "    <Pad n=\"4\" key=\"50\"/>\n"
    "    <Pad n=\"5\" key=\"48\"/>\n"
    "    <Pad n=\"6\" key=\"45\"/>\n"
    "  </Pads>\n"
    "</NassauDigitalProgram>\n";

bool refused(const std::string& xml, const std::string& mustSay, std::string* said = nullptr) {
    const ProgramParseResult r = parseProgram(xml);
    if (said) *said = r.error;
    return !r.ok && r.error.find(mustSay) != std::string::npos;
}

std::string withPads(const std::string& rows) {
    return "<NassauDigitalProgram version=\"1\"><Pads>" + rows + "</Pads></NassauDigitalProgram>";
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: the <Pads> table (DESIGN §13, 2026-11-03) ===\n");

    group("P1 parse, write, round-trip");
    {
        const ProgramParseResult r = parseProgram(kTomKit);
        check("the tom kit parses", r.ok);
        if (!r.ok) note(r.error);
        const Program& p = r.program;
        check("three rows, as written", p.pads.size() == 3 && p.pads[0].n == 4 && p.pads[0].key == 50 &&
                                            p.pads[1].n == 5 && p.pads[1].key == 48 && p.pads[2].n == 6 &&
                                            p.pads[2].key == 45);
        const std::string written = writeProgram(p);
        const size_t at = written.find("<Pads");
        note("written: " + (at == std::string::npos ? std::string("(no <Pads>)") : written.substr(at)));
        check("the table is written after the zones",
              written.find("<Pads>") != std::string::npos && written.find("<Pads>") > written.rfind("</zone>"));
        check("each row is written as <Pad n=\"…\" key=\"…\"/>",
              written.find("<Pad n=\"4\" key=\"50\"/>") != std::string::npos &&
                  written.find("<Pad n=\"6\" key=\"45\"/>") != std::string::npos);
        const ProgramParseResult again = parseProgram(written);
        check("parse(write(p)) == p", again.ok && again.program == p);
        check("write is a fixed point", again.ok && writeProgram(again.program) == written);

        Program none;
        check("a program with no rows writes no <Pads>", writeProgram(none).find("Pads") == std::string::npos);
        const ProgramParseResult empty = parseProgram(withPads(""));
        check("an empty <Pads/> reads as no rows and is not written back",
              empty.ok && empty.program.pads.empty() && writeProgram(empty.program).find("Pads") == std::string::npos);

        // Rows out of order and unknown content are kept where they were.
        const ProgramParseResult odd = parseProgram(withPads(
            "<Pad n=\"9\" key=\"1\" color=\"red\"/><Pad n=\"2\" key=\"3\"><x/></Pad><y z=\"1\"/>"));
        check("rows keep file order, unknown attributes and children survive a round trip",
              odd.ok && odd.program.pads.size() == 2 && odd.program.pads[0].n == 9 &&
                  odd.program.pads[0].extras.attributes.size() == 1 && odd.program.pads[1].extras.children.size() == 1 &&
                  odd.program.padsExtras.children.size() == 1 &&
                  parseProgram(writeProgram(odd.program)).program == odd.program);
        Program changed = p;
        changed.pads[2].key = 46;
        check("equality sees a changed row", !(changed == p));
    }

    group("P2 validation, refused with a reason");
    {
        std::string why;
        check("n = 128 is refused", refused(withPads("<Pad n=\"128\" key=\"40\"/>"), "n: 128 is out of range", &why));
        note(why);
        check("n = -1 is refused", refused(withPads("<Pad n=\"-1\" key=\"40\"/>"), "n: -1 is out of range"));
        check("key = 128 is refused", refused(withPads("<Pad n=\"3\" key=\"128\"/>"), "key: 128 is out of range", &why));
        note(why);
        check("key = -1 is refused", refused(withPads("<Pad n=\"3\" key=\"-1\"/>"), "key: -1 is out of range"));
        check("a duplicate n is refused",
              refused(withPads("<Pad n=\"3\" key=\"40\"/><Pad n=\"3\" key=\"41\"/>"), "pad 3 has more than one row",
                      &why));
        note(why);
        check("a row without n is refused", refused(withPads("<Pad key=\"40\"/>"), "<Pad> needs n="));
        check("a row without key is refused", refused(withPads("<Pad n=\"1\"/>"), "<Pad> needs key="));
        check("a non-integer is refused", refused(withPads("<Pad n=\"1.5\" key=\"40\"/>"), "n: expected an integer"));
        check("a second <Pads> is refused",
              refused("<NassauDigitalProgram version=\"1\"><Pads/><Pads/></NassauDigitalProgram>",
                      "more than one <Pads>"));
        check("n 0 and 127, key 0 and 127 are accepted",
              parseProgram(withPads("<Pad n=\"0\" key=\"127\"/><Pad n=\"127\" key=\"0\"/>")).ok);
        check("two pads may share a key", parseProgram(withPads("<Pad n=\"0\" key=\"50\"/><Pad n=\"1\" key=\"50\"/>")).ok);
    }

    group("P3 a pad's key");
    {
        const Program p = parseProgram(kTomKit).program;
        check("a pad with a row plays its row's key (pad 4 -> 50)", p.padKey(4) == 50);
        check("a pad without a row plays 36 + n (pad 0 -> 36, pad 91 -> 127)", p.padKey(0) == 36 && p.padKey(91) == 127);
        check("a pad whose default passes 127 has no key (pad 92 -> -1)", p.padKey(92) == -1 && defaultPadKey(92) == -1);
        check("n out of range has no key", p.padKey(-1) == -1 && p.padKey(128) == -1);
        const Program q = parseProgram(withPads("<Pad n=\"100\" key=\"60\"/>")).program;
        check("a row gives a pad past the defaults a key (pad 100 -> 60)", q.padKey(100) == 60);
    }

    group("P4 setPadKey");
    {
        Program p = parseProgram(kTomKit).program;
        const Program before = p;
        PadEditResult r = setPadKey(p, 4, 51);
        check("reassigning pad 4 changes its row's key only", r.ok && r.priorKey == 50 && p.padKey(4) == 51 &&
                                                                  p.pads.size() == 3 && p.zones == before.zones);
        r = setPadKey(p, 4, r.priorKey);
        check("the prior key undoes it exactly", r.ok && p == before);

        r = setPadKey(p, 1, 60);
        check("a new row goes in order of n (before n 4)", r.ok && r.priorKey == 37 && p.pads.size() == 4 &&
                                                              p.pads[0].n == 1 && p.pads[0].key == 60);
        r = setPadKey(p, 9, 61);
        check("...and after the last smaller n", r.ok && p.pads.size() == 5 && p.pads[4].n == 9);
        r = setPadKey(p, 1, 37);
        check("its default key removes the row", r.ok && r.priorKey == 60 && p.pads.size() == 4 && p.padKey(1) == 37);
        r = setPadKey(p, 9, 45);
        check("pad 9 back on its default 45 removes its row", r.ok && r.priorKey == 61 && p.pads.size() == 3 &&
                                                                  p.padKey(9) == 45);
        r = setPadKey(p, 9, 45);
        check("setting the default of a pad with no row writes none", r.ok && r.priorKey == 45 && p.pads.size() == 3);

        const Program keep = p;
        check("n = 128 is refused, nothing changes", !setPadKey(p, 128, 40).ok && p == keep);
        check("key = 128 is refused, nothing changes", !setPadKey(p, 3, 128).ok && p == keep);
        const PadEditResult bad = setPadKey(p, 3, -1);
        check("key = -1 is refused for a pad with a default", !bad.ok && p == keep);
        note("refusal: " + bad.error);

        Program q;
        r = setPadKey(q, 100, 60);
        check("a pad past the defaults takes a key (prior -1)", r.ok && r.priorKey == -1 && q.padKey(100) == 60);
        r = setPadKey(q, 100, -1);
        check("...and key -1 removes its row again (the undo)", r.ok && q.pads.empty());

        // An unknown attribute on a row survives a reassignment.
        Program x = parseProgram(withPads("<Pad n=\"2\" key=\"3\" color=\"red\"/>")).program;
        setPadKey(x, 2, 4);
        check("a row's unknown attributes survive a reassignment",
              x.pads.size() == 1 && x.pads[0].key == 4 && x.pads[0].extras.attributes.size() == 1);
    }

    group("P5 a shared key selects a pad");
    {
        Program p = parseProgram(withPads("<Pad n=\"3\" key=\"40\"/><Pad n=\"20\" key=\"40\"/>")).program;
        // Pad 4's default is 40 too.
        const std::vector<int> on = padsOnKey(p, 40);
        check("pads 3, 4 and 20 play key 40, lowest first", on == std::vector<int>({3, 4, 20}));
        check("pad 3 no longer plays its default 39", padsOnKey(p, 39).empty());
        check("the selected pad stays when it is one of them (20)", padSelectedByKey(p, 40, 20) == 20);
        check("the selected pad stays when it is one of them (4)", padSelectedByKey(p, 40, 4) == 4);
        check("otherwise the lowest-numbered one (selected 7 -> 3)", padSelectedByKey(p, 40, 7) == 3);
        check("a key no pad plays selects nothing (-1)", padSelectedByKey(p, 20, 7) == -1);
        check("a key one pad plays selects it, switching bank (key 60 -> pad 24)", padSelectedByKey(p, 60, 0) == 24);
    }

    group("P6 the zones covering a key: one tom zone, three pads");
    {
        const Program p = parseProgram(kTomKit).program;
        bool allTom = true;
        for (int n : {4, 5, 6}) {
            const std::vector<uint32_t> z = zonesCoveringKey(p, p.padKey(n));
            if (z != std::vector<uint32_t>({3})) allTom = false;
        }
        check("pads 4, 5 and 6 each find zone 3 at their own key", allTom);
        check("pad 0 finds the kick zone", zonesCoveringKey(p, p.padKey(0)) == std::vector<uint32_t>({1}));
        check("pad 1 (key 37) finds no zone", zonesCoveringKey(p, p.padKey(1)).empty());

        Program layered;
        Zone soft, hard;
        soft.keyLo = soft.keyHi = 38;
        soft.velHi = 63;
        hard.keyLo = hard.keyHi = 38;
        hard.velLo = 64;
        addZone(layered, soft);
        addZone(layered, hard);
        check("two velocity zones on one key both cover it, in program order",
              zonesCoveringKey(layered, 38) == std::vector<uint32_t>({1, 2}));
    }

    group("P7 the zone attribute pad is kept and means nothing");
    {
        const char* xml =
            "<NassauDigitalProgram version=\"1\"><zone id=\"1\" keyLo=\"50\" keyHi=\"50\" pad=\"7\"/></NassauDigitalProgram>";
        const ProgramParseResult r = parseProgram(xml);
        check("pad=\"7\" is read", r.ok && r.program.zones[0].pad == 7);
        check("...and written back", r.ok && writeProgram(r.program).find("pad=\"7\"") != std::string::npos);
        check("pad 7 still plays its default key 43, not the zone's 50",
              r.ok && r.program.padKey(7) == 43 && padsOnKey(r.program, 50).size() == 1 &&
                  padsOnKey(r.program, 50)[0] == 14);
    }

    group("the table travels through a library overlay");
    {
        const Program base = parseProgram(kTomKit).program;
        Overlay o;
        Program current = base;
        recordZoneEdit(o, current, ZoneScope::zones({3}), {ParamEdit::absolute("tune", "1")});
        const OverlayResult applied = applyOverlay(base, o);
        const OverlayResult flat = flatten(base, o);
        check("applying an overlay keeps the base's pads", applied.program.pads == base.pads);
        check("a flattened program keeps them, and writes them",
              flat.program.pads == base.pads && writeProgram(flat.program).find("<Pad n=\"5\" key=\"48\"/>") !=
                                                    std::string::npos);
    }

    return ndtest::summary("pads_tests");
}
