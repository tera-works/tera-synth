#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <mutex>

#include "hsynth/fx/dsp.h"

namespace hsynth {

class Json;

struct MasterStageConfig {
    struct EqBand {
        float freq;
        float gainDb = 0.0f;
        float q = 0.70710678f;
        bool enabled = false;
    };
    struct Compressor {
        float thresholdDb = -18.0f;
        float ratio = 2.0f;
        float attackMs = 10.0f;
        float releaseMs = 150.0f;
        float kneeDb = 6.0f;
        float makeupDb = 0.0f;
    };

    bool bypass = true;
    bool eqEnabled = false;
    bool compEnabled = false;
    std::array<EqBand, 5> bands{{{80.0f}, {250.0f}, {1000.0f}, {4000.0f}, {12000.0f}}};
    Compressor comp;
};

Json masterStageToJson(const MasterStageConfig& config);
// The input may omit fields to keep their current values. When present, bands
// is a five-element array and each element may update any of its fields.
bool masterStageFromJson(const Json& json, MasterStageConfig& config);

class MasterStage {
public:
    void setConfig(const MasterStageConfig& config);
    MasterStageConfig getConfig() const;
    bool updateConfig(const std::function<bool(MasterStageConfig&)>& edit);
    void prepare(float sampleRate);
    bool sync();  // audio block boundary, including during silent idle
    void process(float* left, float* right, int frames);
    bool busy() const { return fadeRemaining_ > 0 || tail_; }
    float gainReductionDb() const { return grDb_.load(std::memory_order_relaxed); }

private:
    struct Path {
        std::array<fx::StereoBiquad, 5> bands;
        unsigned mask = 0;
        bool compOn = false;
        MasterStageConfig::Compressor comp;
        float attack = 0.0f, release = 0.0f, reduction = 0.0f;
        void configure(const MasterStageConfig& config, float sampleRate);
        void process(float& left, float& right);
        bool hasTail() const;
    };

    void publish(const MasterStageConfig& config);

    // The host writer, exchange slot and audio reader each own one buffer.
    static constexpr unsigned kDirty = 4;
    std::array<MasterStageConfig, 3> mailbox_{};
    mutable std::mutex writerMutex_;
    unsigned writer_ = 0, reader_ = 2;
    std::atomic<unsigned> middle_{1};
    MasterStageConfig hostConfig_;

    Path paths_[2];
    int active_ = 0, fadeLength_ = 1, fadeRemaining_ = 0;
    float sampleRate_ = 48000.0f;
    bool tail_ = false;
    std::atomic<float> grDb_{0.0f};
};

}  // namespace hsynth
