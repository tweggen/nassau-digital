#pragma once

// nd_sample.h - an immutable PCM buffer (DESIGN §2, §6).
//
// The core never sees where audio came from: the host hands it a Sample and
// says which layer uses it (Instrument::setSample). A Sample is never changed
// after it is made; new audio is a new Sample with a higher generation
// (DESIGN §6.1).
//
// Storage is interleaved float with kPad zero frames before the first frame
// and after the last, so an interpolator can read its taps around any frame
// of the sample without a bounds test.

#include <cstdint>
#include <memory>
#include <vector>

namespace nd {

class Sample {
public:
    /// Zero frames before and after the audio: enough for the widest
    /// interpolator's taps (nd_interp.h).
    static constexpr int kPad = 32;

    /// Makes a sample from interleaved frames (1 or 2 channels). Allocates;
    /// call it off the audio thread. Returns nullptr for an invalid shape.
    static std::shared_ptr<const Sample> fromInterleaved(int channels, double sampleRate, const float* data,
                                                         int64_t frames, uint64_t generation = 0);
    /// Convenience: a mono sample from one channel of frames.
    static std::shared_ptr<const Sample> mono(double sampleRate, const std::vector<float>& frames,
                                              uint64_t generation = 0);
    /// Convenience: a stereo sample from two equally long channels.
    static std::shared_ptr<const Sample> stereo(double sampleRate, const std::vector<float>& left,
                                                const std::vector<float>& right, uint64_t generation = 0);

    int channels() const { return channels_; }
    double sampleRate() const { return sampleRate_; }
    int64_t frames() const { return frames_; }
    uint64_t generation() const { return generation_; }

    /// Pointer to frame i (interleaved), valid for -kPad <= i < frames + kPad.
    const float* frame(int64_t i) const { return data_.data() + (i + kPad) * channels_; }

private:
    Sample() = default;
    int channels_ = 1;
    double sampleRate_ = 48000.0;
    int64_t frames_ = 0;
    uint64_t generation_ = 0;
    std::vector<float> data_;
};

inline std::shared_ptr<const Sample> Sample::fromInterleaved(int channels, double sampleRate, const float* data,
                                                             int64_t frames, uint64_t generation) {
    if ((channels != 1 && channels != 2) || !(sampleRate > 0.0) || frames < 0 || (frames > 0 && !data))
        return nullptr;
    std::shared_ptr<Sample> s(new Sample());
    s->channels_ = channels;
    s->sampleRate_ = sampleRate;
    s->frames_ = frames;
    s->generation_ = generation;
    s->data_.assign(static_cast<size_t>((frames + 2 * kPad) * channels), 0.0f);
    for (int64_t i = 0; i < frames * channels; ++i)
        s->data_[static_cast<size_t>(kPad * channels + i)] = data[i];
    return s;
}

inline std::shared_ptr<const Sample> Sample::mono(double sampleRate, const std::vector<float>& frames,
                                                  uint64_t generation) {
    return fromInterleaved(1, sampleRate, frames.data(), static_cast<int64_t>(frames.size()), generation);
}

inline std::shared_ptr<const Sample> Sample::stereo(double sampleRate, const std::vector<float>& left,
                                                    const std::vector<float>& right, uint64_t generation) {
    if (left.size() != right.size()) return nullptr;
    std::vector<float> inter(left.size() * 2);
    for (size_t i = 0; i < left.size(); ++i) {
        inter[2 * i] = left[i];
        inter[2 * i + 1] = right[i];
    }
    return fromInterleaved(2, sampleRate, inter.data(), static_cast<int64_t>(left.size()), generation);
}

}  // namespace nd
