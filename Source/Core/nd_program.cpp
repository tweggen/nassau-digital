#include "nd_program.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace nd {

// ---- Equality ---------------------------------------------------------------

bool Layer::operator==(const Layer& o) const {
    return id == o.id && src == o.src && start == o.start && end == o.end && loopStart == o.loopStart &&
           loopEnd == o.loopEnd && beats == o.beats && velLo == o.velLo && velHi == o.velHi &&
           velFade == o.velFade && rrGroup == o.rrGroup && tune == o.tune && level == o.level &&
           pan == o.pan && reverse == o.reverse && extras == o.extras;
}

bool Zone::operator==(const Zone& o) const {
    if (id != o.id || layers != o.layers || !(extras == o.extras)) return false;
    for (const auto& p : zoneParams())
        if (p.get(*this) != p.get(o)) return false;
    return true;
}

bool Program::operator==(const Program& o) const {
    if (name != o.name || nextId != o.nextId || zones != o.zones || !(extras == o.extras)) return false;
    for (const auto& p : programParams())
        if (p.kind != ParamKind::String && p.get(*this) != p.get(o)) return false;
    return true;
}

Zone* Program::findZone(uint32_t id) {
    for (auto& z : zones)
        if (z.id == id) return &z;
    return nullptr;
}

const Zone* Program::findZone(uint32_t id) const { return const_cast<Program*>(this)->findZone(id); }

std::pair<Zone*, Layer*> Program::findLayer(uint32_t id) {
    for (auto& z : zones)
        for (auto& l : z.layers)
            if (l.id == id) return {&z, &l};
    return {nullptr, nullptr};
}

std::pair<const Zone*, const Layer*> Program::findLayer(uint32_t id) const {
    auto r = const_cast<Program*>(this)->findLayer(id);
    return {r.first, r.second};
}

bool isValidPolyphony(int voices) {
    for (int c : kPolyphonyChoices)
        if (c == voices) return true;
    return false;
}

// ---- Number formatting --------------------------------------------------------

std::string formatReal(double v) {
    if (v == 0.0) return std::signbit(v) ? "-0" : "0";
    // The shortest of 15..17 significant digits that reads back exactly: 0.2
    // stays "0.2", and every double still round-trips (17 digits always do).
    for (int prec = 15; prec <= 17; ++prec) {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        os << std::setprecision(prec) << v;
        double back = 0.0;
        if (parseReal(os.str(), back) && back == v) return os.str();
    }
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(17) << v;
    return os.str();
}

