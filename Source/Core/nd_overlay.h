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
// Understood today (decided 2026-10-27, DESIGN §8): one target per verb, and
// beside every changed parameter `p` the value it expects to replace, `p.was`,
// both absolute:
//   <set-zone zone="ID" p="new" p.was="previous" .../>
//   <set-layer layer="ID" p="new" p.was="previous" .../>
// The edits are MADE with recordZoneEdit() / recordLayerEdit(): a relative
// edit ("+2") is resolved there into absolute values, and ALL is expanded
// into one set-zone per zone that exists then, each with its own `.was`.
// undoRecordedEdit() takes the last recorded edit back out.
//
// Each edit ends in one of five states, reported per edit, in order:
//   Applied    it changed the program;
//   Drifted    it changed the program, but the target's value before it was
//              not its `.was` (the base changed under it). Still applied: the
//              overlay's value plays. The report carries was / now per
//              parameter. Compared as the values round-trip through the
//              format (formatParam text of each), so float noise cannot fire it;
//   Orphaned   the zone or layer it names is not in this base (a newer base
//              dropped it). The edit is KEPT in the overlay, never dropped
//              (DESIGN §8: kept and shown greyed out), and applies again if a
//              base brings the id back. Nothing else changes: every other
//              edit applies as it would have;
//   Refused    it names an existing target but cannot be applied (an unknown
//              parameter, a malformed or out-of-range value, a parameter
//              without its `.was` or the reverse, a stored relative "+n",
//              a stored zone="all", which needs expansion). Kept, like an
//              orphan; the program is as if it were not there;
//   Unknown    a verb this version does not understand (a newer writer's).
//              Kept and skipped.
//
// `base` and `baseVersion` are carried and written back; applyOverlay() does
// not compare them with the base it is handed (where a base's own version is
// recorded is not decided yet; the caller chooses the base).
//
// Flatten (DESIGN §8) turns base + overlay into a program of your own: the
// same program applyOverlay() makes, which refers to no base. Nothing here
// runs on the audio thread.

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
    std::string baseVersion;   ///< the base's version the overlay was made on, as written
    std::vector<XmlNode> edits;                                    ///< the verbs, in order
    std::vector<std::pair<std::string, std::string>> attributes;   ///< unknown root attributes, kept

    bool operator==(const Overlay& o) const {
        return version == o.version && base == o.base && baseVersion == o.baseVersion && edits == o.edits &&
               attributes == o.attributes;
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
    uint32_t missingId = 0;    ///< Orphaned: the zone or layer id the base lacks
    std::string error;         ///< Refused / Unknown: why
    std::vector<OverlayDrift> drift;  ///< Drifted: each parameter that drifted
};

struct OverlayResult {
    Program program;                        ///< the base with the overlay applied
    std::vector<OverlayEditReport> edits;   ///< one per edit, in order

    /// Indices of the edits whose target this base does not have.
    std::vector<size_t> orphaned() const;
    /// Indices of the edits that applied over a value other than their `.was`.
    std::vector<size_t> drifted() const;
};

/// The base with the overlay applied, edit by edit, in order. The base is
/// not changed.
OverlayResult applyOverlay(const Program& base, const Overlay& overlay);

/// A program of your own from base + overlay: what applyOverlay() plays,
/// referring to no base. The report says which edits did not apply (an
/// orphaned edit has nothing to apply to, so the flattened program does not
/// carry it).
OverlayResult flatten(const Program& base, const Overlay& overlay);

/// What one recorded edit appended to an overlay, kept for its undo.
struct RecordedEdit {
    bool ok = false;
    std::string error;
    size_t first = 0;             ///< index of its first verb in Overlay::edits
    size_t count = 0;             ///< verbs appended (one per target)
    std::vector<XmlNode> verbs;   ///< the verbs, to check they are still the last ones
    std::vector<PriorValue> prior;  ///< the `.was` values, for the undo
    bool layers = false;          ///< set-layer verbs (else set-zone)
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
/// Undoes the LAST recorded edit: removes its verbs from the overlay and
/// writes its `.was` values back into `current`, so both are as before the
/// edit. Refused (false, nothing changed) when `edit` is not the overlay's
/// last edit.
bool undoRecordedEdit(Overlay& overlay, Program& current, const RecordedEdit& edit);

}  // namespace nd
