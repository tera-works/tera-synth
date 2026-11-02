// HSynth engine: platform-independent real-time synthesizer core.
//
// Threading contract
//   * process() is called from exactly one real-time audio thread. It never
//     allocates, locks, or touches the file system.
//   * Every other public method may be called from any non-audio thread.
//     Events go through a lock-free queue, parameters through atomics, and
//     sample sets are swapped by pointer and retired to a garbage queue that
//     collectGarbage() frees on a non-audio thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "hsynth/common.h"
#include "hsynth/dsp/effects.h"
#include "hsynth/dsp/lfo.h"
#include "hsynth/event.h"
#include "hsynth/fx/convolver.h"
#include "hsynth/fx/rack.h"
#include "hsynth/params.h"
#include "hsynth/sample_set.h"
#include "hsynth/spsc_queue.h"
#include "hsynth/voice.h"

namespace hsynth {

enum class Quality : int { Low = 0, Normal = 1, High = 2 };

struct EngineStats {
    int activeVoices = 0;
    int peakVoices = 0;
    float cpuLoad = 0.0f;       // smoothed render time / buffer duration (0..1+)
    float cpuLoadPeak = 0.0f;   // since last reset
    float renderUsAvg = 0.0f;   // microseconds per process() call
    float renderUsPeak = 0.0f;
    uint64_t blocks = 0;
    uint32_t droppedEvents = 0;
    int lastBlockSize = 0;
    float outputPeak = 0.0f;    // since last reset
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Not real-time safe. Call before audio starts or with audio stopped.
    void prepare(float sampleRate, int maxBlockSize = kMaxBlockSize);
    float sampleRate() const { return sampleRate_; }

    // Audio thread. Renders n frames into non-interleaved buffers (overwrites).
    void process(float* left, float* right, int n);

    // ---- any non-audio thread ----
    bool sendEvent(const Event& e);
    void setParam(int id, float value);
    float getParam(int id) const;
    void setAllParams(const float* values);  // kNumParams values
    void getAllParams(float* out) const;

    // Takes ownership. Pass an empty SampleSet (or nullptr) to unload.
    void setSampleSet(int layer, std::unique_ptr<SampleSet> set);
    std::string sampleSetName(int layer) const;
    std::string sampleSetPath(int layer) const;
    // Free sample sets retired by the audio thread.
    void collectGarbage();

    void setMaxVoices(int n);
    int maxVoices() const { return maxVoices_.load(); }
    void setQuality(Quality q) { quality_.store((int)q); }
    Quality quality() const { return (Quality)quality_.load(); }

    // ---- effect rack ----
    // User impulse response for the cabinet (target 0) or IR reverb (target
    // 1); nullptr unloads. Build it with loadImpulseResponse() (fx/ir_load.h).
    void setFxImpulse(int target, std::unique_ptr<fx::ConvIr> ir);
    std::string fxImpulseName(int target) const;
    std::string fxImpulsePath(int target) const;
    void looperCommand(fx::LooperCmd c) {
        Event e;
        e.type = EventType::Looper;
        e.controller = (uint16_t)c;
        sendEvent(e);
    }
    fx::LooperStatus looperStatus() const { return rack_.looperStatus(); }
    // Blocks in processing order; returns count (<= fx::kFxSlots).
    int fxChain(int* blocks) const { return rack_.chainSnapshot(blocks); }

    // Audio thread, before process(): external input for the vocoder for the
    // next process() call (n frames, or nullptr). The pointers are not kept.
    void setInputBuffer(const float* left, const float* right) { inL_ = left; inR_ = right; }

    EngineStats stats() const;
    void resetPeakStats();

private:
    void handleEvent(const Event& e);
    void noteOn(const Event& e);
    void noteOff(const Event& e);
    void releaseSustained();
    Voice* allocateVoice();
    void updateEffects();

    float sampleRate_ = 48000.0f;
    bool prepared_ = false;

    // parameters
    std::unique_ptr<std::atomic<float>[]> sharedParams_;
    std::atomic<uint32_t> paramVersion_{1};
    uint32_t seenParamVersion_ = 0;
    float p_[P::kNumParams] = {};

    // events
    SpscQueue<Event> events_{2048};
    std::mutex producerMutex_;
    std::atomic<uint32_t> droppedEvents_{0};

    // sample sets
    std::atomic<SampleSet*> pendingSets_[kNumLayers];
    SampleSet* currentSets_[kNumLayers] = {};
    SpscQueue<SampleSet*> retired_{64};
    std::mutex loaderMutex_;
    mutable std::mutex nameMutex_;
    std::string setNames_[kNumLayers];
    std::string setPaths_[kNumLayers];

    // voices
    Voice voices_[kMaxVoices + kVoicePoolExtra];
    std::atomic<int> maxVoices_{32};
    std::atomic<int> quality_{(int)Quality::Normal};
    uint64_t ageCounter_ = 0;
    Rng rng_{0xC0FFEEu};

    // channel state
    VoiceContext ctx_;
    float pitchBend_ = 0.0f;
    bool sustain_ = false;
    Lfo lfo2_;

    // effects
    fx::FxRack rack_;
    Limiter limiter_;
    int effectsQuality_ = -1;
    const float* inL_ = nullptr;
    const float* inR_ = nullptr;
    // pending user IR: nullptr = nothing pending, &unloadIr_ = unload request
    std::atomic<fx::ConvIr*> pendingIr_[2];
    fx::ConvIr unloadIr_;
    fx::ConvIr* currentIr_[2] = {};
    SpscQueue<fx::ConvIr*> retiredIr_{16};
    std::string irNames_[2], irPaths_[2];
    Smoother masterGain_;

    // stats (written by audio thread)
    std::atomic<int> statActive_{0}, statPeakVoices_{0}, statBlockSize_{0};
    std::atomic<float> statLoad_{0}, statLoadPeak_{0}, statUs_{0}, statUsPeak_{0}, statOutPeak_{0};
    std::atomic<uint64_t> statBlocks_{0};
};

}  // namespace hsynth