bool parseReal(const std::string& text, double& out) {
    if (text.empty()) return false;
    // Only plain decimal numbers: no hex floats, inf or nan in a program file.
    for (char c : text) {
        const bool ok = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E';
        if (!ok) return false;
    }
    std::istringstream is(text);
    is.imbue(std::locale::classic());
    double v = 0.0;
    is >> v;
    if (is.fail()) return false;
    is.peek();
    if (!is.eof()) return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parseInteger(const std::string& text, long long& out) {
    if (text.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (text[0] == '-' || text[0] == '+') {
        neg = text[0] == '-';
        i = 1;
    }
    if (i >= text.size()) return false;
    unsigned long long v = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned long long>(c - '0');
        if (v > (1ull << 62)) return false;
    }
    out = neg ? -static_cast<long long>(v) : static_cast<long long>(v);
    return true;
}

// ---- The tables -----------------------------------------------------------------

namespace {

const char* const kModeNames[] = {"oneshot", "gated", "loop", "tempo-loop", "tempo-oneshot"};
const char* const kTempoMethodNames[] = {"stretch", "resample"};
const char* const kDriveCurveNames[] = {"soft", "tube", "hard", "fold"};
const char* const kFilterNames[] = {"lp24", "lp12", "hp12", "hp24", "off"};  // append-only (R4)
const char* const kOutputNames[] = {"main", "aux1", "aux2", "aux3", "aux4", "aux5", "aux6", "aux7", "aux8"};

const std::string& noText(const void*) {
    static const std::string empty;
    return empty;
}

constexpr double kMaxFrames = 9.0e15;  // within double's exact integers
constexpr double kMaxTime = 60.0;      // seconds, any envelope segment

}  // namespace

// A numeric field of T. The lambdas capture nothing, so they decay to the
// table's function pointers.
#define ND_NUM(T, field, kind, lo, hi)                                                            \
    ParamDesc<T> {                                                                                \
        #field, ParamKind::kind, lo, hi, nullptr, 0,                                              \
            [](const T& o) { return static_cast<double>(o.field); },                              \
            [](T& o, double v) { o.field = static_cast<decltype(o.field)>(v); },                  \
            [](const T& o) -> const std::string& { return noText(&o); }, [](T&, const std::string&) {} \
    }
#define ND_BOOL(T, field)                                                                         \
    ParamDesc<T> {                                                                                \
        #field, ParamKind::Bool, 0, 1, nullptr, 0, [](const T& o) { return o.field ? 1.0 : 0.0; },  \
            [](T& o, double v) { o.field = v != 0.0; },                                           \
            [](const T& o) -> const std::string& { return noText(&o); }, [](T&, const std::string&) {} \
    }
#define ND_ENUM(T, field, names)                                                                  \
    ParamDesc<T> {                                                                                \
        #field, ParamKind::Enum, 0, static_cast<double>(sizeof(names) / sizeof(names[0]) - 1),    \
            names, static_cast<int>(sizeof(names) / sizeof(names[0])),                            \
            [](const T& o) { return static_cast<double>(static_cast<int>(o.field)); },            \
            [](T& o, double v) { o.field = static_cast<decltype(o.field)>(static_cast<int>(v)); }, \
            [](const T& o) -> const std::string& { return noText(&o); }, [](T&, const std::string&) {} \
    }
#define ND_TEXT(T, field)                                                                         \
    ParamDesc<T> {                                                                                \
        #field, ParamKind::String, 0, 0, nullptr, 0, [](const T&) { return 0.0; }, [](T&, double) {}, \
            [](const T& o) -> const std::string& { return o.field; },                             \
            [](T& o, const std::string& s) { o.field = s; }                                       \
    }

const std::vector<ZoneParam>& zoneParams() {
    // Append-only (R4): a new attribute goes at the end.
    static const std::vector<ZoneParam> t = {
        ND_NUM(Zone, keyLo, Int, 0, 127),
        ND_NUM(Zone, keyHi, Int, 0, 127),
        ND_NUM(Zone, velLo, Int, 0, 127),
        ND_NUM(Zone, velHi, Int, 0, 127),
        ND_NUM(Zone, rootKey, Int, 0, 127),
        ND_BOOL(Zone, keyTrack),
        ND_ENUM(Zone, mode, kModeNames),
        ND_ENUM(Zone, tempoMethod, kTempoMethodNames),
        ND_NUM(Zone, drive, Real, 0, 1),
        ND_ENUM(Zone, driveCurve, kDriveCurveNames),
        ND_NUM(Zone, driveMix, Real, 0, 1),
        ND_ENUM(Zone, filter, kFilterNames),
        ND_NUM(Zone, cutoff, Real, 20, 20000),
        ND_NUM(Zone, resonance, Real, 0, 1),
        ND_NUM(Zone, filterKeyTrack, Real, 0, 1),
        ND_NUM(Zone, filterEnvAmount, Real, -96, 96),
        ND_NUM(Zone, attack, Real, 0, kMaxTime),
        ND_NUM(Zone, hold, Real, 0, kMaxTime),
        ND_NUM(Zone, decay, Real, 0, kMaxTime),
        ND_NUM(Zone, sustain, Real, 0, 1),
        ND_NUM(Zone, release, Real, 0, kMaxTime),
        ND_NUM(Zone, filterAttack, Real, 0, kMaxTime),
        ND_NUM(Zone, filterDecay, Real, 0, kMaxTime),
        ND_NUM(Zone, filterSustain, Real, 0, 1),
        ND_NUM(Zone, filterRelease, Real, 0, kMaxTime),
        ND_NUM(Zone, lfoRate, Real, 0.01, 50),
        ND_NUM(Zone, lfoToPitch, Real, -24, 24),
        ND_NUM(Zone, lfoToCutoff, Real, -96, 96),
        ND_NUM(Zone, lfoToPan, Real, 0, 1),
        ND_NUM(Zone, velToLevel, Real, 0, 1),
        ND_NUM(Zone, velToCutoff, Real, -96, 96),
        ND_NUM(Zone, velToStart, Real, 0, kMaxFrames),
        ND_NUM(Zone, modWheelToLfoDepth, Real, 0, 1),
        ND_NUM(Zone, modWheelToCutoff, Real, -96, 96),
        ND_NUM(Zone, pan, Real, -1, 1),
        ND_NUM(Zone, level, Real, -144, 24),
        ND_NUM(Zone, tune, Real, -96, 96),
        ND_NUM(Zone, choke, Int, 0, 127),
        ND_ENUM(Zone, output, kOutputNames),
        ND_NUM(Zone, pad, Int, -1, 127),
    };
    return t;
}

const std::vector<LayerParam>& layerParams() {
    static const std::vector<LayerParam> t = {
        ND_TEXT(Layer, src),
        ND_NUM(Layer, start, Int, 0, kMaxFrames),
        ND_NUM(Layer, end, Int, -1, kMaxFrames),
        ND_NUM(Layer, loopStart, Int, -1, kMaxFrames),
        ND_NUM(Layer, loopEnd, Int, -1, kMaxFrames),
        ND_NUM(Layer, beats, Real, 0, 1.0e6),
        ND_NUM(Layer, velLo, Int, 0, 127),
        ND_NUM(Layer, velHi, Int, 0, 127),
        ND_NUM(Layer, velFade, Int, 0, 127),
        ND_NUM(Layer, rrGroup, Int, 0, 127),
        ND_NUM(Layer, tune, Real, -96, 96),
        ND_NUM(Layer, level, Real, -144, 24),
        ND_NUM(Layer, pan, Real, -1, 1),
        ND_BOOL(Layer, reverse),
    };
    return t;
}

const std::vector<ProgramParam>& programParams() {
    static const std::vector<ProgramParam> t = {
        ND_NUM(Program, version, Int, 1, 1.0e6),
        ND_TEXT(Program, name),
        ND_NUM(Program, polyphony, Int, 8, 256),
        ND_NUM(Program, busDrive, Real, 0, 1),
        ND_NUM(Program, outputLevel, Real, -144, 24),
        ND_NUM(Program, masterTune, Real, -96, 96),
    };
    return t;
}

#undef ND_NUM
#undef ND_BOOL
#undef ND_ENUM
#undef ND_TEXT

namespace {
template <typename T>
const ParamDesc<T>* findIn(const std::vector<ParamDesc<T>>& t, const std::string& name) {
    for (const auto& p : t)
        if (name == p.name) return &p;
    return nullptr;
}
}  // namespace

const ZoneParam* findZoneParam(const std::string& name) { return findIn(zoneParams(), name); }
const LayerParam* findLayerParam(const std::string& name) { return findIn(layerParams(), name); }
const ProgramParam* findProgramParam(const std::string& name) { return findIn(programParams(), name); }

// ---- Formatting and parsing a parameter --------------------------------------------

template <typename T>
std::string formatParam(const ParamDesc<T>& p, const T& obj) {
    switch (p.kind) {
        case ParamKind::String: return p.getText(obj);
        case ParamKind::Real: return formatReal(p.get(obj));
        case ParamKind::Enum: {
            const int i = static_cast<int>(p.get(obj));
            return (i >= 0 && i < p.numEnumNames) ? p.enumNames[i] : std::string("?");
        }
        case ParamKind::Bool: return p.get(obj) != 0.0 ? "1" : "0";
        case ParamKind::Int: {
            std::ostringstream os;
            os.imbue(std::locale::classic());
            os << static_cast<long long>(p.get(obj));
            return os.str();
        }
    }
    return std::string();
}

template <typename T>
bool parseParam(const ParamDesc<T>& p, T& obj, const std::string& text, std::string& error) {
    switch (p.kind) {
        case ParamKind::String: p.setText(obj, text); return true;
        case ParamKind::Enum:
            for (int i = 0; i < p.numEnumNames; ++i) {
                if (text == p.enumNames[i]) {
                    p.set(obj, static_cast<double>(i));
                    return true;
                }
            }
            error = std::string(p.name) + ": unknown value \"" + text + "\"";
            return false;
        case ParamKind::Bool:
            if (text == "1" || text == "true") { p.set(obj, 1.0); return true; }
            if (text == "0" || text == "false") { p.set(obj, 0.0); return true; }
            error = std::string(p.name) + ": expected 0 or 1, got \"" + text + "\"";
            return false;
        case ParamKind::Int: {
            long long v = 0;
            if (!parseInteger(text, v)) {
                error = std::string(p.name) + ": expected an integer, got \"" + text + "\"";
                return false;
            }
            const double d = static_cast<double>(v);
            if (d < p.minValue || d > p.maxValue) {
                error = std::string(p.name) + ": " + text + " is out of range [" + formatReal(p.minValue) + ", " +
                        formatReal(p.maxValue) + "]";
                return false;
            }
            if (std::string(p.name) == "polyphony" && !isValidPolyphony(static_cast<int>(v))) {
                error = "polyphony: " + text + " is not one of 8, 16, 32, 64, 128, 256";
                return false;
            }
            p.set(obj, d);
            return true;
        }
        case ParamKind::Real: {
            double v = 0.0;
            if (!parseReal(text, v)) {
                error = std::string(p.name) + ": expected a number, got \"" + text + "\"";
                return false;
            }
            if (v < p.minValue || v > p.maxValue) {
                error = std::string(p.name) + ": " + text + " is out of range [" + formatReal(p.minValue) + ", " +
                        formatReal(p.maxValue) + "]";
                return false;
            }
            p.set(obj, v);
            return true;
        }
    }
    return false;
}

template std::string formatParam<Zone>(const ZoneParam&, const Zone&);
template std::string formatParam<Layer>(const LayerParam&, const Layer&);
template std::string formatParam<Program>(const ProgramParam&, const Program&);
template bool parseParam<Zone>(const ZoneParam&, Zone&, const std::string&, std::string&);
template bool parseParam<Layer>(const LayerParam&, Layer&, const std::string&, std::string&);
template bool parseParam<Program>(const ProgramParam&, Program&, const std::string&, std::string&);

}  // namespace nd
