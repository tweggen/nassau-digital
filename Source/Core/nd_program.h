#pragma once

// nd_program.h - the program model (DESIGN §2, §4, §12).
//
//   Program   name, polyphony, kit-bus drive, output level, master tune, nextId
//    └─ Zone × n      key × velocity rectangle, root key, play mode, drive,
//        │            filter, envelopes, modulation routes, pan, level, tune,
//        │            choke group, output, pad
//        └─ Layer × 1..4   sample reference, start/end, loop points, beats,
//                          velocity window and fade, rrGroup, tune, level,
//                          pan, reverse
//
// Every attribute of a zone, a layer and the program is described once, in a
// parameter table (zoneParams(), layerParams(), programParams()): its XML
// name, its kind, its range and its default. The XML reader and writer
// (nd_program_xml.h) and the edits (nd_edit.h) both go through that table, so
// an attribute cannot be readable but not writable, or editable but not
// saved. R4: the tables are append-only.
//
// Units, chosen once here and used everywhere:
//   keys and velocities   MIDI numbers 0..127
//   level, outputLevel    dB
//   tune, masterTune      semitones (fractional)
//   pan                   -1 (left) .. +1 (right)
//   times                 seconds
//   cutoff                Hz
//   cutoff modulation     semitones of cutoff (filterEnvAmount, velToCutoff,
//                         lfoToCutoff, modWheelToCutoff)
//   frames                sample frames at the sample's own rate
//
// The model is plain data: no Qt, no I/O, no threads. It is what the
// instrument is handed as an immutable snapshot (DESIGN §9.1).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nd_xml.h"

namespace nd {

/// Unknown attributes and child elements, kept on load and written back on
/// save (DESIGN §12, R4), in their original order.
struct Extras {
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;
    bool operator==(const Extras& o) const { return attributes == o.attributes && children == o.children; }
};

enum class PlayMode : int { OneShot = 0, Gated, Loop, TempoLoop, TempoOneShot };
enum class TempoMethod : int { Stretch = 0, Resample };
enum class DriveCurve : int { Soft = 0, Tube, Hard, Fold };
enum class FilterType : int { Lp24 = 0, Lp12, Hp12, Hp24 };

/// Output of a zone: 0 = main, 1..8 = aux 1..8 (DESIGN §3.1).
constexpr int kNumAuxOutputs = 8;
/// Up to four layers per zone (DESIGN §1, decided).
constexpr int kMaxLayers = 4;

struct Layer {
    uint32_t id = 0;
    std::string src;           ///< where the audio came from; the core never reads it (DESIGN §6)
    int64_t start = 0;         ///< first frame played
    int64_t end = -1;          ///< one past the last frame; -1 = the sample's end
    int64_t loopStart = -1;    ///< -1 = start
    int64_t loopEnd = -1;      ///< one past the loop's last frame; -1 = end
    double beats = 0.0;        ///< length in beats for the tempo modes; 0 = unknown
    int velLo = 0;
    int velHi = 127;
    int velFade = 0;           ///< crossfade width in velocity steps (DESIGN §2)
    int rrGroup = 0;           ///< 0 = not round-robin; layers sharing a number take turns (§2.2)
    double tune = 0.0;
    double level = 0.0;
    double pan = 0.0;
    bool reverse = false;
    Extras extras;

    bool operator==(const Layer& o) const;
    bool operator!=(const Layer& o) const { return !(*this == o); }
};

struct Zone {
    uint32_t id = 0;
    int keyLo = 0;
    int keyHi = 127;
    int velLo = 0;
    int velHi = 127;
    int rootKey = 60;
    bool keyTrack = true;      ///< off: every key plays at the root pitch (drums)
    PlayMode mode = PlayMode::Gated;
    TempoMethod tempoMethod = TempoMethod::Stretch;
    // Drive (DESIGN §4.3), applied from gate C5.
    double drive = 0.0;
    DriveCurve driveCurve = DriveCurve::Soft;
    double driveMix = 1.0;
    // Filter (DESIGN §4.2), applied from gate C5.
    FilterType filter = FilterType::Lp24;
    double cutoff = 20000.0;
    double resonance = 0.0;
    double filterKeyTrack = 0.0;   ///< 0..1: fraction of the key's distance from rootKey added to cutoff
    double filterEnvAmount = 0.0;  ///< semitones of cutoff at full filter envelope
    // Amp envelope (DESIGN §4.1): ADSR plus hold.
    double attack = 0.0;
    double hold = 0.0;
    double decay = 0.0;
    double sustain = 1.0;
    double release = 0.05;
    // Filter envelope (DESIGN §4.4): ADSR.
    double filterAttack = 0.0;
    double filterDecay = 0.0;
    double filterSustain = 1.0;
    double filterRelease = 0.05;
    // The one LFO and the fixed routes (DESIGN §4.4).
    double lfoRate = 2.0;              ///< Hz
    double lfoToPitch = 0.0;           ///< semitones at full depth
    double lfoToCutoff = 0.0;          ///< semitones at full depth
    double lfoToPan = 0.0;             ///< pan units at full depth
    double velToLevel = 1.0;           ///< 0..1
    double velToCutoff = 0.0;          ///< semitones at velocity 127 relative to velocity 0
    double velToStart = 0.0;           ///< frames skipped at velocity 0 (none at 127)
    double modWheelToLfoDepth = 0.0;   ///< 0..1
    double modWheelToCutoff = 0.0;     ///< semitones at full wheel
    double pan = 0.0;
    double level = 0.0;
    double tune = 0.0;
    int choke = 0;                     ///< 0 = no choke group
    int output = 0;                    ///< 0 = main, 1..8 = aux
    int pad = -1;                      ///< pad assignment (DESIGN §7); -1 = none
    std::vector<Layer> layers;
    Extras extras;

