// Phase-modulation (DX-style FM) operator stack.
// Algorithms are data: each operator lists a bitmask of the operators that
// modulate it, plus a carrier mask. Modulators always have a higher index than
// the operator they modulate, so evaluation runs from the last operator down.
// To support 6 or 8 operators, raise P::kFmOps and add tables.
#pragma once

#include <cstdint>

#include "hsynth/common.h"
#include "hsynth/dsp/envelope.h"
#include "hsynth/dsp/oscillators.h"
#include "hsynth/params.h"

namespace hsynth {

struct FmAlgorithm {
    uint8_t modulators[P::kFmOps];  // bit i set => op i modulates this op
    uint8_t carriers;               // bit i set => op i is audible
    int8_t feedbackOp;              // operator with self-feedback
};

constexpr int kNumFmAlgorithms = 8;

inline const FmAlgorithm& fmAlgorithm(int index) {
    static const FmAlgorithm table[kNumFmAlgorithms] = {
        {{0b0010, 0b0100, 0b1000, 0}, 0b0001, 3},  // 4>3>2>1
        {{0b0010, 0b1100, 0, 0}, 0b0001, 3},       // (4+3)>2>1
        {{0b0110, 0, 0b1000, 0}, 0b0001, 3},       // 4>3>1 + 2>1
        {{0b0010, 0, 0b1000, 0}, 0b0101, 3},       // 4>3 + 2>1
        {{0b1000, 0b1000, 0b1000, 0}, 0b0111, 3},  // 4>(3,2,1)
        {{0, 0b0100, 0b1000, 0}, 0b0011, 3},       // 4>3>2 + 1
        {{0b1000, 0, 0, 0}, 0b0111, 3},            // 4>1 + 2 + 3
        {{0, 0, 0, 0}, 0b1111, 3},                 // additive
    };
    if (index < 0) index = 0;
    if (index >= kNumFmAlgorithms) index = kNumFmAlgorithms - 1;
    return table[index];
}

inline float fmRatio(float coarse) { return coarse < 0.5f ? 0.5f : coarse; }

class FmVoice {
public:
    void setSampleRate(float sr) {
        for (auto& e : env_) e.setSampleRate(sr);
    }

    void noteOn(float velocity) {
        velocity_ = velocity;
        for (int i = 0; i < P::kFmOps; ++i) {
            phase_[i] = 0;
            env_[i].reset();
            env_[i].gate(true);
            lastCoarse_[i] = lastDetune_[i] = -1e9f;
        }
        fb1_ = fb2_ = 0.0f;
    }
    void noteOff() {
        for (auto& e : env_) e.gate(false);
    }
    void kill(float seconds) {
        for (auto& e : env_) e.kill(seconds);
    }

    // Update per-control-block settings from the parameter array.
    void control(const float* p, int layerIdx) {
        algo_ = &fmAlgorithm((int)p[P::layer(layerIdx, P::FmAlgo)]);
        for (int i = 0; i < P::kFmOps; ++i) {
            const float coarse = p[P::fmOp(layerIdx, i, P::OpRatio)];
            const float det = p[P::fmOp(layerIdx, i, P::OpDetune)];
            if (coarse != lastCoarse_[i] || det != lastDetune_[i]) {
                lastCoarse_[i] = coarse;
                lastDetune_[i] = det;
                ratio_[i] = fmRatio(coarse) * std::exp2(det / 1200.0f);
            }
            const float vs = p[P::fmOp(layerIdx, i, P::OpVelSens)];
            level_[i] = p[P::fmOp(layerIdx, i, P::OpLevel)] * (1.0f - vs + vs * velocity_);
            env_[i].set(p[P::fmOp(layerIdx, i, P::OpAttack)], p[P::fmOp(layerIdx, i, P::OpDecay)],
                        p[P::fmOp(layerIdx, i, P::OpSustain)], p[P::fmOp(layerIdx, i, P::OpRelease)]);
        }
        int carriers = 0;
        for (int i = 0; i < P::kFmOps; ++i)
            if (algo_->carriers & (1 << i)) ++carriers;
        carrierGain_ = carriers > 0 ? 1.0f / (float)carriers : 0.0f;
    }

