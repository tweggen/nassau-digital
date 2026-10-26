#pragma once

// nd_edit.h - edits of a program (DESIGN §2, §2.1).
//
// Every edit either applies completely or not at all: it is validated first,
// and a refusal leaves the program untouched and says why. Every edit that
// succeeds returns what it replaced, so an undo is one more call:
//
//   set     -> the previous ABSOLUTE value of every parameter of every target
//              (DESIGN §2.1: a relative edit clamps per zone and so cannot be
//              undone by its opposite; its undo restores the absolute values);
//              restore() writes them back.
//   add     -> the new id; remove() with it undoes the add.
//   remove  -> the removed object and where it was; reinsert() undoes it.
//
// Ids are handed out from Program::nextId and never reused (DESIGN §2):
// removing the zone with the highest id does not lower nextId.
//
// The textual syntax of a verb (`tune="+2"`, `zone="all"`) is the front end's
// (qbx's .qxa verbs, DESIGN §9.3). Here a relative edit is explicit.

#include <cstdint>
#include <string>
#include <vector>

#include "nd_program.h"

namespace nd {

/// One parameter of a set edit: absolute (`value`, as XML text) or relative
/// (`delta`, added to each target's own value, then clamped to the range).
struct ParamEdit {
    std::string attr;
    bool relative = false;
    double delta = 0.0;
    std::string value;

    static ParamEdit absolute(std::string attr, std::string value) {
        ParamEdit e;
        e.attr = std::move(attr);
        e.value = std::move(value);
        return e;
    }
    static ParamEdit by(std::string attr, double delta) {
        ParamEdit e;
        e.attr = std::move(attr);
        e.relative = true;
        e.delta = delta;
        return e;
    }
};

/// Which zones a zone edit reaches: the listed ids, or ALL (DESIGN §2.1).
struct ZoneScope {
    bool all = false;
    std::vector<uint32_t> ids;
    static ZoneScope everyZone() { ZoneScope s; s.all = true; return s; }
    static ZoneScope zones(std::vector<uint32_t> ids) { ZoneScope s; s.ids = std::move(ids); return s; }
};

/// One replaced value: the target's id (0 for the program itself), the
/// parameter, and its previous absolute value as XML text.
struct PriorValue {
    uint32_t id = 0;
    std::string attr;
    std::string value;
    bool operator==(const PriorValue& o) const { return id == o.id && attr == o.attr && value == o.value; }
};

struct EditResult {
    bool ok = false;
    std::string error;
    std::vector<PriorValue> prior;  ///< set edits: what they replaced
};

/// Sets parameters on the zones in scope.
EditResult setZones(Program& prog, const ZoneScope& scope, const std::vector<ParamEdit>& edits);
/// Sets parameters on the listed layers (by id, in whatever zone).
EditResult setLayers(Program& prog, const std::vector<uint32_t>& layerIds, const std::vector<ParamEdit>& edits);
/// Sets program-level parameters (name, polyphony, busDrive, ...).
EditResult setProgramParams(Program& prog, const std::vector<ParamEdit>& edits);

/// Writes back the values a set edit returned (the undo of a set).
EditResult restoreZones(Program& prog, const std::vector<PriorValue>& prior);
EditResult restoreLayers(Program& prog, const std::vector<PriorValue>& prior);
EditResult restoreProgramParams(Program& prog, const std::vector<PriorValue>& prior);

/// Adds `zone` at the end, giving it and each of its layers a fresh id
/// (whatever ids they carried are ignored). Returns the zone's id, 0 if
/// refused (more than four layers).
uint32_t addZone(Program& prog, Zone zone);
/// Adds `layer` to the zone with a fresh id; 0 if the zone is missing or full.
uint32_t addLayer(Program& prog, uint32_t zoneId, Layer layer);

struct RemovedZone {
    bool ok = false;
    size_t index = 0;
    Zone zone;
};
struct RemovedLayer {
    bool ok = false;
    uint32_t zoneId = 0;
    size_t index = 0;
    Layer layer;
};

RemovedZone removeZone(Program& prog, uint32_t zoneId);
RemovedLayer removeLayer(Program& prog, uint32_t layerId);
/// The undo of a remove: puts the object back where it was, with its ids.
bool reinsertZone(Program& prog, const RemovedZone& removed);
bool reinsertLayer(Program& prog, const RemovedLayer& removed);

}  // namespace nd
