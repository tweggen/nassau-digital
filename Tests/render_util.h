#pragma once

// render_util.h - driving the instrument from tests: a program in, notes at
// absolute frames in, stereo audio out, through the public API only (R6).

#include "nd_instrument.h"
#include "nd_program.h"

#include <memory>
#include <vector>

namespace ndtest {

struct Note {
    int64_t frame;
    nd::Event::Type type;
    int key;
    int value;
};

inline Note on(int64_t frame, int key, int vel = 127) { return {frame, nd::Event::NoteOn, key, vel}; }
inline Note off(int64_t frame, int key) { return {frame, nd::Event::NoteOff, key, 0}; }
inline Note wheel(int64_t frame, int value) { return {frame, nd::Event::ModWheel, 0, value}; }

struct Stereo {
    std::vector<float> l, r;
};

/// Renders `frames` frames starting at timeline frame `start`, in blocks of
/// `block`, feeding each note in the block it falls in. Notes must be sorted.
inline Stereo render(nd::Instrument& inst, const std::vector<Note>& notes, int64_t frames, int block = 256,
                     int64_t start = 0) {
    Stereo out;
    out.l.assign(static_cast<size_t>(frames), 0.0f);
    out.r.assign(static_cast<size_t>(frames), 0.0f);
    std::vector<nd::Event> ev;
    ev.reserve(notes.size());
    size_t next = 0;
    while (next < notes.size() && notes[next].frame < start) ++next;
    for (int64_t b = 0; b < frames; b += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, frames - b));
        ev.clear();
        while (next < notes.size() && notes[next].frame < start + b + n) {
            nd::Event e;
            e.offset = static_cast<int32_t>(notes[next].frame - start - b);
            e.type = notes[next].type;
            e.key = static_cast<uint8_t>(notes[next].key);
            e.value = static_cast<uint8_t>(notes[next].value);
            ev.push_back(e);
            ++next;
        }
        inst.process(start + b, ev.data(), static_cast<int>(ev.size()), out.l.data() + b, out.r.data() + b, n);
    }
    return out;
}

/// A zone over every key and velocity with one layer, mode as given.
inline nd::Zone fullZone(nd::PlayMode mode = nd::PlayMode::Gated, int rootKey = 60) {
    nd::Zone z;
    z.mode = mode;
    z.rootKey = rootKey;
    z.layers.push_back(nd::Layer{});
    return z;
}

/// Installs `prog` (by copy) on the instrument.
inline void install(nd::Instrument& inst, const nd::Program& prog) {
    inst.setProgram(std::make_shared<const nd::Program>(prog));
}

}  // namespace ndtest
