// program_tests.cpp - C1: the program model and its XML (DESIGN §2, §2.1, §12).
//
// C1.1 parse -> write -> parse is the identity, on a fixture covering every
//      element and attribute (checked here, not assumed).
// C1.2 unknown attributes and elements survive the round trip.
// C1.3 ids are never reused.
// C1.4 the ALL scope: absolute reaches every zone, relative moves each zone
//      from its own value and clamps per zone, relative on an enum is refused.
// C1.5 every edit returns what it replaced, and applying that restores the
//      program exactly.

#include "nd_edit.h"
#include "nd_program.h"
#include "nd_program_xml.h"
#include "nd_xml.h"
#include "test_util.h"

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

using namespace nd;
using ndtest::check;
using ndtest::group;
using ndtest::note;

namespace {

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

Program loadFixture() {
    const ProgramParseResult r = parseProgram(readFile(ND_FIXTURE_PROGRAM_FULL));
    if (!r.ok) note("fixture did not parse: " + r.error);
    return r.program;
}

template <typename T>
bool anyNonDefault(const std::vector<const T*>& objs, const ParamDesc<T>& p) {
    const T def{};
    for (const T* o : objs) {
        if (p.kind == ParamKind::String) {
            if (p.getText(*o) != p.getText(def)) return true;
        } else if (p.get(*o) != p.get(def)) {
            return true;
        }
    }
    return false;
}

template <typename T>
bool everyEnumValueUsed(const std::vector<const T*>& objs, const ParamDesc<T>& p) {
    std::set<int> seen;
    for (const T* o : objs) seen.insert(static_cast<int>(p.get(*o)));
    return static_cast<int>(seen.size()) == p.numEnumNames;
}

Program threeZones() {
    Program p;
    Zone a, b, c;
    a.tune = -95.0;  // near the bottom of [-96, 96]
    b.tune = 0.0;
    c.tune = 95.5;   // near the top
    a.cutoff = 100.0;
    b.cutoff = 5000.0;
    c.cutoff = 19000.0;
    a.filter = FilterType::Lp12;
    a.layers.push_back(Layer{});
    b.layers.push_back(Layer{});
    b.layers.push_back(Layer{});
    addZone(p, a);
    addZone(p, b);
    addZone(p, c);
    return p;
}

}  // namespace

