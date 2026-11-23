#pragma once

#include "hsynth/common.h"

namespace hsynth {

class PianoBusState {
public:
    void pedal(float value, float amount, float sr, int offset, bool audible = true);
    void process(float* left, float* right, int frames, const float* params, float sr);
    bool busy() const {
        return bursts_[0].remaining > 0 || bursts_[1].remaining > 0 ||
               std::fabs(left_) > 1e-5f || std::fabs(right_) > 1e-5f;
    }
    void silence() { bursts_[0] = bursts_[1] = {}; }

private:
    struct Burst {
        int remaining = 0, duration = 0, delay = 0;
        float level = 0, phase = 0, lp = 0, hp = 0;
    } bursts_[2];
    Rng rng_{0x493c42u};
    bool down_ = false;
    float lid_ = 1.0f, lidCoef_ = 1.0f, left_ = 0.0f, right_ = 0.0f, detector_ = 0.0f;
};

}  // namespace hsynth
