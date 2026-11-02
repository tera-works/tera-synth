// HSynth engine: platform-independent real-time synthesizer core.
//
// Threading contract
//   * process() is called from exactly one real-time audio thread. It never
//     allocates, locks, or touches the file system.
//   * Every other public method may be called from any non-audio thread.
//     Events go through a lock-free queue, parameters through atomics, and
//     sample sets are swapped by pointer and retired to a garbage queue that
//     collectGarbage() frees on a non-audio thread.
//
// Multi-timbral operation
//   The engine has kNumParts (16) parts. A part is a complete patch: its own
//   copy of the part parameters (isPartParam(): voice, layers, mod matrix) and
//   its own three sample sets / user wavetables. The effect rack, looper,
//   master volume and limiter are global and shared by all parts; the voice
//   pool is shared too (fair stealing across parts).
//   * Single-part mode (default): only part 0 ("part 1" in UIs) plays, every
//     event goes to it regardless of its MIDI channel and the mix settings are
//     not applied, i.e. the engine behaves exactly like the single-timbral one.
//   * Multi-timbral mode: events with a MIDI channel (Event::channel) go to
//     every part whose Mix::Channel matches; events with kOmniChannel go to the
//     selected part. Each part is mixed with volume / pan / mute, filtered by
//     key range and transpose, and split by FX send between the effect rack
//     and a dry bus that is added after the rack (before master + limiter).
//   The legacy single-part API (setParam, setSampleSet, ...) addresses part 0.
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
    void setSampleSet(int layer, std::unique_ptr<SampleSet> set) { setPartSampleSet(0, layer, std::move(set)); }
    std::string sampleSetName(int layer) const { return partSampleSetName(0, layer); }
    std::string sampleSetPath(int layer) const { return partSampleSetPath(0, layer); }
    // Free sample sets retired by the audio thread.
    void collectGarbage();

    // ---- multi-timbral (any non-audio thread) ----
    // Switching modes releases nothing in part 0; voices of other parts are
    // stopped when going back to single-part mode.
    void setMultiTimbral(bool on) { multi_.store(on); }
    bool multiTimbral() const { return multi_.load(); }
    // Part that receives events without a MIDI channel (kOmniChannel) in
    // multi-timbral mode (touch keyboard / host UI).
    void selectPart(int part) { selectedPart_.store(std::max(0, std::min(kNumParts - 1, part))); }
    int selectedPart() const { return selectedPart_.load(); }
    // Parameters of part N. For ids that are not part parameters (effect rack,
    // master volume, limiter) these forward to the global setParam/getParam.
    void setPartParam(int part, int id, float value);
    float getPartParam(int part, int id) const;
    // kNumParams-sized arrays. set: only the part parameters are used. get:
    // the part parameters of `part`, everything else from the global array.
    void setPartParams(int part, const float* values);
    void getPartParams(int part, float* out) const;
    // Only the non-part parameters (rack, master volume, limiter).
    void setGlobalParams(const float* values);
    // Mix settings (Mix::Volume ...); used in multi-timbral mode only.
    void setPartMix(int part, int id, float value);
    float getPartMix(int part, int id) const;
    void setPartSampleSet(int part, int layer, std::unique_ptr<SampleSet> set);
    std::string partSampleSetName(int part, int layer) const;
    std::string partSampleSetPath(int part, int layer) const;
    int partActiveVoices(int part) const {
        return part >= 0 && part < kNumParts ? statPartVoices_[part].load(std::memory_order_relaxed) : 0;
    }

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
    // Audio-thread state of one part.
    struct PartState {
        const float* p = nullptr;  // parameter array (part 0: p_)
        VoiceContext ctx;
        float pitchBend = 0.0f;
        bool sustain = false;
        Lfo lfo2;
        Rng rng;                   // lfo2 S&H of parts 1..15 (part 0 uses the engine rng)
        float ccVolume = 1.0f;     // CC7 (multi mode)
        float ccPan = 0.0f;        // CC10 as -1..1 offset (multi mode)
        float gain[4] = {};        // smoothed send L/R, dry L/R
        float target[4] = {};
    };

    void handleEvent(const Event& e);
    void partEvent(int part, const Event& e);
    void noteOn(int part, const Event& e);
    // part < 0: any part (note-id matched events in multi mode)
    void noteOff(const Event& e, int part);
    void perNote(const Event& e, int part);
    void releaseSustained(int part);
    Voice* allocateVoice(int part);
    void updateEffects();
    void updatePartTargets();
    void processMulti(float* L, float* R, int n);
    void swapSampleSet(int part, int layer, SampleSet* s);

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

    // parts 1..15 (part 0 lives in sharedParams_ / p_)
    std::unique_ptr<std::atomic<float>[]> partShared_;     // (kNumParts-1) * kPartParams
    std::atomic<uint32_t> partVersion_[kNumParts];
    uint32_t seenPartVersion_[kNumParts] = {};
    std::unique_ptr<float[]> partP_;                       // audio-thread copy
    std::atomic<float> mixShared_[kNumParts][Mix::Count];
    std::atomic<uint32_t> mixVersion_{1};
    uint32_t seenMixVersion_ = 0;
    float mix_[kNumParts][Mix::Count] = {};
    std::atomic<bool> multi_{false};
    std::atomic<int> selectedPart_{0};
    bool multiActive_ = false;  // audio thread
    PartState parts_[kNumParts];
    float partBuf_[kNumParts][2][kControlBlock] = {};
    float dryL_[kMaxBlockSize] = {}, dryR_[kMaxBlockSize] = {};
    float gainStep_ = 0.0f;  // per sample

    // sample sets
    std::atomic<SampleSet*> pendingSets_[kNumParts][kNumLayers];
    SampleSet* currentSets_[kNumParts][kNumLayers] = {};
    SpscQueue<SampleSet*> retired_{128};
    std::mutex loaderMutex_;
    mutable std::mutex nameMutex_;
    std::string setNames_[kNumParts][kNumLayers];
    std::string setPaths_[kNumParts][kNumLayers];

    // voices
    Voice voices_[kMaxVoices + kVoicePoolExtra];
    std::atomic<int> maxVoices_{32};
    std::atomic<int> quality_{(int)Quality::Normal};
    uint64_t ageCounter_ = 0;
    Rng rng_{0xC0FFEEu};

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
    std::atomic<int> statPartVoices_[kNumParts];
};

}  // namespace hsynth