int main() {
    std::printf("=== NassauDigital core: program model and XML (C1) ===\n");

    // ---------------------------------------------------------------- XML ---
    group("XML reader/writer basics");
    {
        const auto r = parseXml("<a x='1' y=\"&lt;2&gt;\"><b/>t&amp;u<!-- c --><c z=\"&#65;&#x42;\">v</c></a>");
        check("parses attributes, entities, children, text", r.ok && r.root.name == "a" &&
                                                                 r.root.attributes.size() == 2 &&
                                                                 *r.root.attribute("y") == "<2>" &&
                                                                 r.root.children.size() == 2 &&
                                                                 r.root.text == "t&u" &&
                                                                 *r.root.children[1].attribute("z") == "AB");
        const auto again = parseXml(writeXml(r.root));
        check("write -> parse is the identity on a node", again.ok && again.root == r.root);
        check("refuses a mismatched end tag", !parseXml("<a><b></a></b>").ok);
        check("refuses an unknown entity", !parseXml("<a x=\"&foo;\"/>").ok);
        check("refuses a duplicate attribute", !parseXml("<a x=\"1\" x=\"2\"/>").ok);
        check("refuses a DOCTYPE", !parseXml("<!DOCTYPE a><a/>").ok);
        check("refuses content after the root", !parseXml("<a/><b/>").ok);
        check("refuses an unterminated document", !parseXml("<a><b/>").ok);
        const auto e = parseXml("<a>\n<b/>\n  <c\n x=1\n  y='2'/>\n</a>");
        check("reports the line of an error", !e.ok && e.error.find("line 4") != std::string::npos);
        note("error text: " + e.error);
    }

    group("Number formatting is locale-independent and round-trips");
    {
        const double vals[] = {0.2, 1.0 / 3.0, -3.5, 1e-9, 20000.0, 0.1 + 0.2, 123456789.125};
        bool exact = true;
        for (double v : vals) {
            double back = 0;
            if (!parseReal(formatReal(v), back) || back != v) exact = false;
        }
        check("formatReal -> parseReal is exact for awkward doubles", exact);
        check("0.2 is written as \"0.2\", not 17 digits", formatReal(0.2) == "0.2");
        double x = 0;
        check("parseReal refuses \"0,5\", \"nan\", \"inf\", \"\", \"1x\"",
              !parseReal("0,5", x) && !parseReal("nan", x) && !parseReal("inf", x) && !parseReal("", x) &&
                  !parseReal("1x", x));
    }

    // ---------------------------------------------------------------- C1.1 ---
    group("C1.1 parse -> write -> parse yields the identical structure");
    {
        const std::string text = readFile(ND_FIXTURE_PROGRAM_FULL);
        const ProgramParseResult first = parseProgram(text);
        check("the fixture parses", first.ok);
        if (!first.ok) note(first.error);
        const Program& p = first.program;

        // The fixture's coverage is measured, not assumed.
        std::vector<const Zone*> zones;
        std::vector<const Layer*> layers;
        for (const Zone& z : p.zones) {
            zones.push_back(&z);
            for (const Layer& l : z.layers) layers.push_back(&l);
        }
        std::vector<std::string> uncovered;
        for (const auto& d : zoneParams()) {
            if (text.find(std::string(d.name) + "=\"") == std::string::npos || !anyNonDefault(zones, d))
                uncovered.push_back(std::string("zone.") + d.name);
            // Every value of every enum, except the nine outputs (main and
            // three aux are enough to show the mapping).
            if (d.kind == ParamKind::Enum && std::string(d.name) != "output" && !everyEnumValueUsed(zones, d))
                uncovered.push_back(std::string("zone.") + d.name + " (every value)");
        }
        for (const auto& d : layerParams())
            if (text.find(std::string(d.name) + "=\"") == std::string::npos || !anyNonDefault(layers, d))
                uncovered.push_back(std::string("layer.") + d.name);
        std::vector<const Program*> progs{&p};
        for (const auto& d : programParams()) {
            if (std::string(d.name) == "version") continue;  // 1 is the only version there is
            if (text.find(std::string(d.name) + "=\"") == std::string::npos || !anyNonDefault(progs, d))
                uncovered.push_back(std::string("program.") + d.name);
        }
        std::string list;
        for (const auto& u : uncovered) list += u + " ";
        check("the fixture sets every zone, layer and program attribute to a non-default value, and every "
              "enum value occurs" + (list.empty() ? std::string() : " -- missing: " + list),
              uncovered.empty());
        check("the fixture has zones with 1, 2 and 4 layers and two with none",
              p.zones.size() == 5 && p.zones[0].layers.size() == 2 && p.zones[1].layers.size() == 4 &&
                  p.zones[2].layers.size() == 1 && p.zones[3].layers.empty());
        check("entities decoded in the name", p.name == "Kit & \"Friends\" \xE2\x80\x93 1");
        check("a parsed value is what the file says (zone 1 velToStart=441, layer 2 loopEnd=40000)",
              p.zones[0].velToStart == 441.0 && p.zones[0].layers[0].loopEnd == 40000 &&
                  p.zones[0].output == 1 && p.zones[1].output == 8 && p.zones[0].layers[0].reverse);

        const std::string written = writeProgram(p);
        const ProgramParseResult second = parseProgram(written);
        check("the written program parses", second.ok);
        check("parse(write(parse(fixture))) == parse(fixture), structurally", second.ok && second.program == p);
        check("write is stable: write(parse(write(x))) == write(x), byte for byte",
              second.ok && writeProgram(second.program) == written);
        check("nextId survives (13)", second.program.nextId == 13);

        // Equality must be able to fail, or the checks above prove nothing.
        Program q = p;
        q.zones[0].layers[1].velFade = 9;
        Program r2 = p;
        r2.zones[2].extras.attributes.emplace_back("x", "y");
        Program r3 = p;
        r3.zones[0].lfoToPan = 0.2500001;
        check("structural equality notices a one-field change anywhere (layer, extras, zone)",
              !(q == p) && !(r2 == p) && !(r3 == p));
    }

    // ------------------------------------------------------------ filter off ---
    group("filter off (DESIGN §3, decided 2026-10-27): an appended enum value, and the default");
    {
        // Append-only (R4): the four filters keep their numbers, off is the fifth.
        check("FilterType numbers: lp24 0, lp12 1, hp12 2, hp24 3, off 4",
              static_cast<int>(FilterType::Lp24) == 0 && static_cast<int>(FilterType::Lp12) == 1 &&
                  static_cast<int>(FilterType::Hp12) == 2 && static_cast<int>(FilterType::Hp24) == 3 &&
                  static_cast<int>(FilterType::Off) == 4);
        const ZoneParam* fp = nullptr;
        for (const auto& d : zoneParams())
            if (std::string(d.name) == "filter") fp = &d;
        std::string spelled;
        if (fp) {
            for (int i = 0; i < fp->numEnumNames; ++i) spelled += std::string(i ? " " : "") + fp->enumNames[i];
        }
        note("filter values in the table, by number: " + spelled);
        check("the table spells them lp24 lp12 hp12 hp24 off, in that order", spelled == "lp24 lp12 hp12 hp24 off");
        check("a default Zone's filter is off", Zone{}.filter == FilterType::Off);

        const auto noAttr = parseProgram("<NassauDigitalProgram version=\"1\"><zone id=\"1\"/></NassauDigitalProgram>");
        check("a zone with no filter attribute reads as off",
              noAttr.ok && noAttr.program.zones.size() == 1 && noAttr.program.zones[0].filter == FilterType::Off);
        const auto offAttr =
            parseProgram("<NassauDigitalProgram version=\"1\"><zone id=\"1\" filter=\"off\"/></NassauDigitalProgram>");
        check("filter=\"off\" reads as off", offAttr.ok && offAttr.program.zones[0].filter == FilterType::Off);
        const auto lpAttr =
            parseProgram("<NassauDigitalProgram version=\"1\"><zone id=\"1\" filter=\"lp24\"/></NassauDigitalProgram>");
        check("a program that names lp24 keeps it", lpAttr.ok && lpAttr.program.zones[0].filter == FilterType::Lp24);
        const auto bad =
            parseProgram("<NassauDigitalProgram version=\"1\"><zone id=\"1\" filter=\"none\"/></NassauDigitalProgram>");
        check("an unknown filter name is refused (\"none\")", !bad.ok);

        // The writer writes every attribute, defaults included (C1), so off
        // is written out as filter="off", and reads back as off.
        const std::string written = writeProgram(noAttr.program);
        note("written: " + written.substr(0, std::min<size_t>(written.size(), 160)) + "...");
        check("written as filter=\"off\"", written.find(" filter=\"off\"") != std::string::npos);
        const auto back = parseProgram(written);
        check("...and read back as off, the same program", back.ok && back.program == noAttr.program &&
                                                               back.program.zones[0].filter == FilterType::Off);

        // An edit can choose it and leave it, like any enum value.
        Program q = lpAttr.program;
        const auto toOff = setZones(q, ZoneScope::everyZone(), {ParamEdit::absolute("filter", "off")});
        const bool wasOff = q.zones[0].filter == FilterType::Off;
        const auto toLp = setZones(q, ZoneScope::everyZone(), {ParamEdit::absolute("filter", "lp24")});
        check("an absolute edit sets filter off and back to lp24",
              toOff.ok && wasOff && toLp.ok && q.zones[0].filter == FilterType::Lp24);
    }

    // ---------------------------------------------------------------- C1.2 ---
    group("C1.2 unknown attributes and elements survive the round trip");
    {
        const Program p = loadFixture();
        const Program back = parseProgram(writeProgram(p)).program;
        auto hasAttr = [](const Extras& e, const char* k, const char* v) {
            for (const auto& a : e.attributes)
                if (a.first == k && a.second == v) return true;
            return false;
        };
        check("unknown program attribute (with escaped text) survives",
              hasAttr(back.extras, "futureProgramAttr", "kept <verbatim>"));
        check("unknown zone attribute survives", hasAttr(back.zones[0].extras, "futureZoneAttr", "z"));
        check("unknown layer attribute survives", hasAttr(back.zones[0].layers[0].extras, "futureLayerAttr", "l"));
        check("unknown program child with nested element, text and CDATA survives",
              back.extras.children.size() == 1 && back.extras.children[0].name == "futureProgramChild" &&
                  back.extras.children[0].children.size() == 1 &&
                  back.extras.children[0].children[0].text == "text & more<raw>" &&
                  back.extras.children[0] == p.extras.children[0]);
        check("unknown zone child survives",
              back.zones[0].extras.children.size() == 1 && back.zones[0].extras.children[0].name == "futureZoneChild");
        check("unknown layer child with text survives",
              back.zones[0].layers[1].extras.children.size() == 1 &&
                  back.zones[0].layers[1].extras.children[0].text == "layer text");
        Program edited = back;
        const auto res = setZones(edited, ZoneScope::everyZone(), {ParamEdit::by("tune", 1.0)});
        const Program again = parseProgram(writeProgram(edited)).program;
        check("unknowns also survive an edit and a second round trip",
              res.ok && again.extras == p.extras && again.zones[0].extras == p.zones[0].extras &&
                  again.zones[0].layers[1].extras == p.zones[0].layers[1].extras);
    }

    group("Refused on load (with a message)");
    {
        auto refuses = [](const std::string& xml, const char* what) {
            const auto r = parseProgram(xml);
            check(std::string("refuses ") + what + (r.ok ? "" : "  [" + r.error + "]"), !r.ok);
        };
        refuses("<NassauDigitalProgram><zone keyLo=\"1\"/></NassauDigitalProgram>", "a zone without an id");
        refuses("<NassauDigitalProgram><zone id=\"1\"/><zone id=\"1\"/></NassauDigitalProgram>", "a duplicate id");
        refuses("<NassauDigitalProgram><zone id=\"1\"><layer id=\"1\"/></zone></NassauDigitalProgram>",
                "a layer reusing a zone's id");
        refuses("<NassauDigitalProgram><zone id=\"1\" mode=\"granular\"/></NassauDigitalProgram>",
                "an unknown enum value");
        refuses("<NassauDigitalProgram><zone id=\"1\" keyHi=\"128\"/></NassauDigitalProgram>", "a value out of range");
        refuses("<NassauDigitalProgram><zone id=\"1\" tune=\"0,5\"/></NassauDigitalProgram>", "a malformed number");
        refuses("<NassauDigitalProgram polyphony=\"48\"/>", "a polyphony outside 8..256 in powers of two");
        refuses("<NassauDigitalProgram><zone id=\"1\"><layer id=\"2\"/><layer id=\"3\"/><layer id=\"4\"/>"
                "<layer id=\"5\"/><layer id=\"6\"/></zone></NassauDigitalProgram>",
                "a fifth layer");
        refuses("<SomethingElse/>", "another root element");
        const auto lag = parseProgram("<NassauDigitalProgram nextId=\"2\"><zone id=\"7\"/></NassauDigitalProgram>");
        check("a nextId lagging behind the file's ids is raised past them (7 -> 8)", lag.ok && lag.program.nextId == 8);
        const auto bare = parseProgram("<NassauDigitalProgram/>");
        check("a bare program takes the defaults (polyphony 32, nextId 1)",
              bare.ok && bare.program.polyphony == 32 && bare.program.nextId == 1);
    }

    // ---------------------------------------------------------------- C1.3 ---
    group("C1.3 ids are never reused");
    {
        Program p = loadFixture();  // highest id 12 (the last zone), nextId 13
        Program q;
        const uint32_t z1 = addZone(q, Zone{});
        Zone withLayers;
        withLayers.layers.resize(2);
        const uint32_t z2 = addZone(q, withLayers);
        check("addZone hands out nextId to the zone and then to each layer (1; 2 with layers 3, 4)",
              z1 == 1 && z2 == 2 && q.zones[1].layers[0].id == 3 && q.zones[1].layers[1].id == 4 && q.nextId == 5);
        const uint32_t oldNext = q.nextId;
        const RemovedZone gone = removeZone(q, z2);  // holds the highest ids
        const uint32_t z3 = addZone(q, Zone{});
        check("delete the zone with the highest id, add one: the new id equals the old nextId (5)",
              gone.ok && z3 == oldNext && z3 == 5);
        note("old nextId " + std::to_string(oldNext) + ", new zone id " + std::to_string(z3));

        const uint32_t oldNextP = p.nextId;
        const RemovedZone g2 = removeZone(p, 12);  // the zone with the highest id
        const uint32_t z4 = addZone(p, Zone{});
        const ProgramParseResult rt = parseProgram(writeProgram(p));
        check("the same through a fixture and a save: new id 13, and nextId 14 survives the save",
              g2.ok && z4 == oldNextP && z4 == 13 && rt.ok && rt.program.nextId == 14);
        const uint32_t l = addLayer(p, 4, Layer{});
        check("addLayer takes the next id too (14)", l == 14);
        check("a fifth layer is refused", addLayer(p, 5, Layer{}) == 0 && p.nextId == 15);
    }

    // ---------------------------------------------------------------- C1.4 ---
    group("C1.4 the ALL scope");
    {
        Program p = threeZones();
        const auto abs = setZones(p, ZoneScope::everyZone(), {ParamEdit::absolute("attack", "0.01")});
        check("an absolute edit on ALL reaches every zone",
              abs.ok && p.zones[0].attack == 0.01 && p.zones[1].attack == 0.01 && p.zones[2].attack == 0.01);
        const auto absEnum = setZones(p, ZoneScope::everyZone(), {ParamEdit::absolute("filter", "hp12")});
        check("an absolute edit of an enum on ALL reaches every zone",
              absEnum.ok && p.zones[0].filter == FilterType::Hp12 && p.zones[2].filter == FilterType::Hp12);

        Program q = threeZones();
        const auto rel = setZones(q, ZoneScope::everyZone(), {ParamEdit::by("tune", 2.0)});
        note("tune before -95 / 0 / 95.5, after " + formatReal(q.zones[0].tune) + " / " + formatReal(q.zones[1].tune) +
             " / " + formatReal(q.zones[2].tune));
        check("a relative edit (tune +2) moves each zone from its own value (-93, 2)",
              rel.ok && q.zones[0].tune == -93.0 && q.zones[1].tune == 2.0);
        check("...and clamps per zone (95.5 + 2 -> 96, the top of the range)", q.zones[2].tune == 96.0);
        const auto relDown = setZones(q, ZoneScope::everyZone(), {ParamEdit::by("cutoff", -150.0)});
        check("a relative edit clamps each zone at the bottom independently (100 -> 20, 5000 -> 4850)",
              relDown.ok && q.zones[0].cutoff == 20.0 && q.zones[1].cutoff == 4850.0 && q.zones[2].cutoff == 18850.0);
        const auto back = setZones(q, ZoneScope::everyZone(), {ParamEdit::by("tune", -2.0)});
        check("the opposite relative edit does not invert a clamped one (96 - 2 = 94, not 95.5)",
              back.ok && q.zones[2].tune == 94.0);
        const auto relInt = setZones(q, ZoneScope::everyZone(), {ParamEdit::by("keyHi", 5.0)});
        check("a relative edit of an integer clamps at 127", relInt.ok && q.zones[0].keyHi == 127);

        const Program before = q;
        const char* enums[] = {"filter", "driveCurve", "mode", "tempoMethod", "output", "keyTrack"};
        bool allRefused = true;
        std::string msgs;
        for (const char* e : enums) {
            const auto r = setZones(q, ZoneScope::everyZone(), {ParamEdit::by(e, 1.0)});
            if (r.ok) allRefused = false;
            msgs = r.error;
        }
        check("a relative edit on an enum (filter, driveCurve, mode, tempoMethod, output, keyTrack) is refused",
              allRefused);
        note("message: " + msgs);
        const auto mixed = setZones(q, ZoneScope::everyZone(), {ParamEdit::by("tune", 1.0), ParamEdit::by("mode", 1.0)});
        check("an edit mixing a valid relative and a refused one changes nothing (all-or-nothing)",
              !mixed.ok && q == before);
        const auto bad = setZones(q, ZoneScope::everyZone(), {ParamEdit::absolute("cutoff", "30000")});
        const auto unk = setZones(q, ZoneScope::everyZone(), {ParamEdit::absolute("nonsense", "1")});
        const auto noZone = setZones(q, ZoneScope::zones({1, 99}), {ParamEdit::absolute("tune", "1")});
        check("refused too, changing nothing: an out-of-range absolute value, an unknown parameter, a missing zone",
              !bad.ok && !unk.ok && !noZone.ok && q == before);

        Program s = threeZones();
        const auto sel = setZones(s, ZoneScope::zones({1, 6}), {ParamEdit::by("tune", 1.0)});
        check("a selection reaches only the selected zones", sel.ok && s.zones[0].tune == -94.0 &&
                                                                 s.zones[1].tune == 0.0 && s.zones[2].tune == 96.0);
    }

    // ---------------------------------------------------------------- C1.5 ---
    group("C1.5 every edit returns what it replaced; applying it restores the program");
    {
        const Program base = loadFixture();
        int restored = 0, total = 0;
        auto expectRestore = [&](const char* what, bool ok) {
            ++total;
            if (ok) ++restored;
            check(what, ok);
        };

        {
            Program p = base;
            const auto r = setZones(p, ZoneScope::everyZone(),
                                    {ParamEdit::by("tune", 90.0), ParamEdit::by("cutoff", 15000.0),
                                     ParamEdit::absolute("mode", "loop"), ParamEdit::absolute("attack", "1.5")});
            const bool changed = !(p == base);
            const auto u = restoreZones(p, r.prior);
            expectRestore("ALL, relative (clamping) + absolute + enum: restore gives back the program exactly",
                          r.ok && changed && u.ok && p == base);
            check("the prior values are absolute: 5 zones x 4 parameters = 20, zone 1's tune was \"2\"",
                  r.prior.size() == 20 && r.prior[0] == PriorValue{1, "tune", "2"});
            // The restore itself returns what it replaced: redo.
            Program redo = base;
            setZones(redo, ZoneScope::everyZone(),
                     {ParamEdit::by("tune", 90.0), ParamEdit::by("cutoff", 15000.0),
                      ParamEdit::absolute("mode", "loop"), ParamEdit::absolute("attack", "1.5")});
            Program again = p;
            restoreZones(again, u.prior);
            expectRestore("the restore returns the values it replaced, so redo is a restore too", again == redo);
        }
        {
            Program p = base;
            const auto r = setLayers(p, {2, 9, 11}, {ParamEdit::absolute("src", "x.wav"), ParamEdit::by("velFade", 200.0),
                                                     ParamEdit::absolute("reverse", "1")});
            const auto u = restoreLayers(p, r.prior);
            expectRestore("set-layer (text, clamped integer, bool) restores exactly", r.ok && u.ok && p == base);
        }
        {
            Program p = base;
            const auto r = setProgramParams(p, {ParamEdit::absolute("name", "Renamed"),
                                                ParamEdit::absolute("polyphony", "256"), ParamEdit::by("busDrive", 5.0)});
            const auto u = restoreProgramParams(p, r.prior);
            expectRestore("program parameters restore exactly", r.ok && u.ok && p == base);
            check("relative polyphony is refused (fixed set of choices)",
                  !setProgramParams(p, {ParamEdit::by("polyphony", 1.0)}).ok);
        }
        {
            Program p = base;
            const uint32_t id = addZone(p, Zone{});
            const RemovedZone r = removeZone(p, id);
            Program expected = base;
            expected.nextId = p.nextId;  // an add consumes an id for good (C1.3)
            expectRestore("add-zone is undone by removing the id it returned (nextId stays advanced)",
                          id != 0 && r.ok && p == expected);
        }
        {
            Program p = base;
            const RemovedZone r = removeZone(p, 5);
            const bool gone = p.findZone(5) == nullptr;
            expectRestore("remove-zone is undone by reinsert, at its place, with its ids and layers",
                          r.ok && gone && reinsertZone(p, r) && p == base);
            const RemovedLayer rl = removeLayer(p, 7);
            expectRestore("remove-layer is undone by reinsert", rl.ok && reinsertLayer(p, rl) && p == base);
        }
        {
            Program p = base;
            const auto r = setZones(p, ZoneScope::zones({5}), {ParamEdit::absolute("tune", "3")});
            removeZone(p, 5);
            check("a restore naming a zone that no longer exists is refused, not half-applied",
                  r.ok && !restoreZones(p, r.prior).ok);
        }
        note(std::to_string(restored) + "/" + std::to_string(total) + " kinds of edit restored exactly");
    }

    return ndtest::summary("program_tests");
}
