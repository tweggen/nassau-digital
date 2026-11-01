#pragma once

// nd_overlay.h - libraries: a read-only base program with your edits on top
// (DESIGN §8, §12).
//
// A program can be based on a read-only BASE program. Your edits are kept as
// a separate OVERLAY: the list of changes, each naming a zone or a layer by
// its stable id, applied to the base in order. What plays is the base with
// the overlay applied; the base is never written (applyOverlay() takes it by
// const reference and returns a new program).
//
//   <NassauDigitalOverlay version="1" base="Library/Strings.ndp" baseVersion="3">
//     <set-zone zone="17" attack="0.25" attack.was="0.1"/>
//     <set-layer layer="18" tune="-1" tune.was="0"/>
//   </NassauDigitalOverlay>
//
// The edits are the verbs of the command language (DESIGN §9.3) in their XML
// form, and are kept as written: an overlay read and written back is
// byte-identical, unknown attributes and verbs included (R4).
//
// Understood today (decided 2026-10-27 and 2026-11-01, DESIGN §8): one
// target per verb, and beside every changed parameter `p` the value it
// expects to replace, `p.was`, both absolute:
//   <set-zone zone="ID" p="new" p.was="previous" .../>
//   <set-layer layer="ID" p="new" p.was="previous" .../>
//   <set-program p="new" p.was="previous" .../>          (2026-11-01)
// and the structural verbs (2026-11-01), which replace nothing and so carry
// no `.was`:
//   <add-zone zone="ID" p="value" .../>                    a zone of the overlay's own
//   <add-layer layer="ID" zone="ZONE" p="value" .../>      a layer of its own, in any zone
//   <hide-zone zone="ID"/>                                 a zone hidden from what plays
//   <hide-layer layer="ID"/>                               a layer hidden from what plays
// An added zone or layer takes its id from the range a base never uses
// (kOverlayIdFirst = 1000000 on), handed out from the overlay's own `nextId`
// root attribute (never reused, like a program's); add-zone writes every
// zone attribute and add-layer every layer attribute, as a program file
// does. A zone the overlay adds is written as one add-zone followed by one
// add-layer per layer. Hiding a zone leaves it in the base: the overlay's
// program does not have it, flatten drops it, and undoing the hide brings it
// back. hide-layer does the same for one layer (the author, 2026-11-01): its
// zone stays, the layer is not in what plays, undo puts it back where it was,
// flatten drops it; a base without the layer (or its zone) orphans it.
// set-program edits any program attribute but `version` (the format's)
// and `libraryVersion` (the base's own: an overlay that rewrote it would
// claim to be a version of the library it is not).
// The edits are MADE with recordZoneEdit() / recordLayerEdit() /
// recordProgramEdit() / recordAddZone() / recordAddLayer() /
// recordHideZone() / recordHideLayer(): a relative edit ("+2") is resolved there into absolute
// values, and ALL or a zone list ("3 5 7", parseZoneScope()) is expanded into
// one set-zone per zone, each with its own `.was`. undoRecordedEdit() takes
// the last recorded edit back out.
//
// Each edit ends in one of five states, reported per edit, in order:
//   Applied    it changed the program;
//   Drifted    it changed the program, but the target's value before it was
//              not its `.was` (the base changed under it). Still applied: the
//              overlay's value plays. The report carries was / now per
//              parameter. Compared as the values round-trip through the
//              format (formatParam text of each), so float noise cannot fire it;
//   Orphaned   the zone or layer it names is not in this base (a newer base
//              dropped it; for add-layer, the zone it adds to). The edit is
//              KEPT in the overlay, never dropped (DESIGN §8: kept and shown
//              greyed out), and applies again if a base brings the id back.
//              Nothing else changes: every other edit applies as it would
//              have;
//   Refused    it cannot be applied (an unknown parameter, a malformed or
//              out-of-range value, a parameter without its `.was` or the
//              reverse, a stored relative "+n", a stored zone="all" or zone
//              list, which needs expansion, an added id outside the overlay
//              range or already in the base, a full zone, a set-program of
//              `version` or `libraryVersion`). Kept, like an orphan; the
//              program is as if it were not there;
//   Unknown    a verb this version does not understand (a newer writer's).
//              Kept and skipped.
//
// Library versions (decided 2026-11-01): `baseVersion` is the base's
// `libraryVersion` when the overlay was made (recorded with the overlay's
// first edit, when the base has one). Applied to a base whose libraryVersion
// is another, the overlay still applies, edit by edit as always, and the
// result reports the mismatch in OverlayResult::baseDrift, as one
// OverlayDrift {"libraryVersion", baseVersion, the base's}. An overlay with
// no baseVersion is not compared. `base` (where the base came from) is
// carried and written back; the caller chooses the base.
//
// Rebase (the author, 2026-11-01): after a drift or version report, the user
// accepts the new base. rebaseOverlay() stamps baseVersion with the new
// base's libraryVersion and rewrites the `.was` of every edit that DRIFTED on
// it to the value the new base (with the earlier edits applied, in order) has
// under it, so both reports clear. What plays does not change: every new
// value stays. Orphaned, refused and unknown edits are left exactly as they
// are (an orphan stays an orphan and applies again if its target returns),
// and an edit that applied without drift already names the new base's value
// and is left byte for byte. undoRebase() takes it back.
//
// Flatten (DESIGN §8) turns base + overlay into a program of your own: the
// same program applyOverlay() makes, which refers to no base, and so carries
// no libraryVersion either. Nothing here runs on the audio thread.