    // Render n <= kControlBlock samples, adding into out. Operator envelopes
    // run at control rate and are interpolated; operators are processed one at
    // a time over the block (modulators first). Phases are 32-bit integers
    // (one cycle = 2^32) so wrapping costs nothing.
    // dtStart/dtEnd: base frequency / sampleRate ramp, index: modulation depth
    // in cycles, feedback 0..1.
    void render(float* out, int n, float dtStart, float dtEnd, float index, float feedback) {
        float buf[P::kFmOps][kControlBlock];
        float mod[kControlBlock];
        const float invN = 1.0f / (float)n;
        const float fbAmt = feedback * 0.35f;
        constexpr float kTwo32 = 4294967296.0f;
        for (int i = P::kFmOps - 1; i >= 0; --i) {
            const float e0 = env_[i].level();
            const float e1 = env_[i].processBlock(n);
            const float lvl = level_[i];
            float* o = buf[i];
            const float inc0 = dtStart * ratio_[i];
            const float incStep = (dtEnd - dtStart) * ratio_[i] * invN;
            if (lvl <= 0.0f || (e0 <= 0.0f && e1 <= 0.0f)) {
                for (int s = 0; s < n; ++s) o[s] = 0.0f;
                phase_[i] += (uint32_t)(int64_t)((inc0 + incStep * (float)n * 0.5f) * (float)n * kTwo32);
                continue;
            }
            const uint8_t mask = algo_->modulators[i];
            for (int s = 0; s < n; ++s) mod[s] = 0.0f;
            for (int m = i + 1; m < P::kFmOps; ++m)
                if (mask & (1 << m))
                    for (int s = 0; s < n; ++s) mod[s] += buf[m][s];
            const float eStep = (e1 - e0) * invN;
            float e = e0;
            uint32_t ph = phase_[i];
            float inc = inc0;
            if (i == algo_->feedbackOp) {
                float f1 = fb1_, f2 = fb2_;
                for (int s = 0; s < n; ++s) {
                    e += eStep;
                    const float v = sineLookupU32(ph + cyclesToPhase(mod[s] * index + fbAmt * (f1 + f2))) * e * lvl;
                    f2 = f1;
                    f1 = v;
                    o[s] = v;
                    inc += incStep;
                    ph += (uint32_t)(inc * kTwo32);
                }
                fb1_ = f1;
                fb2_ = f2;
            } else {
                for (int s = 0; s < n; ++s) {
                    e += eStep;
                    o[s] = sineLookupU32(ph + cyclesToPhase(mod[s] * index)) * e * lvl;
                    inc += incStep;
                    ph += (uint32_t)(inc * kTwo32);
                }
            }
            phase_[i] = ph;
            if (algo_->carriers & (1 << i))
                for (int s = 0; s < n; ++s) out[s] += o[s] * carrierGain_;
        }
    }

    // True while any carrier envelope is still running.
    bool active() const {
        for (int i = 0; i < P::kFmOps; ++i)
            if ((algo_->carriers & (1 << i)) && env_[i].active()) return true;
        return false;
    }

private:
    const FmAlgorithm* algo_ = &fmAlgorithm(0);
    Adsr env_[P::kFmOps];
    uint32_t phase_[P::kFmOps] = {};
    float ratio_[P::kFmOps] = {1, 1, 1, 1};
    float lastCoarse_[P::kFmOps] = {}, lastDetune_[P::kFmOps] = {};
    float level_[P::kFmOps] = {};
    float velocity_ = 1.0f;
    float carrierGain_ = 1.0f;
    float fb1_ = 0.0f, fb2_ = 0.0f;
};
}  // namespace hsynth
