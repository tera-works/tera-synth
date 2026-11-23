#pragma once

#include <atomic>

#include "hsynth/sample_history.h"

namespace hsynth {

class AudioTap {
public:
    static constexpr int kCapacity = 4096;
    static constexpr int kFftSize = 1024;

    void prepare(float sampleRate);
    void setEnabled(bool enabled);
    bool enabled() const { return observerActive_.load(std::memory_order_acquire); }
    void capture(const float* left, const float* right, int frames, bool silentIdle = false);
    float readFrame(float* wave, int waveLen, float* spec, int specLen) const;

private:
    SampleHistory<kCapacity> history_;
    std::atomic<bool> observerActive_{false}, silent_{true};
    std::atomic<float> sampleRate_{48000.0f};
};

}  // namespace hsynth