#include <cstddef>
#include <string>
#include <vector>

#include "nd_edit.h"
#include "nd_program.h"
#include "nd_xml.h"

namespace nd {

constexpr const char* kOverlayElement = "NassauDigitalOverlay";

struct Overlay {
    int version = 1;
    std::string base;          ///< where the base came from; the core never reads it
    std::string baseVersion;   ///< the base's libraryVersion the overlay was made on, as written
    /// The next id for a zone or layer the overlay adds (kOverlayIdFirst on);
    /// 0 until the overlay has added one, and then not written.
    uint32_t nextId = 0;
    std::vector<XmlNode> edits;                                    ///< the verbs, in order
    std::vector<std::pair<std::string, std::string>> attributes;   ///< unknown root attributes, kept

    bool operator==(const Overlay& o) const {
        return version == o.version && base == o.base && baseVersion == o.baseVersion && nextId == o.nextId &&
               edits == o.edits && attributes == o.attributes;
    }
    bool operator!=(const Overlay& o) const { return !(*this == o); }
};

struct OverlayParseResult {
    bool ok = false;
    std::string error;
    Overlay overlay;
};

OverlayParseResult parseOverlay(const std::string& xml);
OverlayParseResult overlayFromXml(const XmlNode& root);
std::string writeOverlay(const Overlay& overlay);
XmlNode overlayToXml(const Overlay& overlay);

enum class OverlayEditState { Applied = 0, Orphaned, Refused, Unknown, Drifted };

/// One parameter whose value before the edit was not the edit's `.was`.
struct OverlayDrift {
    std::string attr;
    std::string was;  ///< what the edit expected, as written by formatParam
    std::string now;  ///< what the program had, as written by formatParam
    bool operator==(const OverlayDrift& o) const { return attr == o.attr && was == o.was && now == o.now; }
};

struct OverlayEditReport {
    size_t index = 0;          ///< position of the edit in Overlay::edits
    OverlayEditState state = OverlayEditState::Applied;
    uint32_t missingId = 0;    ///< Orphaned: the zone or layer id the base lacks (add-layer: its zone)
    std::string error;         ///< Refused / Unknown: why
    std::vector<OverlayDrift> drift;  ///< Drifted: each parameter that drifted
};

struct OverlayResult {
    Program program;                        ///< the base with the overlay applied
    std::vector<OverlayEditReport> edits;   ///< one per edit, in order
    /// The base is of another library version than the overlay was made on:
    /// {"libraryVersion", the overlay's baseVersion, the base's}. Empty when
    /// they agree or the overlay records none. The edits apply all the same.
    std::vector<OverlayDrift> baseDrift;

    bool versionMismatch() const { return !baseDrift.empty(); }