    bool operator==(const Zone& o) const;
    bool operator!=(const Zone& o) const { return !(*this == o); }
};

/// Polyphony choices (DESIGN §4.5, decided): 8, 16, ..., 256; default 32.
constexpr int kPolyphonyChoices[] = {8, 16, 32, 64, 128, 256};
constexpr int kDefaultPolyphony = 32;
constexpr int kMaxPolyphony = 256;
bool isValidPolyphony(int voices);

struct Program {
    int version = 1;
    std::string name;
    int polyphony = kDefaultPolyphony;
    double busDrive = 0.0;
    double outputLevel = 0.0;
    double masterTune = 0.0;
    uint32_t nextId = 1;   ///< the next id to hand out; ids are never reused (DESIGN §2)
    std::vector<Zone> zones;
    Extras extras;

    bool operator==(const Program& o) const;
    bool operator!=(const Program& o) const { return !(*this == o); }

    Zone* findZone(uint32_t id);
    const Zone* findZone(uint32_t id) const;
    /// The layer with this id and the zone holding it, or {nullptr, nullptr}.
    std::pair<Zone*, Layer*> findLayer(uint32_t id);
    std::pair<const Zone*, const Layer*> findLayer(uint32_t id) const;
};

// ---- The parameter tables ---------------------------------------------------

enum class ParamKind { Int, Real, Bool, Enum, String };

/// One attribute: how it is named, typed, bounded and defaulted. Numeric
/// values (Int, Real, Bool as 0/1, Enum as its index) travel as double; a
/// String travels as text. `id` is not in the tables: it is not an editable
/// parameter.
template <typename T>
struct ParamDesc {
    const char* name;
    ParamKind kind;
    double minValue;
    double maxValue;
    const char* const* enumNames;  ///< Enum only
    int numEnumNames;              ///< Enum only
    double (*get)(const T&);
    void (*set)(T&, double);
    const std::string& (*getText)(const T&);    ///< String only
    void (*setText)(T&, const std::string&);    ///< String only
};

using ZoneParam = ParamDesc<Zone>;
using LayerParam = ParamDesc<Layer>;
using ProgramParam = ParamDesc<Program>;

const std::vector<ZoneParam>& zoneParams();
const std::vector<LayerParam>& layerParams();
const std::vector<ProgramParam>& programParams();

const ZoneParam* findZoneParam(const std::string& name);
const LayerParam* findLayerParam(const std::string& name);
const ProgramParam* findProgramParam(const std::string& name);

/// True for a parameter a relative edit may move (Int and Real; DESIGN §2.1).
template <typename T>
bool isNumeric(const ParamDesc<T>& p) {
    return p.kind == ParamKind::Int || p.kind == ParamKind::Real;
}

/// The parameter's value as it is written in XML.
template <typename T>
std::string formatParam(const ParamDesc<T>& p, const T& obj);

/// Parses `text` as the parameter's value and stores it. Refuses (returns
/// false, leaves `obj` unchanged, says why in `error`) a malformed value, a
/// value out of range, an unknown enum name or an invalid polyphony.
template <typename T>
bool parseParam(const ParamDesc<T>& p, T& obj, const std::string& text, std::string& error);

/// Locale-independent shortest round-trip formatting of a double, and its
/// inverse. "C" locale always: a host that called setlocale() (Qt does) must
/// not turn 0.5 into "0,5".
std::string formatReal(double v);
bool parseReal(const std::string& text, double& out);
bool parseInteger(const std::string& text, long long& out);

}  // namespace nd
