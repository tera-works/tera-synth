// Band-limited virtual-analog oscillators (polyBLEP / polyBLAMP) and a shared
// sine lookup table.
#pragma once

#include <cmath>

#include "hsynth/common.h"
#include "hsynth/dsp/simd.h"
#include "hsynth/params.h"

namespace hsynth {

// 4096-point sine table with linear interpolation (phase in cycles). The table
// is a plain global so lookups carry no static-initialisation guard.
constexpr int kSineTableSize = 4096;
extern const float* const g_sineTable;  // kSineTableSize + 1 entries

inline float sineLookup(float phase) {
    phase -= fastFloor(phase);
    const float idx = phase * (float)kSineTableSize;
    const int i0 = (int)idx;
    const float f = idx - (float)i0;
    const int i = i0 & (kSineTableSize - 1);  // guards phase rounding to exactly 1.0
    return g_sineTable[i] + f * (g_sineTable[i + 1] - g_sineTable[i]);
}

// Integer-phase variant: phase is a 32-bit fraction of a cycle (wraps for free).
inline float sineLookupU32(uint32_t phase) {
    const uint32_t i = phase >> 20;  // 12-bit table index
    const float f = (float)(phase & 0xFFFFFu) * (1.0f / 1048576.0f);
    return g_sineTable[i] + f * (g_sineTable[i + 1] - g_sineTable[i]);
}

// Convert cycles (any sign, |x| < 2^31) to a 32-bit phase offset.
inline uint32_t cyclesToPhase(float cycles) { return (uint32_t)(int64_t)(cycles * 4294967296.0f); }
inline float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

inline float polyBlamp(float t, float dt) {
    if (t < dt) {
        t = t / dt - 1.0f;
        return -(1.0f / 3.0f) * t * t * t;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt + 1.0f;
        return (1.0f / 3.0f) * t * t * t;
    }
    return 0.0f;
}

class VaOsc {
public:
    void reset(float phase) { phase_ = phase - fastFloor(phase); }

    // dt = frequency / sampleRate (cycles per sample), pw = pulse width 0..1
    float tick(VaWave wave, float dt, float pw) {
        dt = clampf(dt, 0.0f, 0.45f);
        const float t = phase_;
        float v;
        switch (wave) {
            case VaWave::Saw:
                v = 2.0f * t - 1.0f - polyBlep(t, dt);
                break;
            case VaWave::Square:
                pw = 0.5f;
                [[fallthrough]];
            case VaWave::Pulse: {
                v = t < pw ? 1.0f : -1.0f;
                v += polyBlep(t, dt);
                float t2 = t + 1.0f - pw;
                t2 -= fastFloor(t2);
                v -= polyBlep(t2, dt);
                // remove DC of asymmetric pulse
                v -= 2.0f * pw - 1.0f;
                break;
            }
            case VaWave::Triangle: {
                v = 1.0f - 4.0f * std::fabs(t - 0.5f);  // -1 at phase 0, +1 at 0.5
                float t2 = t + 0.5f;
                t2 -= fastFloor(t2);
                v += 4.0f * dt * (polyBlamp(t, dt) - polyBlamp(t2, dt));
                break;
            }
            case VaWave::Sine:
            default:
                v = sineLookup(t);
                break;
        }
        phase_ += dt;
        wrapped_ = phase_ >= 1.0f;
        if (wrapped_) {
            phase_ -= 1.0f;
            wrapFrac_ = dt > 0.0f ? phase_ / dt : 0.0f;  // fraction of this sample after the wrap
        }
        return v;
    }
    // For oscillators that do not drive hard sync.
    float tickSaw(float dt) { return tickSawPhase(phase_, dt); }

    static float tickSawPhase(float& phase, float dt) {
        dt = clampf(dt, 0.0f, 0.45f);
        const float t = phase;
        const float v = 2.0f * t - 1.0f - polyBlep(t, dt);
        phase = t + dt;
        if (phase >= 1.0f) phase -= 1.0f;
        return v;
    }

    // Four independent supersaw sides. Keep the scalar accumulation order.
    static void tickSaw4(float* phases, const float* ratios, float dt, float gain, float* out) {
#if defined(HSYNTH_SIMD_NEON) && defined(__aarch64__)
        const float32x4_t one = vdupq_n_f32(1.0f);
        const float32x4_t zero = vdupq_n_f32(0.0f);
        const float32x4_t inc = vmaxq_f32(zero, vminq_f32(vdupq_n_f32(0.45f), vmulq_n_f32(vld1q_f32(ratios), dt)));
        const auto phase = simd::load(phases);
        const float32x4_t t = phase.v;
        const uint32x4_t lo = vcltq_f32(t, inc);
        const uint32x4_t hi = vcgtq_f32(t, vsubq_f32(one, inc));
        const uint32x4_t edge = vorrq_u32(lo, hi);
        float32x4_t blep = zero;
        if (vmaxvq_u32(edge)) {
            const float32x4_t num = vbslq_f32(hi, vsubq_f32(t, one), t);
            const float32x4_t x = vdivq_f32(num, vbslq_f32(edge, inc, one));
            const float32x4_t sq = vmulq_f32(x, x);
            const float32x4_t a = vsubq_f32(vsubq_f32(vaddq_f32(x, x), sq), one);
            const float32x4_t b = vaddq_f32(vaddq_f32(vaddq_f32(sq, x), x), one);
            blep = vbslq_f32(edge, vbslq_f32(lo, a, b), zero);
        }
        const float32x4_t v = vsubq_f32(vsubq_f32(vaddq_f32(t, t), one), blep);
        vst1q_f32(out, vmulq_n_f32(v, gain));
        const auto next = phase + simd::f4{inc};
        simd::store(phases, simd::subIfGe(next, next, simd::f4{one}));
#else
        for (int i = 0; i < 4; ++i) out[i] = tickSawPhase(phases[i], dt * ratios[i]) * gain;
#endif
    }

    // hard sync: did the last tick wrap, and how far past the wrap (in samples)
    bool wrapped() const { return wrapped_; }
    float wrapFrac() const { return wrapFrac_; }

private:
    float phase_ = 0.0f;
    bool wrapped_ = false;
    float wrapFrac_ = 0.0f;
};

}  // namespace hsynth