    /// Indices of the edits whose target this base does not have.
    std::vector<size_t> orphaned() const;
    /// Indices of the edits that applied over a value other than their `.was`.
    std::vector<size_t> drifted() const;
};

/// The base with the overlay applied, edit by edit, in order. The base is
/// not changed.
OverlayResult applyOverlay(const Program& base, const Overlay& overlay);

/// A program of your own from base + overlay: what applyOverlay() plays,
/// referring to no base (its libraryVersion is cleared). The report says
/// which edits did not apply (an orphaned edit has nothing to apply to, so
/// the flattened program does not carry it); a hidden zone is not in it.
OverlayResult flatten(const Program& base, const Overlay& overlay);

enum class RecordedKind { SetZone = 0, SetLayer, SetProgram, AddZone, AddLayer, HideZone, HideLayer };

/// What one recorded edit appended to an overlay, kept for its undo.
struct RecordedEdit {
    bool ok = false;
    std::string error;
    RecordedKind kind = RecordedKind::SetZone;
    size_t first = 0;             ///< index of its first verb in Overlay::edits
    size_t count = 0;             ///< verbs appended (one per target; add-zone: 1 + its layers)
    std::vector<XmlNode> verbs;   ///< the verbs, to check they are still the last ones
    std::vector<PriorValue> prior;  ///< set verbs: the `.was` values, for the undo
    uint32_t id = 0;              ///< AddZone / AddLayer: the id handed out; HideZone: the zone hidden
    RemovedZone hidden;           ///< HideZone: the zone and where it was, for the undo
    RemovedLayer hiddenLayer;     ///< HideLayer: the layer and where it was, for the undo
    bool stampedBaseVersion = false;  ///< this edit recorded the overlay's baseVersion
};

/// Makes a zone edit on `current` (what plays: the base with the overlay
/// applied) and records it in the overlay: one set-zone per target zone,
/// each parameter with its new absolute value and its `.was`, the zone's
/// value before the edit. A relative ParamEdit is resolved per zone (clamped
/// per zone, DESIGN §2.1); ALL is expanded over the zones `current` has now
/// (a zone a later base adds is not covered). All-or-nothing: a refusal
/// changes neither `current` nor the overlay and says why.
RecordedEdit recordZoneEdit(Overlay& overlay, Program& current, const ZoneScope& scope,
                            const std::vector<ParamEdit>& edits);
/// The same for layers: one set-layer per layer id.
RecordedEdit recordLayerEdit(Overlay& overlay, Program& current, const std::vector<uint32_t>& layerIds,
                             const std::vector<ParamEdit>& edits);
/// The same for the program's own attributes: one set-program. `version` and
/// `libraryVersion` are refused (not an overlay's to edit).
RecordedEdit recordProgramEdit(Overlay& overlay, Program& current, const std::vector<ParamEdit>& edits);
/// Adds `zone` (with its layers) as the overlay's own: the zone and each
/// layer get the next ids of the overlay range (whatever ids they carried are
/// ignored); recorded as one add-zone and one add-layer per layer. RecordedEdit::id
/// is the zone's id. Refused: more than four layers.
RecordedEdit recordAddZone(Overlay& overlay, Program& current, Zone zone);
/// Adds `layer` to any zone `current` has (the base's or the overlay's own),
/// with the next id of the overlay range. Refused: no such zone, or full.
RecordedEdit recordAddLayer(Overlay& overlay, Program& current, uint32_t zoneId, Layer layer);
/// Removes a zone from what plays by a stored hide-zone; the base keeps it.
RecordedEdit recordHideZone(Overlay& overlay, Program& current, uint32_t zoneId);
/// Removes one layer from what plays by a stored hide-layer; the base keeps it.
RecordedEdit recordHideLayer(Overlay& overlay, Program& current, uint32_t layerId);
/// Undoes the LAST recorded edit: removes its verbs from the overlay and
/// writes its `.was` values back into `current` (an add: removes what it
/// added; a hide: puts the zone back where it was), so both are as before
/// the edit, except that the overlay's nextId stays advanced (ids are never
/// reused, like a program's). Refused (false, nothing changed) when `edit`
/// is not the overlay's last edit.
bool undoRecordedEdit(Overlay& overlay, Program& current, const RecordedEdit& edit);

/// What a rebase did, kept for its undo.
struct RebasedOverlay {
    bool ok = false;
    std::string fromVersion, toVersion;  ///< baseVersion before and after
    std::vector<size_t> refreshed;       ///< edits whose `.was` were rewritten (they had drifted)
    std::vector<size_t> orphaned;        ///< edits still orphaned on the new base, left as they were
    Overlay before, after;               ///< the overlay before and after the rebase
    Program currentBefore;               ///< what played before (on the old base)
};

/// Accepts `newBase` as the overlay's base: re-stamps baseVersion and
/// refreshes the drifted edits' `.was` (see the header comment). `current`
/// becomes newBase with the overlay applied. Undoable with undoRebase().
RebasedOverlay rebaseOverlay(Overlay& overlay, Program& current, const Program& newBase);
/// Undoes a rebase: the overlay and `current` as before it. Refused (false,
/// nothing changed) when the overlay has changed since the rebase.
bool undoRebase(Overlay& overlay, Program& current, const RebasedOverlay& rebased);

}  // namespace nd
