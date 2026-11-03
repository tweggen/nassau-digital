#include "nd_program_xml.h"

#include <set>

namespace nd {

namespace {

constexpr const char* kZoneElement = "zone";
constexpr const char* kLayerElement = "layer";
constexpr const char* kPadsElement = "Pads";
constexpr const char* kPadElement = "Pad";

// One integer attribute of a <Pad> row, in [0, maxValue]: n 0..95 (six
// banks), key 0..127.
bool padNumber(const XmlNode& n, const char* attr, int maxValue, int& out, std::string& error) {
    const std::string* v = n.attribute(attr);
    long long i = 0;
    if (!v) {
        error = "<Pad> needs " + std::string(attr) + "=";
        return false;
    }
    if (!parseInteger(*v, i)) {
        error = "<Pad>: " + std::string(attr) + ": expected an integer, got \"" + *v + "\"";
        return false;
    }
    if (i < 0 || i > maxValue) {
        error = "<Pad>: " + std::string(attr) + ": " + *v + " is out of range [0, " + std::to_string(maxValue) + "]";
        return false;
    }
    out = static_cast<int>(i);
    return true;
}

// Reads the <Pads> table (DESIGN §13, settled 2026-11-03, pads). Refused: a
// row without n or key, an n outside 0..95 or a key outside 0..127, the same n
// twice. Unknown attributes and children are kept, of the table and of each
// row.
bool readPads(const XmlNode& pn, Program& prog, std::string& error) {
    for (const auto& a : pn.attributes) prog.padsExtras.attributes.push_back(a);
    std::set<int> seen;
    for (const XmlNode& rn : pn.children) {
        if (rn.name != kPadElement) {
            prog.padsExtras.children.push_back(rn);
            continue;
        }
        PadRow row;
        if (!padNumber(rn, "n", kNumPads - 1, row.n, error) || !padNumber(rn, "key", 127, row.key, error))
            return false;
        if (!seen.insert(row.n).second) {
            error = "<Pads>: pad " + std::to_string(row.n) + " has more than one row";
            return false;
        }
        for (const auto& a : rn.attributes)
            if (a.first != "n" && a.first != "key") row.extras.attributes.push_back(a);
        row.extras.children = rn.children;
        prog.pads.push_back(std::move(row));
    }
    return true;
}

bool parseId(const XmlNode& n, uint32_t& id, std::string& error) {
    const std::string* v = n.attribute("id");
    long long i = 0;
    if (!v || !parseInteger(*v, i) || i < 1 || i > 0xFFFFFFFFll) {
        error = "<" + n.name + "> needs an id, a positive integer";
        return false;
    }
    id = static_cast<uint32_t>(i);
    return true;
}

// Reads the attributes of `n` into `obj` through `table`; anything the table
// does not know (other than id) is kept in `extras`.
template <typename T>
bool readAttributes(const XmlNode& n, const std::vector<ParamDesc<T>>& table, T& obj, Extras& extras,
                    bool hasId, std::string& error) {
    for (const auto& a : n.attributes) {
        if (hasId && a.first == "id") continue;
        const ParamDesc<T>* p = nullptr;
        for (const auto& d : table)
            if (a.first == d.name) p = &d;
        if (!p) {
            extras.attributes.push_back(a);
            continue;
        }
        std::string why;
        if (!parseParam(*p, obj, a.second, why)) {
            error = "<" + n.name + ">: " + why;
            return false;
        }
    }
    return true;
}

template <typename T>
void writeAttributes(XmlNode& n, const std::vector<ParamDesc<T>>& table, const T& obj, const Extras& extras) {
    for (const auto& p : table) n.attributes.emplace_back(p.name, formatParam(p, obj));
    for (const auto& a : extras.attributes) n.attributes.push_back(a);
}

}  // namespace

ProgramParseResult programFromXml(const XmlNode& root) {
    ProgramParseResult r;
    Program& prog = r.program;
    if (root.name != kProgramElement) {
        r.error = "expected <" + std::string(kProgramElement) + ">, got <" + root.name + ">";
        return r;
    }
    // nextId is not a table parameter: it is bookkeeping, not something an
    // edit sets. Read it by hand.
    XmlNode rootAttrs = root;
    rootAttrs.attributes.clear();
    const std::string* nextIdText = nullptr;
    for (const auto& a : root.attributes) {
        if (a.first == "nextId") nextIdText = &a.second;
        else rootAttrs.attributes.push_back(a);
    }
    if (!readAttributes(rootAttrs, programParams(), prog, prog.extras, false, r.error)) return r;

    std::set<uint32_t> ids;
    uint32_t maxId = 0;  // the highest id below the overlay range
    auto claim = [&](uint32_t id) {
        if (!ids.insert(id).second) {
            r.error = "id " + std::to_string(id) + " is used twice";
            return false;
        }
        // An id from the overlay range (a flattened program keeps its
        // overlay's ids) does not move nextId: the program itself never hands
        // one out (DESIGN §8).
        if (id < kOverlayIdFirst && id > maxId) maxId = id;
        return true;
    };

    bool sawPads = false;
    for (const XmlNode& zn : root.children) {
        if (zn.name == kPadsElement) {
            if (sawPads) {
                r.error = "more than one <Pads> table";
                return r;
            }
            sawPads = true;
            if (!readPads(zn, prog, r.error)) return r;
            continue;
        }
        if (zn.name != kZoneElement) {
            prog.extras.children.push_back(zn);
            continue;
        }
        Zone z;
        if (!parseId(zn, z.id, r.error) || !claim(z.id)) return r;
        if (!readAttributes(zn, zoneParams(), z, z.extras, true, r.error)) return r;
        for (const XmlNode& ln : zn.children) {
            if (ln.name != kLayerElement) {
                z.extras.children.push_back(ln);
                continue;
            }
            Layer l;
            if (!parseId(ln, l.id, r.error) || !claim(l.id)) return r;
            if (!readAttributes(ln, layerParams(), l, l.extras, true, r.error)) return r;
            l.extras.children = ln.children;
            if (static_cast<int>(z.layers.size()) >= kMaxLayers) {
                r.error = "zone " + std::to_string(z.id) + " has more than " + std::to_string(kMaxLayers) + " layers";
                return r;
            }
            z.layers.push_back(std::move(l));
        }
        prog.zones.push_back(std::move(z));
    }

    long long next = 1;
    if (nextIdText && (!parseInteger(*nextIdText, next) || next < 1 || next > 0xFFFFFFFFll)) {
        r.error = "nextId: expected a positive integer, got \"" + *nextIdText + "\"";
        return r;
    }
    if (next > static_cast<long long>(kOverlayIdFirst)) {
        // nextId == kOverlayIdFirst is a program that has used up its range;
        // beyond it, the program would hand out an overlay's ids.
        r.error = "nextId: " + *nextIdText + " is in the range reserved for overlays (from " +
                  std::to_string(kOverlayIdFirst) + ")";
        return r;
    }
    // A file whose nextId lags behind its own ids would hand out a used id;
    // never let that happen (DESIGN §2: ids are never reused).
    prog.nextId = static_cast<uint32_t>(next);
    if (prog.nextId <= maxId) prog.nextId = maxId + 1;
    r.ok = true;
    return r;
}

ProgramParseResult parseProgram(const std::string& xml) {
    const XmlParseResult x = parseXml(xml);
    if (!x.ok) {
        ProgramParseResult r;
        r.error = "XML: " + x.error;
        return r;
    }
    return programFromXml(x.root);
}

XmlNode programToXml(const Program& prog) {
    XmlNode root;
    root.name = kProgramElement;
    writeAttributes(root, programParams(), prog, Extras{});
    // libraryVersion only when set: a program that is not a library has no
    // version, and every program written before 2026-11-01 stays as it was.
    if (prog.libraryVersion.empty()) {
        for (auto it = root.attributes.begin(); it != root.attributes.end(); ++it) {
            if (it->first == "libraryVersion") {
                root.attributes.erase(it);
                break;
            }
        }
    }
    root.attributes.emplace_back("nextId", std::to_string(prog.nextId));
    for (const auto& a : prog.extras.attributes) root.attributes.push_back(a);
    for (const Zone& z : prog.zones) {
        XmlNode zn;
        zn.name = kZoneElement;
        zn.attributes.emplace_back("id", std::to_string(z.id));
        writeAttributes(zn, zoneParams(), z, z.extras);
        for (const Layer& l : z.layers) {
            XmlNode ln;
            ln.name = kLayerElement;
            ln.attributes.emplace_back("id", std::to_string(l.id));
            writeAttributes(ln, layerParams(), l, l.extras);
            ln.children = l.extras.children;
            zn.children.push_back(std::move(ln));
        }
        for (const auto& c : z.extras.children) zn.children.push_back(c);
        root.children.push_back(std::move(zn));
    }
    // The pads table, after the zones, and only when there is something in
    // it: a program whose pads are all on their default keys writes no
    // <Pads>, so every program written before 2026-11-03 stays as it was.
    if (!prog.pads.empty() || !prog.padsExtras.attributes.empty() || !prog.padsExtras.children.empty()) {
        XmlNode pn;
        pn.name = kPadsElement;
        pn.attributes = prog.padsExtras.attributes;
        for (const PadRow& row : prog.pads) {
            XmlNode rn;
            rn.name = kPadElement;
            rn.attributes.emplace_back("n", std::to_string(row.n));
            rn.attributes.emplace_back("key", std::to_string(row.key));
            for (const auto& a : row.extras.attributes) rn.attributes.push_back(a);
            rn.children = row.extras.children;
            pn.children.push_back(std::move(rn));
        }
        for (const auto& c : prog.padsExtras.children) pn.children.push_back(c);
        root.children.push_back(std::move(pn));
    }
    for (const auto& c : prog.extras.children) root.children.push_back(c);
    return root;
}

std::string writeProgram(const Program& prog) { return writeXml(programToXml(prog)); }

}  // namespace nd
