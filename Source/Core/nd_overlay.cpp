#include "nd_overlay.h"

#include <algorithm>

#include "nd_edit.h"

namespace nd {

namespace {

constexpr const char* kSetZone = "set-zone";
constexpr const char* kSetLayer = "set-layer";
constexpr const char* kSetProgram = "set-program";
constexpr const char* kAddZone = "add-zone";
constexpr const char* kAddLayer = "add-layer";
constexpr const char* kHideZone = "hide-zone";
constexpr const char* kHideLayer = "hide-layer";
constexpr const char* kSetPad = "set-pad";
constexpr const char* kHidePad = "hide-pad";
constexpr const char* kNoKey = "none";  ///< set-pad key.was of a pad 92..95 without a row
constexpr const char* kWasSuffix = ".was";
constexpr size_t kWasLength = 4;

bool parseTargetId(const std::string& text, uint32_t& id) {
    long long v = 0;
    if (!parseInteger(text, v) || v < 1 || v > 0xFFFFFFFFll) return false;
    id = static_cast<uint32_t>(v);
    return true;
}

bool endsWithWas(const std::string& name) {
    return name.size() > kWasLength && name.compare(name.size() - kWasLength, kWasLength, kWasSuffix) == 0;
}

// One parameter of a stored verb: its new value and the value it expects to
// replace, both as written.
struct StoredParam {
    std::string attr, value, was;
};

// Reads the parameter pairs of a verb (every attribute but the addressing
// one). Refused: a parameter without its `.was` or a `.was` without its
// parameter (DESIGN §8: every edit stores the value it expects to replace),
// and a numeric value or `.was` written "+n" - the command language's
// relative form, resolved when an edit is made and never stored (§8). Parsed
// as a plain number it would silently become the absolute n.
template <typename T>
bool storedParams(const XmlNode& verb, const char* addressAttr, const ParamDesc<T>* (*find)(const std::string&),
                  std::vector<StoredParam>& out, std::string& error) {
    for (const auto& a : verb.attributes) {
        if ((addressAttr && a.first == addressAttr) || endsWithWas(a.first)) continue;
        const std::string* was = verb.attribute(a.first + kWasSuffix);
        if (!was) {
            error = a.first + ": no " + a.first + kWasSuffix + " (the value it replaces)";
            return false;
        }
        out.push_back({a.first, a.second, *was});
    }
    for (const auto& a : verb.attributes) {
        if (!endsWithWas(a.first)) continue;
        const std::string param = a.first.substr(0, a.first.size() - kWasLength);
        if (!verb.attribute(param)) {
            error = a.first + " without " + param;
            return false;
        }
    }
    for (const StoredParam& s : out) {
        const ParamDesc<T>* p = find(s.attr);
        if (!p) continue;  // checkDrift names the unknown parameter
        if (isNumeric(*p) && ((!s.value.empty() && s.value[0] == '+') || (!s.was.empty() && s.was[0] == '+'))) {
            error = s.attr + ": a relative value is resolved when an edit is made and is never stored";
            return false;
        }
    }
    return true;
}

// Compares each `.was` with the target's current value exactly as values
// round-trip through the format: the `.was` text is parsed into a copy of
// the target and both values are written with formatParam (the shortest
// text that reads back to the same double), and the two texts compared. So
// "0.10" and "0.1" agree, and any difference in the stored value - however
// small - is a difference; there is no tolerance to tune.
template <typename T>
bool checkDrift(const T& target, const std::vector<StoredParam>& params, const ParamDesc<T>* (*find)(const std::string&),
                std::vector<OverlayDrift>& drift, std::string& error) {
    for (const StoredParam& s : params) {
        const ParamDesc<T>* p = find(s.attr);
        if (!p) {
            error = "unknown parameter \"" + s.attr + "\"";
            return false;
        }
        T scratch = target;
        std::string why;
        if (!parseParam(*p, scratch, s.was, why)) {
            error = s.attr + kWasSuffix + ": " + why;
            return false;
        }
        const std::string was = formatParam(*p, scratch), now = formatParam(*p, target);
        if (was != now) drift.push_back({s.attr, was, now});
    }
    return true;
}

std::vector<ParamEdit> asEdits(const std::vector<StoredParam>& params) {
    std::vector<ParamEdit> out;
    for (const StoredParam& s : params) out.push_back(ParamEdit::absolute(s.attr, s.value));
    return out;
}

bool idInUse(const Program& prog, uint32_t id) {
    return prog.findZone(id) != nullptr || prog.findLayer(id).second != nullptr;
}

OverlayEditReport refused(OverlayEditReport rep, const std::string& verb, const std::string& why) {
    rep.state = OverlayEditState::Refused;
    rep.error = "<" + verb + ">: " + why;
    return rep;
}

// A stored zone address that the command language would expand: "all" or a
// list ("3 5 7"). Expanded when the edit is made (DESIGN §8); stored, it
// would also reach zones a later base adds, or address several targets with
// one `.was`: refused, not guessed.
bool needsExpansion(const std::string& target) {
    return target == "all" || target.find_first_of(" \t") != std::string::npos;
}

// Reads the attributes of an add-zone / add-layer into `obj` through
// `table`, skipping the addressing ones; unknown attributes are kept in
// `extras` (R4), as the program reader keeps them. An added object replaces
// nothing, so a `.was` is refused.
template <typename T>
bool readAdded(const XmlNode& verb, const std::vector<ParamDesc<T>>& table, const char* skip1, const char* skip2,
               T& obj, Extras& extras, std::string& error) {
    for (const auto& a : verb.attributes) {
        if (a.first == skip1 || (skip2 && a.first == skip2)) continue;
        if (endsWithWas(a.first)) {
            error = a.first + ": an added object replaces nothing";
            return false;
        }
        const ParamDesc<T>* p = nullptr;
        for (const auto& d : table)
            if (a.first == d.name) p = &d;
        if (!p) {
            extras.attributes.push_back(a);
            continue;
        }
        if (!parseParam(*p, obj, a.second, error)) return false;
    }
    extras.children = verb.children;
    return true;
}

// The id an add-zone / add-layer gives its object: from the overlay range,
// and not one the base already has. A base never hands out an id from the
// range (nd_edit, nd_program_xml), so a newer base that adds zones cannot
// collide; a base that carries one anyway (a flattened program used as a
// base) is told, not merged.
bool addedId(const XmlNode& verb, const char* attr, const Program& prog, uint32_t& id, std::string& error) {
    const std::string* t = verb.attribute(attr);
    if (!t || !parseTargetId(*t, id)) {
        error = std::string("needs ") + attr + "=\"ID\"";
        return false;
    }
    if (id < kOverlayIdFirst) {
        error = "id " + *t + " is not in the overlay range (from " + std::to_string(kOverlayIdFirst) + ")";
        return false;
    }
    if (idInUse(prog, id)) {
        error = "id " + *t + " is already in the base";
        return false;
    }
    return true;
}

OverlayEditReport applyAddZone(Program& prog, const XmlNode& verb, OverlayEditReport rep) {
    Zone z;
    std::string why;
    if (!addedId(verb, "zone", prog, z.id, why)) return refused(rep, verb.name, why);
    if (!readAdded<Zone>(verb, zoneParams(), "zone", nullptr, z, z.extras, why)) return refused(rep, verb.name, why);
    prog.zones.push_back(std::move(z));
    return rep;
}

OverlayEditReport applyAddLayer(Program& prog, const XmlNode& verb, OverlayEditReport rep) {
    Layer l;
    std::string why;
    const std::string* zt = verb.attribute("zone");
    uint32_t zoneId = 0;
    if (!zt || needsExpansion(*zt) || !parseTargetId(*zt, zoneId))
        return refused(rep, verb.name, "needs zone=\"ID\", the zone it adds to");
    if (!addedId(verb, "layer", prog, l.id, why)) return refused(rep, verb.name, why);
    Zone* z = prog.findZone(zoneId);
    if (!z) {
        rep.state = OverlayEditState::Orphaned;
        rep.missingId = zoneId;
        return rep;
    }
    if (static_cast<int>(z->layers.size()) >= kMaxLayers)
        return refused(rep, verb.name, "zone " + *zt + " already has " + std::to_string(kMaxLayers) + " layers");
    if (!readAdded<Layer>(verb, layerParams(), "layer", "zone", l, l.extras, why)) return refused(rep, verb.name, why);
    z->layers.push_back(std::move(l));
    return rep;
}

OverlayEditReport applyHideZone(Program& prog, const XmlNode& verb, OverlayEditReport rep) {
    const std::string* t = verb.attribute("zone");
    uint32_t id = 0;
    if (t && needsExpansion(*t)) return refused(rep, verb.name, "a zone list or \"all\" needs expansion into one hide-zone per zone");
    if (!t || !parseTargetId(*t, id)) return refused(rep, verb.name, "needs zone=\"ID\"");
    if (verb.attributes.size() != 1 || !verb.children.empty())
        return refused(rep, verb.name, "takes zone=\"ID\" and nothing else");
    if (!removeZone(prog, id).ok) {
        rep.state = OverlayEditState::Orphaned;
        rep.missingId = id;
    }
    return rep;
}

OverlayEditReport applyHideLayer(Program& prog, const XmlNode& verb, OverlayEditReport rep) {
    const std::string* t = verb.attribute("layer");
    uint32_t id = 0;
    if (!t || !parseTargetId(*t, id)) return refused(rep, verb.name, "needs layer=\"ID\"");
    if (verb.attributes.size() != 1 || !verb.children.empty())
        return refused(rep, verb.name, "takes layer=\"ID\" and nothing else");
    if (!removeLayer(prog, id).ok) {  // the layer, or its zone, is not in this base
        rep.state = OverlayEditState::Orphaned;
        rep.missingId = id;
    }
    return rep;
}

// set-program: the program's own attributes, `.was` beside each, never
// orphaned (there is always a program). `version` and `libraryVersion` are
// not an overlay's to edit (DESIGN §8).
OverlayEditReport applySetProgram(Program& prog, const XmlNode& verb, OverlayEditReport rep) {
    std::vector<StoredParam> params;
    std::vector<OverlayDrift> drift;
    std::string why;
    if (!storedParams<Program>(verb, nullptr, &findProgramParam, params, why)) return refused(rep, verb.name, why);
    for (const StoredParam& p : params)
        if (p.attr == "version" || p.attr == "libraryVersion")
            return refused(rep, verb.name, p.attr + " is not edited by an overlay");
    if (!checkDrift<Program>(prog, params, &findProgramParam, drift, why)) return refused(rep, verb.name, why);
    const EditResult r = setProgramParams(prog, asEdits(params));  // all-or-nothing
    if (!r.ok) return refused(rep, verb.name, r.error);
    if (!drift.empty()) {
        rep.state = OverlayEditState::Drifted;
        rep.drift = std::move(drift);
    }
    return rep;
}

// A pad's key as set-pad writes it: the number, or "none" for a pad with
// no key (92..95 without a row).
std::string padKeyText(int key) { return key < 0 ? std::string(kNoKey) : std::to_string(key); }

// A plain integer as set-pad stores it: never the command language's
// relative "+n", which is resolved when an edit is made and never stored (§8).
bool plainInteger(const std::string* text, long long& v) {
    return text && !text->empty() && (*text)[0] != '+' && parseInteger(*text, v);
}

// Reads set-pad's n and key (each a plain integer in range) and key.was (an
// integer 0..127 or "none"), normalised as padKeyText() writes it. Any other
// attribute, or a child, is refused: the verb has nothing else to say.
bool readSetPad(const XmlNode& verb, int& n, int& key, std::string& was, std::string& error) {
    for (const auto& a : verb.attributes) {
        if (a.first != "n" && a.first != "key" && a.first != std::string("key") + kWasSuffix) {
            error = "unknown attribute \"" + a.first + "\" (set-pad takes n, key and key.was)";
            return false;
        }
    }
    if (!verb.children.empty()) {
        error = "takes no children";
        return false;
    }
    const std::string* nt = verb.attribute("n");
    const std::string* kt = verb.attribute("key");
    const std::string* wt = verb.attribute(std::string("key") + kWasSuffix);
    long long v = 0;
    if (!plainInteger(nt, v) || v < 0 || v >= kNumPads) {
        error = "needs n=\"0.." + std::to_string(kNumPads - 1) + "\"";
        return false;
    }
    n = static_cast<int>(v);
    if (!plainInteger(kt, v) || v < 0 || v > 127) {
        error = "needs key=\"0..127\"";
        return false;
    }
    key = static_cast<int>(v);
    if (!wt) {
        error = "key: no key.was (the value it replaces)";
        return false;
    }
    if (*wt == kNoKey) {
        was = kNoKey;
    } else if (plainInteger(wt, v) && v >= 0 && v <= 127) {
        was = std::to_string(v);
    } else {
        error = "key.was: expected 0..127 or \"none\", got \"" + *wt + "\"";
        return false;
    }
    return true;
}

// set-pad (the author's decision of 2026-11-04, QBX-278): set-program's
// semantics for one row of the <Pads> table. Never orphaned: every pad
// 0..95 has a place in the table, row or not.
OverlayEditReport applySetPad(Program& prog, const XmlNode& verb, OverlayEditReport rep) {
    int n = 0, key = 0;
    std::string was, why;
    if (!readSetPad(verb, n, key, was, why)) return refused(rep, verb.name, why);
    const std::string now = padKeyText(prog.padKey(n));
    const PadEditResult r = setPadKey(prog, n, key);
    if (!r.ok) return refused(rep, verb.name, r.error);
    if (was != now) {
        rep.state = OverlayEditState::Drifted;
        rep.drift.push_back({"key", was, now});
    }
    return rep;
}

// hide-pad (the author's decision of 2026-11-04, QBX-278): hide-zone's
// mechanics for one row of the <Pads> table. The row is left out of what
// plays, so the pad goes back to its default key (a pad 92..95 has none and
// ceases to exist); the others keep their order. A program without the row
// at that point orphans the hide, missingId = n.
OverlayEditReport applyHidePad(Program& prog, const XmlNode& verb, OverlayEditReport rep) {
    long long v = 0;
    if (verb.attributes.size() != 1 || !verb.children.empty() || !verb.attribute("n"))
        return refused(rep, verb.name, "takes n=\"0.." + std::to_string(kNumPads - 1) + "\" and nothing else");
    if (!plainInteger(verb.attribute("n"), v) || v < 0 || v >= kNumPads)
        return refused(rep, verb.name, "needs n=\"0.." + std::to_string(kNumPads - 1) + "\"");
    const int n = static_cast<int>(v);
    auto it = std::find_if(prog.pads.begin(), prog.pads.end(), [n](const PadRow& r) { return r.n == n; });
    if (it == prog.pads.end()) {
        rep.state = OverlayEditState::Orphaned;
        rep.missingId = static_cast<uint32_t>(n);
        return rep;
    }
    prog.pads.erase(it);
    return rep;
}

OverlayEditReport applyOne(Program& prog, const XmlNode& verb, size_t index) {
    OverlayEditReport rep;
    rep.index = index;
    if (verb.name == kSetProgram) return applySetProgram(prog, verb, rep);
    if (verb.name == kSetPad) return applySetPad(prog, verb, rep);
    if (verb.name == kHidePad) return applyHidePad(prog, verb, rep);
    if (verb.name == kAddZone) return applyAddZone(prog, verb, rep);
    if (verb.name == kAddLayer) return applyAddLayer(prog, verb, rep);
    if (verb.name == kHideZone) return applyHideZone(prog, verb, rep);
    if (verb.name == kHideLayer) return applyHideLayer(prog, verb, rep);
    const bool zone = verb.name == kSetZone;
    if (!zone && verb.name != kSetLayer) {
        rep.state = OverlayEditState::Unknown;
        rep.error = "unknown verb <" + verb.name + ">";
        return rep;
    }
    const char* addr = zone ? "zone" : "layer";
    const std::string* target = verb.attribute(addr);
    uint32_t id = 0;
    if (zone && target && needsExpansion(*target)) {
        // ALL and zone lists are expanded into one set-zone per zone when the
        // edit is made (DESIGN §8). Stored, ALL would also reach zones a
        // later base adds with no value to compare against, and a list would
        // share one `.was` among its zones: refused, not guessed.
        rep.state = OverlayEditState::Refused;
        rep.error = "<set-zone zone=\"" + *target + "\"> needs expansion into one set-zone per zone";
        return rep;
    }
    if (!target || !parseTargetId(*target, id)) {
        rep.state = OverlayEditState::Refused;
        rep.error = std::string("<") + verb.name + "> needs " + addr + "=\"ID\"";
        return rep;
    }
    Zone* z = zone ? prog.findZone(id) : nullptr;
    Layer* l = zone ? nullptr : prog.findLayer(id).second;
    if (zone ? !z : !l) {
        rep.state = OverlayEditState::Orphaned;
        rep.missingId = id;
        return rep;
    }
    std::vector<StoredParam> params;
    std::vector<OverlayDrift> drift;
    bool ok = zone ? storedParams<Zone>(verb, addr, &findZoneParam, params, rep.error)
                   : storedParams<Layer>(verb, addr, &findLayerParam, params, rep.error);
    if (ok)
        ok = zone ? checkDrift<Zone>(*z, params, &findZoneParam, drift, rep.error)
                  : checkDrift<Layer>(*l, params, &findLayerParam, drift, rep.error);
    if (ok) {
        const EditResult r = zone ? setZones(prog, ZoneScope::zones({id}), asEdits(params))
                                  : setLayers(prog, {id}, asEdits(params));  // all-or-nothing
        ok = r.ok;
        if (!ok) rep.error = r.error;
    }
    if (!ok) {
        rep.state = OverlayEditState::Refused;
        rep.error = std::string("<") + verb.name + ">: " + rep.error;
        return rep;
    }
    // A drifted edit still applies: the overlay's value plays (DESIGN §8).
    if (!drift.empty()) {
        rep.state = OverlayEditState::Drifted;
        rep.drift = std::move(drift);
    }
    return rep;
}

// The verb of one recorded target: the address, then each parameter followed
// by its `.was`, in the order the edit named them.
XmlNode makeVerb(const char* name, const char* addr, uint32_t id, const std::vector<std::string>& attrs,
                 const std::vector<std::string>& values, const std::vector<std::string>& was) {
    XmlNode v;
    v.name = name;
    if (addr) v.attributes.emplace_back(addr, std::to_string(id));
    for (size_t i = 0; i < attrs.size(); ++i) {
        v.attributes.emplace_back(attrs[i], values[i]);
        v.attributes.emplace_back(attrs[i] + kWasSuffix, was[i]);
    }
    return v;
}

// Records the overlay's baseVersion with its first edit: the libraryVersion
// of the base it is made on (`current` has the base's, since no overlay edit
// can change it). An overlay that already has edits is not re-stamped: its
// earlier edits were made on whatever it says, or on an unrecorded version.
void stampBaseVersion(Overlay& overlay, const Program& current, RecordedEdit& rec) {
    if (overlay.edits.empty() && overlay.baseVersion.empty() && !current.libraryVersion.empty()) {
        overlay.baseVersion = current.libraryVersion;
        rec.stampedBaseVersion = true;
    }
}

// Appends a recorded edit's verbs to the overlay.
void append(Overlay& overlay, const Program& current, RecordedEdit& rec, std::vector<XmlNode> verbs) {
    stampBaseVersion(overlay, current, rec);
    rec.ok = true;
    rec.first = overlay.edits.size();
    rec.count = verbs.size();
    for (const XmlNode& v : verbs) overlay.edits.push_back(v);
    rec.verbs = std::move(verbs);
}

template <typename T>
RecordedEdit record(Overlay& overlay, Program& current, const std::vector<ParamEdit>& edits, RecordedKind kind,
                    const char* verbName,
                    const char* addr, const std::vector<ParamDesc<T>>& table, T* (*find)(Program&, uint32_t),
                    EditResult (*apply)(Program&, const std::vector<uint32_t>&, const std::vector<ParamEdit>&),
                    const std::vector<uint32_t>& ids) {
    RecordedEdit rec;
    std::vector<std::string> attrs;
    std::vector<const ParamDesc<T>*> descs;
    for (const ParamEdit& e : edits) {
        if (std::find(attrs.begin(), attrs.end(), e.attr) != attrs.end()) {
            rec.error = "\"" + e.attr + "\" is named twice";
            return rec;
        }
        attrs.push_back(e.attr);
        const ParamDesc<T>* p = nullptr;
        for (const auto& d : table)
            if (e.attr == d.name) p = &d;
        descs.push_back(p);  // an unknown name is refused by apply() below
    }
    // Made on a copy, so a refusal leaves `current` and the overlay alone.
    Program next = current;
    const EditResult r = apply(next, ids, edits);
    if (!r.ok) {
        rec.error = r.error;
        return rec;
    }
    // r.prior holds, per target (in `ids` order, repeats dropped) and per
    // edit, the previous absolute value as written: the `.was`. The new value
    // is the target's value after the edit, as written, so a relative edit is
    // resolved here, clamped per target, and only its result is stored.
    const size_t perTarget = edits.size();
    const size_t targets = perTarget ? r.prior.size() / perTarget : 0;
    std::vector<XmlNode> verbs;
    for (size_t t = 0; t < targets; ++t) {
        const uint32_t id = r.prior[t * perTarget].id;
        const T* obj = find(next, id);
        std::vector<std::string> values, was;
        for (size_t i = 0; i < perTarget; ++i) {
            values.push_back(formatParam(*descs[i], *obj));
            was.push_back(r.prior[t * perTarget + i].value);
        }
        verbs.push_back(makeVerb(verbName, addr, id, attrs, values, was));
    }
    rec.kind = kind;
    append(overlay, current, rec, std::move(verbs));
    rec.prior = r.prior;
    current = std::move(next);
    return rec;
}

Zone* findZoneIn(Program& p, uint32_t id) { return p.findZone(id); }
Layer* findLayerIn(Program& p, uint32_t id) { return p.findLayer(id).second; }
EditResult setZonesById(Program& p, const std::vector<uint32_t>& ids, const std::vector<ParamEdit>& e) {
    return setZones(p, ZoneScope::zones(ids), e);
}
Program* findProgramIn(Program& p, uint32_t) { return &p; }
EditResult setProgramById(Program& p, const std::vector<uint32_t>&, const std::vector<ParamEdit>& e) {
    return setProgramParams(p, e);
}

// The next id the overlay hands out: past its own nextId, past every id its
// add verbs name (an overlay written by hand may lack nextId), and past every
// overlay-range id the program already has (a flattened program used as a
// base). Never below kOverlayIdFirst. 0 when the range is used up.
uint32_t nextOverlayId(const Overlay& overlay, const Program& current) {
    uint64_t n = std::max<uint64_t>(kOverlayIdFirst, overlay.nextId);
    for (const XmlNode& v : overlay.edits) {
        const char* attr = v.name == kAddZone ? "zone" : v.name == kAddLayer ? "layer" : nullptr;
        uint32_t id = 0;
        if (attr && v.attribute(attr) && parseTargetId(*v.attribute(attr), id)) n = std::max<uint64_t>(n, id + 1ull);
    }
    for (const Zone& z : current.zones) {
        n = std::max<uint64_t>(n, z.id + 1ull);
        for (const Layer& l : z.layers) n = std::max<uint64_t>(n, l.id + 1ull);
    }
    return n > 0xFFFFFFFFull ? 0 : static_cast<uint32_t>(n);
}

// The verb that adds `obj` under `id`: the addressing attributes, then every
// attribute of the table as a program file writes it, then the object's
// unknown attributes and children (R4).
template <typename T>
XmlNode addVerb(const char* name, const char* addr, uint32_t id, const char* parentAttr, uint32_t parent,
                const std::vector<ParamDesc<T>>& table, const T& obj, const Extras& extras) {
    XmlNode v;
    v.name = name;
    v.attributes.emplace_back(addr, std::to_string(id));
    if (parentAttr) v.attributes.emplace_back(parentAttr, std::to_string(parent));
    for (const auto& p : table) v.attributes.emplace_back(p.name, formatParam(p, obj));
    for (const auto& a : extras.attributes) v.attributes.push_back(a);
    v.children = extras.children;
    return v;
}

// Applies freshly made structural verbs to a copy of `current` through the
// same code applyOverlay() runs, so what is recorded is what plays. All of
// them must apply; else nothing is recorded.
bool applyStructural(const Program& current, const std::vector<XmlNode>& verbs, Program& next, std::string& error) {
    next = current;
    for (size_t i = 0; i < verbs.size(); ++i) {
        const OverlayEditReport rep = applyOne(next, verbs[i], i);
        if (rep.state != OverlayEditState::Applied) {
            error = rep.error.empty() ? "<" + verbs[i].name + "> did not apply" : rep.error;
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<size_t> OverlayResult::orphaned() const {
    std::vector<size_t> out;
    for (const auto& e : edits)
        if (e.state == OverlayEditState::Orphaned) out.push_back(e.index);
    return out;
}

std::vector<size_t> OverlayResult::drifted() const {
    std::vector<size_t> out;
    for (const auto& e : edits)
        if (e.state == OverlayEditState::Drifted) out.push_back(e.index);
    return out;
}

OverlayParseResult overlayFromXml(const XmlNode& root) {
    OverlayParseResult r;
    if (root.name != kOverlayElement) {
        r.error = "expected <" + std::string(kOverlayElement) + ">, got <" + root.name + ">";
        return r;
    }
    Overlay& o = r.overlay;
    for (const auto& a : root.attributes) {
        if (a.first == "version") {
            long long v = 0;
            if (!parseInteger(a.second, v) || v < 1 || v > 0x7FFFFFFFll) {
                r.error = "version: expected a positive integer, got \"" + a.second + "\"";
                return r;
            }
            o.version = static_cast<int>(v);
        } else if (a.first == "base") {
            o.base = a.second;
        } else if (a.first == "baseVersion") {
            o.baseVersion = a.second;
        } else if (a.first == "nextId") {
            long long v = 0;
            if (!parseInteger(a.second, v) || v < kOverlayIdFirst || v > 0xFFFFFFFFll) {
                r.error = "nextId: expected an id from " + std::to_string(kOverlayIdFirst) + " up, got \"" + a.second +
                          "\"";
                return r;
            }
            o.nextId = static_cast<uint32_t>(v);
        } else {
            o.attributes.push_back(a);
        }
    }
    o.edits = root.children;
    r.ok = true;
    return r;
}

OverlayParseResult parseOverlay(const std::string& xml) {
    const XmlParseResult x = parseXml(xml);
    if (!x.ok) {
        OverlayParseResult r;
        r.error = "XML: " + x.error;
        return r;
    }
    return overlayFromXml(x.root);
}

XmlNode overlayToXml(const Overlay& o) {
    XmlNode root;
    root.name = kOverlayElement;
    root.attributes.emplace_back("version", std::to_string(o.version));
    if (!o.base.empty()) root.attributes.emplace_back("base", o.base);
    if (!o.baseVersion.empty()) root.attributes.emplace_back("baseVersion", o.baseVersion);
    if (o.nextId != 0) root.attributes.emplace_back("nextId", std::to_string(o.nextId));
    for (const auto& a : o.attributes) root.attributes.push_back(a);
    root.children = o.edits;
    return root;
}

std::string writeOverlay(const Overlay& o) { return writeXml(overlayToXml(o)); }

OverlayResult applyOverlay(const Program& base, const Overlay& overlay) {
    OverlayResult r;
    r.program = base;  // the base itself is never written (DESIGN §8)
    // Another library version still applies; it is reported (DESIGN §8).
    if (!overlay.baseVersion.empty() && overlay.baseVersion != base.libraryVersion)
        r.baseDrift.push_back({"libraryVersion", overlay.baseVersion, base.libraryVersion});
    for (size_t i = 0; i < overlay.edits.size(); ++i) r.edits.push_back(applyOne(r.program, overlay.edits[i], i));
    return r;
}

OverlayResult flatten(const Program& base, const Overlay& overlay) {
    // The program model has no reference to a base: the applied program is
    // already one of your own, except that it no longer is that library's
    // version.
    OverlayResult r = applyOverlay(base, overlay);
    r.program.libraryVersion.clear();
    return r;
}

RecordedEdit recordZoneEdit(Overlay& overlay, Program& current, const ZoneScope& scope,
                            const std::vector<ParamEdit>& edits) {
    std::vector<uint32_t> ids;
    if (scope.all) {
        // ALL is expanded over the zones that exist now (DESIGN §8).
        for (const Zone& z : current.zones) ids.push_back(z.id);
    } else {
        ids = scope.ids;
    }
    return record<Zone>(overlay, current, edits, RecordedKind::SetZone, kSetZone, "zone", zoneParams(), &findZoneIn, &setZonesById, ids);
}

RecordedEdit recordLayerEdit(Overlay& overlay, Program& current, const std::vector<uint32_t>& layerIds,
                             const std::vector<ParamEdit>& edits) {
    return record<Layer>(overlay, current, edits, RecordedKind::SetLayer, kSetLayer, "layer", layerParams(), &findLayerIn, &setLayers,
                         layerIds);
}

RecordedEdit recordProgramEdit(Overlay& overlay, Program& current, const std::vector<ParamEdit>& edits) {
    for (const ParamEdit& e : edits) {
        if (e.attr == "version" || e.attr == "libraryVersion") {
            RecordedEdit rec;
            rec.error = e.attr + " is not edited by an overlay";
            return rec;
        }
    }
    return record<Program>(overlay, current, edits, RecordedKind::SetProgram, kSetProgram, nullptr, programParams(),
                           &findProgramIn, &setProgramById, {0});
}

RecordedEdit recordPadEdit(Overlay& overlay, Program& current, int n, int key) {
    RecordedEdit rec;
    rec.kind = RecordedKind::SetPad;
    if (n < 0 || n >= kNumPads) {
        rec.error = "pad " + std::to_string(n) + " is not a pad (0.." + std::to_string(kNumPads - 1) + ")";
        return rec;
    }
    if (key < 0 || key > 127) {
        rec.error = "key " + std::to_string(key) + " is not a MIDI key (0..127)";
        return rec;
    }
    XmlNode v;
    v.name = kSetPad;
    v.attributes.emplace_back("n", std::to_string(n));
    v.attributes.emplace_back("key", std::to_string(key));
    v.attributes.emplace_back(std::string("key") + kWasSuffix, padKeyText(current.padKey(n)));
    Program next;
    if (!applyStructural(current, {v}, next, rec.error)) return rec;
    rec.priorPads = current.pads;
    append(overlay, current, rec, {v});
    current = std::move(next);
    return rec;
}

RecordedEdit recordHidePad(Overlay& overlay, Program& current, const Program& base, int n) {
    RecordedEdit rec;
    rec.kind = RecordedKind::HidePad;
    if (n < 0 || n >= kNumPads) {
        rec.error = "pad " + std::to_string(n) + " is not a pad (0.." + std::to_string(kNumPads - 1) + ")";
        return rec;
    }
    const auto hasRow = [n](const Program& p) {
        return std::any_of(p.pads.begin(), p.pads.end(), [n](const PadRow& r) { return r.n == n; });
    };
    if (!hasRow(base)) {
        rec.error = "pad " + std::to_string(n) + " has no row in the base (nothing to hide)";
        return rec;
    }
    if (!hasRow(current)) {
        rec.error = "pad " + std::to_string(n) + "'s base row does not play now (already hidden, or the overlay "
                    "set the pad to its default key)";
        return rec;
    }
    XmlNode v;
    v.name = kHidePad;
    v.attributes.emplace_back("n", std::to_string(n));
    Program next;
    if (!applyStructural(current, {v}, next, rec.error)) return rec;
    rec.priorPads = current.pads;
    rec.id = static_cast<uint32_t>(n);
    append(overlay, current, rec, {v});
    current = std::move(next);
    return rec;
}

RecordedEdit recordAddZone(Overlay& overlay, Program& current, Zone zone) {
    RecordedEdit rec;
    rec.kind = RecordedKind::AddZone;
    if (static_cast<int>(zone.layers.size()) > kMaxLayers) {
        rec.error = "a zone has at most " + std::to_string(kMaxLayers) + " layers";
        return rec;
    }
    const uint32_t first = nextOverlayId(overlay, current);
    if (first == 0 || 0xFFFFFFFFull - first < zone.layers.size() + 1) {
        rec.error = "the overlay range is used up";
        return rec;
    }
    std::vector<XmlNode> verbs;
    verbs.push_back(addVerb<Zone>(kAddZone, "zone", first, nullptr, 0, zoneParams(), zone, zone.extras));
    for (size_t i = 0; i < zone.layers.size(); ++i)
        verbs.push_back(addVerb<Layer>(kAddLayer, "layer", first + 1 + static_cast<uint32_t>(i), "zone", first,
                                       layerParams(), zone.layers[i], zone.layers[i].extras));
    Program next;
    if (!applyStructural(current, verbs, next, rec.error)) return rec;
    rec.id = first;
    append(overlay, current, rec, std::move(verbs));
    overlay.nextId = first + 1 + static_cast<uint32_t>(zone.layers.size());
    current = std::move(next);
    return rec;
}

RecordedEdit recordAddLayer(Overlay& overlay, Program& current, uint32_t zoneId, Layer layer) {
    RecordedEdit rec;
    rec.kind = RecordedKind::AddLayer;
    const uint32_t id = nextOverlayId(overlay, current);
    if (id == 0 || id == 0xFFFFFFFFu) {
        rec.error = "the overlay range is used up";
        return rec;
    }
    std::vector<XmlNode> verbs{addVerb<Layer>(kAddLayer, "layer", id, "zone", zoneId, layerParams(), layer, layer.extras)};
    Program next;
    if (!current.findZone(zoneId)) {
        rec.error = "no zone with id " + std::to_string(zoneId);
        return rec;
    }
    if (!applyStructural(current, verbs, next, rec.error)) return rec;
    rec.id = id;
    append(overlay, current, rec, std::move(verbs));
    overlay.nextId = id + 1;
    current = std::move(next);
    return rec;
}

RecordedEdit recordHideZone(Overlay& overlay, Program& current, uint32_t zoneId) {
    RecordedEdit rec;
    rec.kind = RecordedKind::HideZone;
    Program next = current;
    rec.hidden = removeZone(next, zoneId);  // for the undo: the zone and where it was
    if (!rec.hidden.ok) {
        rec.error = "no zone with id " + std::to_string(zoneId);
        return rec;
    }
    XmlNode v;
    v.name = kHideZone;
    v.attributes.emplace_back("zone", std::to_string(zoneId));
    if (!applyStructural(current, {v}, next, rec.error)) return rec;
    rec.id = zoneId;
    append(overlay, current, rec, {v});
    current = std::move(next);
    return rec;
}

RecordedEdit recordHideLayer(Overlay& overlay, Program& current, uint32_t layerId) {
    RecordedEdit rec;
    rec.kind = RecordedKind::HideLayer;
    Program next = current;
    rec.hiddenLayer = removeLayer(next, layerId);  // for the undo: the layer and where it was
    if (!rec.hiddenLayer.ok) {
        rec.error = "no layer with id " + std::to_string(layerId);
        return rec;
    }
    XmlNode v;
    v.name = kHideLayer;
    v.attributes.emplace_back("layer", std::to_string(layerId));
    if (!applyStructural(current, {v}, next, rec.error)) return rec;
    rec.id = layerId;
    append(overlay, current, rec, {v});
    current = std::move(next);
    return rec;
}

bool undoRecordedEdit(Overlay& overlay, Program& current, const RecordedEdit& edit) {
    if (!edit.ok || overlay.edits.size() != edit.first + edit.count) return false;
    for (size_t i = 0; i < edit.count; ++i)
        if (overlay.edits[edit.first + i] != edit.verbs[i]) return false;
    // All or nothing: on a copy.
    Program prev = current;
    bool ok = false;
    switch (edit.kind) {
        case RecordedKind::SetZone: ok = restoreZones(prev, edit.prior).ok; break;   // the `.was` values
        case RecordedKind::SetLayer: ok = restoreLayers(prev, edit.prior).ok; break;
        case RecordedKind::SetProgram: ok = restoreProgramParams(prev, edit.prior).ok; break;
        case RecordedKind::AddZone: ok = removeZone(prev, edit.id).ok; break;     // with its layers
        case RecordedKind::AddLayer: ok = removeLayer(prev, edit.id).ok; break;
        case RecordedKind::HideZone: ok = reinsertZone(prev, edit.hidden); break;
        case RecordedKind::HideLayer: ok = reinsertLayer(prev, edit.hiddenLayer); break;
        case RecordedKind::SetPad: prev.pads = edit.priorPads; ok = true; break;  // the table as it was
        case RecordedKind::HidePad: prev.pads = edit.priorPads; ok = true; break;  // the row back where it was
    }
    if (!ok) return false;
    overlay.edits.resize(edit.first);
    if (edit.stampedBaseVersion) overlay.baseVersion.clear();
    current = std::move(prev);
    return true;
}

namespace {

// Rewrites every `.was` of a set verb to the target's value in `prog` (the
// program just before the verb applies), as formatParam writes it, in place:
// the attribute order and the new values are kept.
template <typename T>
void refreshWas(XmlNode& verb, const T& target, const ParamDesc<T>* (*find)(const std::string&)) {
    for (auto& a : verb.attributes) {
        if (!endsWithWas(a.first)) continue;
        const ParamDesc<T>* p = find(a.first.substr(0, a.first.size() - kWasLength));
        if (p) a.second = formatParam(*p, target);
    }
}

void refreshVerb(XmlNode& verb, const Program& prog) {
    uint32_t id = 0;
    if (verb.name == kSetProgram) {
        refreshWas<Program>(verb, prog, &findProgramParam);
    } else if (verb.name == kSetZone && verb.attribute("zone") && parseTargetId(*verb.attribute("zone"), id)) {
        if (const Zone* z = prog.findZone(id)) refreshWas<Zone>(verb, *z, &findZoneParam);
    } else if (verb.name == kSetLayer && verb.attribute("layer") && parseTargetId(*verb.attribute("layer"), id)) {
        if (const Layer* l = prog.findLayer(id).second) refreshWas<Layer>(verb, *l, &findLayerParam);
    } else if (verb.name == kSetPad) {
        // Only a set-pad that applied (drifted) is refreshed, so n is valid.
        long long n = 0;
        const std::string wasName = std::string("key") + kWasSuffix;
        if (verb.attribute("n") && parseInteger(*verb.attribute("n"), n))
            for (auto& a : verb.attributes)
                if (a.first == wasName) a.second = padKeyText(prog.padKey(static_cast<int>(n)));
    }
}

}  // namespace

RebasedOverlay rebaseOverlay(Overlay& overlay, Program& current, const Program& newBase) {
    RebasedOverlay r;
    r.before = overlay;
    r.currentBefore = current;
    r.fromVersion = overlay.baseVersion;
    Overlay next = overlay;
    // Walk the edits in order over the new base, as applyOverlay() does: a
    // drifted edit's `.was` becomes the value under it at that point, which
    // includes the earlier edits (two edits of one parameter chain).
    Program work = newBase;
    for (size_t i = 0; i < next.edits.size(); ++i) {
        Program scratch = work;
        const OverlayEditReport rep = applyOne(scratch, next.edits[i], i);
        if (rep.state == OverlayEditState::Drifted) {
            refreshVerb(next.edits[i], work);
            r.refreshed.push_back(i);
            scratch = work;
            applyOne(scratch, next.edits[i], i);
        } else if (rep.state == OverlayEditState::Orphaned) {
            r.orphaned.push_back(i);
        }
        if (rep.state == OverlayEditState::Applied || rep.state == OverlayEditState::Drifted) work = std::move(scratch);
    }
    next.baseVersion = newBase.libraryVersion;
    r.toVersion = next.baseVersion;
    overlay = next;
    current = std::move(work);
    r.after = overlay;
    r.ok = true;
    return r;
}

bool undoRebase(Overlay& overlay, Program& current, const RebasedOverlay& rebased) {
    if (!rebased.ok || overlay != rebased.after) return false;
    overlay = rebased.before;
    current = rebased.currentBefore;
    return true;
}

}  // namespace nd
