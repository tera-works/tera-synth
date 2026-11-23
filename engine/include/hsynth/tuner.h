#pragma once

#include <atomic>
#include <chrono>
#include <mutex>

#include "hsynth/sample_history.h"

namespace hsynth {

struct TunerReading {
    float freqHz = 0.0f, centsOff = 0.0f, midiNoteFloat = 0.0f, confidence = 0.0f;
};

class Tuner {
public:
    void prepare(float sampleRate);
    void setEnabled(bool enabled);
    bool enabled() const { return tunerEnabled_.load(std::memory_order_acquire); }
    void configure(float a4Hz, bool muteOut);
    float a4Hz() const { return a4Hz_.load(std::memory_order_relaxed); }
    bool muteOut() const { return muteOut_.load(std::memory_order_relaxed); }

    // Output callback is the sole producer. A null input ends the session.
    void pushInput(const float* left, const float* right, int frames);
    bool analyze(TunerReading& out);

private:
    static constexpr int kCapacity = 8192;
    SampleHistory<kCapacity> input_;
    std::atomic<bool> tunerEnabled_{false}, muteOut_{false};
    std::atomic<float> a4Hz_{440.0f}, rate_{48000.0f};
    bool hadInput_ = false;

    std::mutex consumerMutex_;
    uint64_t lastStamp_ = 0;
    std::chrono::steady_clock::time_point lastInput_{};
    struct Pitch { float hz = 0.0f, confidence = 0.0f; };
    Pitch recent_[3], filtered_;
    int next_ = 0, valid_ = 0;
};

}  // namespace hsynth
