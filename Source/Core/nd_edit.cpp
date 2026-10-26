#include "nd_edit.h"

#include <algorithm>
#include <cmath>

namespace nd {

namespace {

// Validates every edit against the table, then applies them to every target.
// `targets` are pointers into the program; `ids` their ids, same order.
template <typename T>
EditResult applySet(const std::vector<ParamDesc<T>>& table, const std::vector<T*>& targets,
                    const std::vector<uint32_t>& ids, const std::vector<ParamEdit>& edits) {
    EditResult r;
    std::vector<const ParamDesc<T>*> descs;
    for (const ParamEdit& e : edits) {
        const ParamDesc<T>* p = nullptr;
        for (const auto& d : table)
            if (e.attr == d.name) p = &d;
        if (!p) {
            r.error = "unknown parameter \"" + e.attr + "\"";
            return r;
        }
        if (e.relative && !isNumeric(*p)) {
            // DESIGN §2.1: relative edits apply to numeric parameters only.
            r.error = "a relative edit of \"" + e.attr + "\" is refused: it is not numeric";
            return r;
        }
        if (e.relative && !std::isfinite(e.delta)) {
            r.error = "a relative edit of \"" + e.attr + "\" needs a finite delta";
            return r;
        }
        if (!e.relative) {
            // Validate the absolute value once, on a scratch copy.
            T scratch = targets.empty() ? T{} : *targets.front();
            std::string why;
            if (!parseParam(*p, scratch, e.value, why)) {
                r.error = why;
                return r;
            }
        }
        descs.push_back(p);
    }

    // Record what is about to be replaced, then apply. Applying cannot fail
    // from here on, so the edit is all-or-nothing.
    for (size_t t = 0; t < targets.size(); ++t)
        for (size_t i = 0; i < edits.size(); ++i)
            r.prior.push_back({ids[t], descs[i]->name, formatParam(*descs[i], *targets[t])});

    for (size_t t = 0; t < targets.size(); ++t) {
        T& obj = *targets[t];
        for (size_t i = 0; i < edits.size(); ++i) {
            const ParamDesc<T>& p = *descs[i];
            const ParamEdit& e = edits[i];
            if (e.relative) {
                double v = p.get(obj) + e.delta;
                if (p.kind == ParamKind::Int) v = std::round(v);
                v = std::min(std::max(v, p.minValue), p.maxValue);  // clamps per target
                p.set(obj, v);
            } else {
                std::string why;
                parseParam(p, obj, e.value, why);
            }
        }
    }
    r.ok = true;
    return r;
}

template <typename T>
EditResult applyRestore(const std::vector<ParamDesc<T>>& table, const std::vector<T*>& targets,
                        const std::vector<PriorValue>& prior) {
    EditResult r;
    // Validate everything first, so a restore is all-or-nothing too.
    std::vector<std::pair<T*, const ParamDesc<T>*>> plan;
    for (size_t i = 0; i < prior.size(); ++i) {
        T* obj = targets[i];
        if (!obj) {
            r.error = "id " + std::to_string(prior[i].id) + " no longer exists";
            return r;
        }
        const ParamDesc<T>* p = nullptr;
        for (const auto& d : table)
            if (prior[i].attr == d.name) p = &d;
        if (!p) {
            r.error = "unknown parameter \"" + prior[i].attr + "\"";
            return r;
        }
        T scratch = *obj;
        std::string why;
        if (!parseParam(*p, scratch, prior[i].value, why)) {
            r.error = why;
            return r;
        }
        plan.emplace_back(obj, p);
    }
    for (size_t i = 0; i < prior.size(); ++i) {
        r.prior.push_back({prior[i].id, prior[i].attr, formatParam(*plan[i].second, *plan[i].first)});
        std::string why;
        parseParam(*plan[i].second, *plan[i].first, prior[i].value, why);
    }
    r.ok = true;
    return r;
}

}  // namespace

EditResult setZones(Program& prog, const ZoneScope& scope, const std::vector<ParamEdit>& edits) {
    std::vector<Zone*> targets;
    std::vector<uint32_t> ids;
    if (scope.all) {
        for (Zone& z : prog.zones) {
            targets.push_back(&z);
            ids.push_back(z.id);
        }
    } else {
        for (uint32_t id : scope.ids) {
            Zone* z = prog.findZone(id);
            if (!z) {
                EditResult r;
                r.error = "no zone with id " + std::to_string(id);
                return r;
            }
            if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
            targets.push_back(z);
            ids.push_back(id);
        }
    }
    return applySet(zoneParams(), targets, ids, edits);
}

EditResult setLayers(Program& prog, const std::vector<uint32_t>& layerIds, const std::vector<ParamEdit>& edits) {
    std::vector<Layer*> targets;
    std::vector<uint32_t> ids;
    for (uint32_t id : layerIds) {
        Layer* l = prog.findLayer(id).second;
        if (!l) {
            EditResult r;
            r.error = "no layer with id " + std::to_string(id);
            return r;
        }
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        targets.push_back(l);
        ids.push_back(id);
    }
    return applySet(layerParams(), targets, ids, edits);
}

EditResult setProgramParams(Program& prog, const std::vector<ParamEdit>& edits) {
    for (const ParamEdit& e : edits) {
        if (e.relative && e.attr == "polyphony") {
            EditResult r;
            r.error = "a relative edit of \"polyphony\" is refused: it is one of a fixed set of choices";
            return r;
        }
    }
    return applySet(programParams(), std::vector<Program*>{&prog}, std::vector<uint32_t>{0}, edits);
}

EditResult restoreZones(Program& prog, const std::vector<PriorValue>& prior) {
    std::vector<Zone*> targets;
    for (const auto& p : prior) targets.push_back(prog.findZone(p.id));
    return applyRestore(zoneParams(), targets, prior);
}

EditResult restoreLayers(Program& prog, const std::vector<PriorValue>& prior) {
    std::vector<Layer*> targets;
    for (const auto& p : prior) targets.push_back(prog.findLayer(p.id).second);
    return applyRestore(layerParams(), targets, prior);
}

EditResult restoreProgramParams(Program& prog, const std::vector<PriorValue>& prior) {
    std::vector<Program*> targets(prior.size(), &prog);
    return applyRestore(programParams(), targets, prior);
}

uint32_t addZone(Program& prog, Zone zone) {
    if (static_cast<int>(zone.layers.size()) > kMaxLayers) return 0;
    zone.id = prog.nextId++;
    for (Layer& l : zone.layers) l.id = prog.nextId++;
    prog.zones.push_back(std::move(zone));
    return prog.zones.back().id;
}

uint32_t addLayer(Program& prog, uint32_t zoneId, Layer layer) {
    Zone* z = prog.findZone(zoneId);
    if (!z || static_cast<int>(z->layers.size()) >= kMaxLayers) return 0;
    layer.id = prog.nextId++;
    z->layers.push_back(std::move(layer));
    return z->layers.back().id;
}

RemovedZone removeZone(Program& prog, uint32_t zoneId) {
    RemovedZone r;
    for (size_t i = 0; i < prog.zones.size(); ++i) {
        if (prog.zones[i].id == zoneId) {
            r.ok = true;
            r.index = i;
            r.zone = std::move(prog.zones[i]);
            prog.zones.erase(prog.zones.begin() + static_cast<std::ptrdiff_t>(i));
            return r;
        }
    }
    return r;
}

RemovedLayer removeLayer(Program& prog, uint32_t layerId) {
    RemovedLayer r;
    for (Zone& z : prog.zones) {
        for (size_t i = 0; i < z.layers.size(); ++i) {
            if (z.layers[i].id == layerId) {
                r.ok = true;
                r.zoneId = z.id;
                r.index = i;
                r.layer = std::move(z.layers[i]);
                z.layers.erase(z.layers.begin() + static_cast<std::ptrdiff_t>(i));
                return r;
            }
        }
    }
    return r;
}

bool reinsertZone(Program& prog, const RemovedZone& removed) {
    if (!removed.ok || removed.index > prog.zones.size() || prog.findZone(removed.zone.id)) return false;
    prog.zones.insert(prog.zones.begin() + static_cast<std::ptrdiff_t>(removed.index), removed.zone);
    return true;
}

bool reinsertLayer(Program& prog, const RemovedLayer& removed) {
    Zone* z = prog.findZone(removed.zoneId);
    if (!removed.ok || !z || removed.index > z->layers.size() ||
        static_cast<int>(z->layers.size()) >= kMaxLayers || prog.findLayer(removed.layer.id).second)
        return false;
    z->layers.insert(z->layers.begin() + static_cast<std::ptrdiff_t>(removed.index), removed.layer);
    return true;
}

}  // namespace nd
