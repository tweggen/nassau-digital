#include "nd_overlay.h"

#include <algorithm>

#include "nd_edit.h"

namespace nd {

namespace {

constexpr const char* kSetZone = "set-zone";
constexpr const char* kSetLayer = "set-layer";
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
        if (a.first == addressAttr || endsWithWas(a.first)) continue;
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

OverlayEditReport applyOne(Program& prog, const XmlNode& verb, size_t index) {
    OverlayEditReport rep;
    rep.index = index;
    const bool zone = verb.name == kSetZone;
    if (!zone && verb.name != kSetLayer) {
        rep.state = OverlayEditState::Unknown;
        rep.error = "unknown verb <" + verb.name + ">";
        return rep;
    }
    const char* addr = zone ? "zone" : "layer";
    const std::string* target = verb.attribute(addr);
    uint32_t id = 0;
    if (zone && target && *target == "all") {
        // ALL is expanded into one set-zone per zone when the edit is made
        // (DESIGN §8). A stored zone="all" would also reach zones a later
        // base adds, with no value to compare against: refused, not guessed.
        rep.state = OverlayEditState::Refused;
        rep.error = "<set-zone zone=\"all\"> needs expansion into one set-zone per zone";
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
    v.attributes.emplace_back(addr, std::to_string(id));
    for (size_t i = 0; i < attrs.size(); ++i) {
        v.attributes.emplace_back(attrs[i], values[i]);
        v.attributes.emplace_back(attrs[i] + kWasSuffix, was[i]);
    }
    return v;
}

template <typename T>
RecordedEdit record(Overlay& overlay, Program& current, const std::vector<ParamEdit>& edits, const char* verbName,
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
    rec.ok = true;
    rec.first = overlay.edits.size();
    rec.count = verbs.size();
    for (const XmlNode& v : verbs) overlay.edits.push_back(v);
    rec.verbs = std::move(verbs);
    rec.prior = r.prior;
    rec.layers = std::string(verbName) == kSetLayer;
    current = std::move(next);
    return rec;
}

Zone* findZoneIn(Program& p, uint32_t id) { return p.findZone(id); }
Layer* findLayerIn(Program& p, uint32_t id) { return p.findLayer(id).second; }
EditResult setZonesById(Program& p, const std::vector<uint32_t>& ids, const std::vector<ParamEdit>& e) {
    return setZones(p, ZoneScope::zones(ids), e);
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
    for (const auto& a : o.attributes) root.attributes.push_back(a);
    root.children = o.edits;
    return root;
}

std::string writeOverlay(const Overlay& o) { return writeXml(overlayToXml(o)); }

OverlayResult applyOverlay(const Program& base, const Overlay& overlay) {
    OverlayResult r;
    r.program = base;  // the base itself is never written (DESIGN §8)
    for (size_t i = 0; i < overlay.edits.size(); ++i) r.edits.push_back(applyOne(r.program, overlay.edits[i], i));
    return r;
}

OverlayResult flatten(const Program& base, const Overlay& overlay) {
    // The program model has no reference to a base: the applied program is
    // already one of your own.
    return applyOverlay(base, overlay);
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
    return record<Zone>(overlay, current, edits, kSetZone, "zone", zoneParams(), &findZoneIn, &setZonesById, ids);
}

RecordedEdit recordLayerEdit(Overlay& overlay, Program& current, const std::vector<uint32_t>& layerIds,
                             const std::vector<ParamEdit>& edits) {
    return record<Layer>(overlay, current, edits, kSetLayer, "layer", layerParams(), &findLayerIn, &setLayers,
                         layerIds);
}

bool undoRecordedEdit(Overlay& overlay, Program& current, const RecordedEdit& edit) {
    if (!edit.ok || overlay.edits.size() != edit.first + edit.count) return false;
    for (size_t i = 0; i < edit.count; ++i)
        if (overlay.edits[edit.first + i] != edit.verbs[i]) return false;
    // The `.was` values written back, all or nothing.
    Program prev = current;
    const EditResult r = edit.layers ? restoreLayers(prev, edit.prior) : restoreZones(prev, edit.prior);
    if (!r.ok) return false;
    overlay.edits.resize(edit.first);
    current = std::move(prev);
    return true;
}

}  // namespace nd
