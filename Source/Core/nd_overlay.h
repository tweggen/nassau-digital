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
//     <set-zone zone="17" attack="0.25"/>
//     <set-layer layer="18" tune="-1"/>
//   </NassauDigitalOverlay>
//
// The edits are the verbs of the command language (DESIGN §9.3) in their XML
// form, and are kept as written: an overlay read and written back is
// byte-identical, unknown attributes and verbs included (R4).
//
// Understood today:
//   <set-zone zone="ID|all" attr="value" .../>   absolute values of zone
//                                                parameters (nd_program.h)
//   <set-layer layer="ID" attr="value" .../>     absolute values of layer
//                                                parameters
//
// Each edit ends in one of four states, reported per edit, in order:
//   Applied    it changed the program;
//   Orphaned   the zone or layer it names is not in this base (a newer base
//              dropped it). The edit is KEPT in the overlay, never dropped
//              (DESIGN §8: kept and shown greyed out), and applies again if a
//              base brings the id back. Nothing else changes: every other
//              edit applies as it would have;
//   Refused    it names an existing target but cannot be applied (an unknown
//              parameter, a malformed or out-of-range value, a value written
//              as a relative "+n", whose meaning in an overlay is not defined
//              yet). Kept, like an orphan; the program is as if it were not
//              there;
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

enum class OverlayEditState { Applied = 0, Orphaned, Refused, Unknown };

struct OverlayEditReport {
    size_t index = 0;          ///< position of the edit in Overlay::edits
    OverlayEditState state = OverlayEditState::Applied;
    uint32_t missingId = 0;    ///< Orphaned: the zone or layer id the base lacks
    std::string error;         ///< Refused / Unknown: why
};

struct OverlayResult {
    Program program;                        ///< the base with the overlay applied
    std::vector<OverlayEditReport> edits;   ///< one per edit, in order

    /// Indices of the edits whose target this base does not have.
    std::vector<size_t> orphaned() const;
};

/// The base with the overlay applied, edit by edit, in order. The base is
/// not changed.
OverlayResult applyOverlay(const Program& base, const Overlay& overlay);

/// A program of your own from base + overlay: what applyOverlay() plays,
/// referring to no base. The report says which edits did not apply (an
/// orphaned edit has nothing to apply to, so the flattened program does not
/// carry it).
OverlayResult flatten(const Program& base, const Overlay& overlay);

}  // namespace nd
