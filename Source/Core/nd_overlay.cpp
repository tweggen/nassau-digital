#include "nd_overlay.h"

#include "nd_edit.h"

namespace nd {

namespace {

constexpr const char* kSetZone = "set-zone";
constexpr const char* kSetLayer = "set-layer";

bool parseTargetId(const std::string& text, uint32_t& id) {
    long long v = 0;
    if (!parseInteger(text, v) || v < 1 || v > 0xFFFFFFFFll) return false;
    id = static_cast<uint32_t>(v);
    return true;
}

// The parameter edits of a verb: every attribute but the addressing one, as
// an absolute value. A numeric value written "+n" is the command language's
// relative form (DESIGN §9.3); parsed as a plain number it would silently
// become the absolute n, so it is refused until its meaning in an overlay is
// decided.
template <typename T>
bool paramEdits(const XmlNode& verb, const char* addressAttr, const ParamDesc<T>* (*find)(const std::string&),
                std::vector<ParamEdit>& out, std::string& error) {
    for (const auto& a : verb.attributes) {
        if (a.first == addressAttr) continue;
        const ParamDesc<T>* p = find(a.first);
        if (p && isNumeric(*p) && !a.second.empty() && a.second[0] == '+') {
            error = "<" + verb.name + "> " + a.first + "=\"" + a.second +
                    "\": a relative value in an overlay is not supported";
            return false;
        }
        out.push_back(ParamEdit::absolute(a.first, a.second));
    }
    return true;
}

OverlayEditReport applyOne(Program& prog, const XmlNode& verb, size_t index) {
    OverlayEditReport rep;
    rep.index = index;
    if (verb.name == kSetZone) {
        const std::string* target = verb.attribute("zone");
        ZoneScope scope;
        uint32_t id = 0;
        if (target && *target == "all") {
            scope = ZoneScope::everyZone();
        } else if (target && parseTargetId(*target, id)) {
            if (!prog.findZone(id)) {
                rep.state = OverlayEditState::Orphaned;
                rep.missingId = id;
                return rep;
            }
            scope = ZoneScope::zones({id});
        } else {
            rep.state = OverlayEditState::Refused;
            rep.error = "<set-zone> needs zone=\"ID\" or zone=\"all\"";
            return rep;
        }
        std::vector<ParamEdit> edits;
        if (!paramEdits<Zone>(verb, "zone", &findZoneParam, edits, rep.error)) {
            rep.state = OverlayEditState::Refused;
            return rep;
        }
        const EditResult r = setZones(prog, scope, edits);  // all-or-nothing
        if (!r.ok) {
            rep.state = OverlayEditState::Refused;
            rep.error = "<set-zone>: " + r.error;
        }
        return rep;
    }
    if (verb.name == kSetLayer) {
        const std::string* target = verb.attribute("layer");
        uint32_t id = 0;
        if (!target || !parseTargetId(*target, id)) {
            rep.state = OverlayEditState::Refused;
            rep.error = "<set-layer> needs layer=\"ID\"";
            return rep;
        }
        if (!prog.findLayer(id).second) {
            rep.state = OverlayEditState::Orphaned;
            rep.missingId = id;
            return rep;
        }
        std::vector<ParamEdit> edits;
        if (!paramEdits<Layer>(verb, "layer", &findLayerParam, edits, rep.error)) {
            rep.state = OverlayEditState::Refused;
            return rep;
        }
        const EditResult r = setLayers(prog, {id}, edits);
        if (!r.ok) {
            rep.state = OverlayEditState::Refused;
            rep.error = "<set-layer>: " + r.error;
        }
        return rep;
    }
    rep.state = OverlayEditState::Unknown;
    rep.error = "unknown verb <" + verb.name + ">";
    return rep;
}

}  // namespace

std::vector<size_t> OverlayResult::orphaned() const {
    std::vector<size_t> out;
    for (const auto& e : edits)
        if (e.state == OverlayEditState::Orphaned) out.push_back(e.index);
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

}  // namespace nd
