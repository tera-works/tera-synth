#include "hsynth/dsp/piano.h"
#include "hsynth/params.h"

#include <algorithm>
#include <cmath>

namespace hsynth {

void PianoBusState::pedal(float value, float amount, float sr, int offset, bool audible) {
    const bool down = down_ ? value > 0.07f : value >= 0.15f;
    if (down == down_) return;
    down_ = down;
    if (!audible || amount == 0.0f) return;
    auto& b = bursts_[down ? 0 : 1];
    if (b.remaining > 0 && b.duration - b.remaining < static_cast<int>(0.010f * sr)) return;
    b = {};
    b.duration = b.remaining = std::max(1, static_cast<int>((down ? 0.067f : 0.036f) * sr));
    b.delay = offset;
    b.level = amount * (down ? 0.095f : 0.063f);
}

void PianoBusState::process(float* l, float* r, int n, const float* p, float sr) {
    const float lid = p[P::PianoLid], comp = p[P::PianoDynComp];
    if (!busy() && lid == 1.0f && lid_ == 1.0f && comp == 0.0f) {
        detector_ = 0.0f;
        return;
    }
    const bool filter = lid != 1.0f || lid_ != 1.0f;
    const float cutoff = std::min(0.45f * sr, 1600.0f * std::pow(10.0f, lid));
    const float lp = 1.0f - std::exp(-6.2831853f * cutoff / sr);
    const float ramp = 1.0f / (0.015f * sr);
    const float attack = 1.0f - std::exp(-1.0f / (0.003f * sr));
    const float release = 1.0f - std::exp(-1.0f / (0.150f * sr));
    const float noiseHi = 1.0f - std::exp(-6.2831853f * 2700.0f / sr);
    const float noiseLo = 1.0f - std::exp(-6.2831853f * 360.0f / sr);
    for (int i = 0; i < n; ++i) {
        for (int t = 0; t < 2; ++t) {
            auto& b = bursts_[t];
            if (b.delay > 0) { --b.delay; continue; }
            if (b.remaining <= 0) continue;
            const float env = static_cast<float>(b.remaining--) / b.duration;
            b.phase += 6.2831853f * (t == 0 ? 72.0f : 112.0f) / sr;
            const float noise = rng_.bipolar();
            b.lp += noiseHi * (noise - b.lp);
            b.hp += noiseLo * (b.lp - b.hp);
            const float x = b.level * env * env * (0.62f * std::sin(b.phase) * env + 0.75f * (b.lp - b.hp));
            l[i] += x;
            r[i] += x;
        }
        if (filter) {
            lid_ += clampf(lid - lid_, -ramp, ramp);
            if (std::fabs(lid - lid_) <= ramp) lid_ = lid;
            lidCoef_ += clampf(lp - lidCoef_, -ramp, ramp);
            left_ += lidCoef_ * (l[i] - left_);
            right_ += lidCoef_ * (r[i] - right_);
            if (lid_ != 1.0f) {
                const float amount = std::min(1.0f, 4.0f * (1.0f - lid_));
                l[i] += amount * (left_ - l[i]);
                r[i] += amount * (right_ - r[i]);
            }
        }
        if (comp != 0.0f) {
            const float peak = std::max(std::fabs(l[i]), std::fabs(r[i]));
            detector_ += (peak > detector_ ? attack : release) * (peak - detector_);
            // Detect on the pre-fader piano bus. A full-velocity single
            // voice is well below line level here, before master gain.
            constexpr float threshold = 0.025f;
            if (detector_ > threshold) {
                const float over = 20.0f * std::log10(detector_ / threshold);
                const float knee = std::min(1.0f, over / 6.0f);
                const float reduction = std::min(6.0f, 0.5f * over * knee);
                const float gain = dbToGain(-comp * reduction);
                l[i] *= gain;
                r[i] *= gain;
            }
        }
    }
    if (lid_ == 1.0f) { left_ = right_ = 0.0f; lidCoef_ = lp; }
    if (comp == 0.0f) detector_ = 0.0f;
}

}  // namespace hsynth
