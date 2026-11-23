#include "hsynth/dsp/physical.h"
#include "hsynth/common.h"
#include "hsynth/params.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace hsynth {
namespace {
constexpr float kTau = 6.283185307f;
constexpr float kT60 = 6.907755279f;
constexpr int kString = 0, kTine = 2, kReed = 3, kHammer = 2, kBow = 3;
constexpr uint16_t kNoDelay = 0xffff;

struct ModeSpec { float ratio, gain, decay; };
ModeSpec modeSpec(const float* p, int model, int index) {
    static constexpr float ratios[][4] = {{1, 2, 3, 4}, {1, 2.76f, 5.40f, 8.93f},
                                           {1, 2.01f, 4.06f, 7.2f}, {1, 3, 5, 7}};
    const float i = static_cast<float>(index);
    const float ratio = (index < 4 ? ratios[model][index] : i + 1.0f) *
                        (1.0f + p[P::PhysInharm] * i * i * 0.004f);
    const float shape = index == 0 ? 1.0f : std::sin(3.14159265f * p[P::PhysPos] * ratio);
    const float pickup = index == 0 ? 1.0f : std::sin(3.14159265f * p[P::PhysPickup] * ratio);
    const float gain = shape * pickup * (0.35f + 0.65f * p[P::PhysBright]) /
                       (1.0f + i * (1.4f - p[P::PhysBright]));
    const float decay = p[P::PhysDecay] / (1.0f + i * (0.15f + 1.5f * p[P::PhysDamp]));
    return {ratio, gain, decay};
}

float bounded(float x) { return std::isfinite(x) ? clampf(x, -4.0f, 4.0f) : 0.0f; }
float softClip(float x) { return x / (1.0f + std::fabs(x)); }

float noise(uint32_t& rng) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<float>(rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

float excitation(PhysicalLayerState& s, const float* p, float sr, bool released, float motion) {
    const float velocity = s.velocity * (1.0f / 255.0f);
    if (s.exciter == kBow) {
        if (released) return 0.0f;
        const float speed = 0.12f + 0.34f * velocity;
        const float slip = speed - motion;
        const float pressure = p[P::PhysBowPress];
        const float friction = slip / (1.0f + (3.0f + 14.0f * pressure) * slip * slip);
        return pressure * (0.09f * friction + 0.0015f * noise(s.rng));
    }
    const bool hammer = s.exciter == kHammer || s.model == kTine;
    const float duration = (hammer ? 0.006f : s.exciter == 1 ? 0.021f : 0.010f) *
                           (1.5f - p[P::PhysHardness] * 0.85f);
    const float pos = static_cast<float>(s.age) / (sr * duration);
    if (pos >= 1.0f || released) return 0.0f;
    const float window = 4.0f * pos * (1.0f - pos);
    return velocity * (window * noise(s.rng) + (hammer && s.age == 0 ? 0.7f : 0.0f));
}
}  // namespace

int physicalModes(const float* params, int layer, float* out, int capacity) {
    if (!params || !out || layer < 0 || layer >= kNumLayers || capacity <= 0) return 0;
    const float* p = params + P::layer(layer, 0);
    const int model = static_cast<int>(p[P::PhysModel]);
    const int count = std::min(capacity, model == kString ? 16 : 4);
    for (int i = 0; i < count; ++i) {
        const auto m = modeSpec(p, model, i);
        out[3 * i] = m.ratio;
        out[3 * i + 1] = std::fabs(m.gain);
        out[3 * i + 2] = m.decay;
    }
    return count;
}

void PhysicalPool::prepare(int layers) {
    if (!states_) states_ = std::make_unique<PhysicalLayerState[]>(layers);
    else std::fill_n(states_.get(), layers, PhysicalLayerState{});
    if (!delay_) delay_ = std::make_unique<float[]>(kSlots * kDelay);
    for (auto& word : free_) word.store(0xffffffffu, std::memory_order_relaxed);
}

PhysicalLayerState* PhysicalPool::layer(int voiceSlot, int layerIndex) {
    return states_.get() + voiceSlot * kNumLayers + layerIndex;
}

void PhysicalPool::start(PhysicalLayerState& s, const float* params, int layer, float hz, float sr,
                         float velocity, uint32_t seed) {
    stop(s);
    s = {};
    const float* p = params + P::layer(layer, 0);
    s.model = static_cast<uint8_t>(p[P::PhysModel]);
    s.exciter = static_cast<uint8_t>(p[P::PhysExciter]);
    s.velocity = static_cast<uint8_t>(std::lround(clampf(velocity, 0.0f, 1.0f) * 255.0f));
    s.rng = seed | 1u;
    // Keep fractional interpolation safely away from one-sample loops. Extra
    // decimation fits the lowest notes in one fixed-capacity delay line.
    hz = clampf(hz, 4.0f, std::min(8000.0f, 0.449f * sr));
    s.stride = static_cast<uint8_t>(std::max(sr / hz > 8.0f ? 2 : 1,
                                           static_cast<int>(std::ceil(sr / (2044.0f * hz)))));
    if (s.model != kString || sr / (hz * s.stride) < 3.5f) return;
    for (int w = 0; w < kSlots / 32; ++w) {
        const uint32_t mask = free_[w].load(std::memory_order_relaxed);
        if (!mask) continue;
        int bit = 0;
        while (!(mask & (uint32_t{1} << bit))) ++bit;
        free_[w].fetch_and(~(uint32_t{1} << bit), std::memory_order_acquire);
        s.slot = static_cast<uint16_t>(w * 32 + bit);
        new (&s.string) PhysicalLayerState::Waveguide{};
        break;
    }
}

void PhysicalPool::stop(PhysicalLayerState& s) {
    if (s.slot == kNoDelay) return;
    free_[s.slot / 32].fetch_or(uint32_t{1} << (s.slot % 32), std::memory_order_release);
    s.slot = kNoDelay;
}

void PhysicalPool::render(PhysicalLayerState& s, const float* params, int layer, float hz, float sr,
                          bool released, float* out, int frames) {
    const float* p = params + P::layer(layer, 0);
    hz = std::isfinite(hz) ? clampf(hz, 4.0f, std::min(8000.0f, 0.449f * sr)) : 440.0f;
    const float release = released ? std::exp(-kT60 / (0.30f * sr)) : 1.0f;
    const float hardness = p[P::PhysHardness], bright = p[P::PhysBright];
    const float drive = p[P::PhysDrive];
    const float bodyCoef = std::min(0.5f, 560.0f / sr);
    if (s.slot == kNoDelay) {
        for (int i = 0; i < 4; ++i) {
            const auto spec = modeSpec(p, s.model, i);
            const float freq = spec.ratio * hz;
            auto& m = s.modes[i];
            const float radius = std::exp(-kT60 / (sr * std::max(0.025f, spec.decay)));
            const float angle = kTau * std::min(freq / sr, 0.449f);
            m.a1 = 2.0f * radius * std::cos(angle);
            m.a2 = -radius * radius;
            m.gain = spec.gain * clampf((0.449f - freq / sr) * 50.0f, 0.0f, 1.0f);
            m.excite = std::sin(angle) * (0.12f + 0.20f * hardness);
        }
        const float excCoef = 0.14f + 0.72f * hardness;
        for (int j = 0; j < frames; ++j) {
            const float e = excitation(s, p, sr, released, s.body);
            s.excitation += excCoef * (e - s.excitation);
            float y = 0.0f;
            for (auto& m : s.modes) {
                const float x = bounded(m.a1 * m.y1 + m.a2 * m.y2 + s.excitation * m.excite);
                m.y2 = m.y1;
                m.y1 = x;
                y += x * m.gain;
            }
            s.body += bodyCoef * (y - s.body);
            y += p[P::PhysBody] * s.body * 0.45f;
            if (s.model == kTine || s.model == kReed)
                y += 0.16f * softClip(y * y) * (s.model == kTine ? 1.0f : -1.0f);
            s.releaseGain *= release;
            out[j] = 0.7f * softClip(y * (1.0f + 4.0f * drive)) * s.releaseGain;
            if (s.age != UINT32_MAX) ++s.age;
        }
        return;
    }

    auto& w = s.string;
    const float srKernel = sr / s.stride;
    w.loss = 0.10f + 0.78f * (1.0f - p[P::PhysDamp]) * (0.35f + 0.65f * bright);
    w.dispersion = 0.7f * p[P::PhysDispersion] + 0.08f * p[P::PhysInharm];
    const float phaseLoss = (1.0f - w.loss) / (1.0f + w.loss);
    const float phaseDisp = w.dispersion == 0.0f ? 0.0f : (1.0f - w.dispersion) / (1.0f + w.dispersion);
    w.delayTarget = clampf(srKernel / hz - phaseLoss - phaseDisp, 2.0f, kDelay - 2.0f);
    if (s.age == 0) w.delay = w.delayTarget;
    w.delayStep = (w.delayTarget - w.delay) / frames;
    w.feedback = std::min(0.99995f, std::exp(-kT60 / (hz * p[P::PhysDecay])));
    w.position = p[P::PhysPos];
    w.pickup = p[P::PhysPickup];
    float* line = delay_.get() + s.slot * kDelay;
    auto read = [&](float delay) {
        const int d = static_cast<int>(delay);
        const float fraction = delay - d;
        const float a = d <= s.valid ? line[(s.write - d) & (kDelay - 1)] : 0.0f;
        const float b = d + 1 <= s.valid ? line[(s.write - d - 1) & (kDelay - 1)] : 0.0f;
        return a + (b - a) * fraction;
    };
    const float excCoef = std::min(0.95f, 0.12f + hardness * 0.5f + bright * 0.3f);
    for (int i = 0; i < frames; ++i) {
        w.delay += w.delayStep;
        if (s.phase == 0) {
            const float x = read(w.delay);
            w.lossState += w.loss * (x - w.lossState);
            float feed = w.lossState;
            if (w.dispersion != 0.0f) {
                feed = w.dispersion * feed + w.allpass;
                w.allpass = w.lossState - w.dispersion * feed;
            }
            const float e = excitation(s, p, sr, released, x);
            w.exciteState += excCoef * (e - w.exciteState);
            const float next = bounded(feed * w.feedback + w.exciteState * 0.46f);
            const float pick = read(std::max(1.0f, w.delay * w.position));
            const float pickup = read(std::max(1.0f, w.delay * w.pickup));
            w.previous = w.current;
            w.current = 0.72f * next + 0.38f * pickup - 0.24f * pick;
            line[s.write] = next;
            s.write = (s.write + 1) & (kDelay - 1);
            if (s.valid < kDelay - 1) ++s.valid;
        }
        const float y = s.stride == 1 ? w.current :
                        w.previous + (w.current - w.previous) * static_cast<float>(s.phase + 1) / s.stride;
        if (++s.phase >= s.stride) s.phase = 0;
        s.body += bodyCoef * (y - s.body);
        s.releaseGain *= release;
        out[i] = 0.7f * softClip((y + p[P::PhysBody] * s.body) * (1.0f + 4.0f * drive)) * s.releaseGain;
        if (s.age != UINT32_MAX) ++s.age;
    }
}

}  // namespace hsynth
