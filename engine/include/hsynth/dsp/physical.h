#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace hsynth {

struct alignas(128) PhysicalLayerState {
    struct Mode { float y1, y2, a1, a2, gain, excite; };
    struct Waveguide {
        float delay, feedback, loss, dispersion, position, pickup, bow, lossState;
        float allpass, previous, current, exciteState, delayTarget, delayStep;
    };
    union {
        Mode modes[4] = {};
        Waveguide string;
    };
    float excitation = 0, releaseGain = 1, body = 0;
    uint32_t rng = 1, age = 0;
    uint16_t slot = 0xffff, write = 0, valid = 0;
    uint8_t model = 0, exciter = 0, stride = 1, phase = 0, velocity = 0;
};
static_assert(sizeof(PhysicalLayerState) == 128 && alignof(PhysicalLayerState) == 128);

int physicalModes(const float* params, int layer, float* out, int capacity);

class PhysicalPool {
public:
    void prepare(int layers);
    PhysicalLayerState* layer(int voiceSlot, int layerIndex);
    void start(PhysicalLayerState& state, const float* params, int layer, float hz, float sr,
               float velocity, uint32_t seed);
    void stop(PhysicalLayerState& state);
    void render(PhysicalLayerState& state, const float* params, int layer, float hz, float sr,
                bool released, float* out, int frames);
private:
    static constexpr int kSlots = 256, kDelay = 2048;
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    std::unique_ptr<PhysicalLayerState[]> states_;
    std::unique_ptr<float[]> delay_;
    std::atomic<uint32_t> free_[kSlots / 32]{};
};

}  // namespace hsynth
