#include "hsynth/tuner.h"

#include <cmath>

namespace hsynth {

void Tuner::prepare(float sampleRate) {
    rate_.store(sampleRate, std::memory_order_relaxed);
    input_.reset();
    hadInput_ = false;
}

void Tuner::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(consumerMutex_);
    if (enabled == this->enabled()) return;
    input_.reset();
    tunerEnabled_.store(enabled, std::memory_order_release);
}

void Tuner::configure(float a4, bool mute) {
    a4Hz_.store(std::isfinite(a4) ? std::clamp(a4, 415.0f, 466.0f) : 440.0f, std::memory_order_relaxed);
    muteOut_.store(mute, std::memory_order_relaxed);
}

void Tuner::pushInput(const float* left, const float* right, int frames) {
    if (!left) {
        if (hadInput_) input_.reset();
        hadInput_ = false;
        return;
    }
    if (frames <= 0) return;
    hadInput_ = true;
    input_.push(left, right, frames);
}

bool Tuner::analyze(TunerReading& out) {
    std::lock_guard<std::mutex> lock(consumerMutex_);
    out = {};
    if (!enabled()) return false;
    float samples[kCapacity];
    uint64_t stamp = 0;
    const int count = input_.latest(samples, kCapacity, stamp);
    if (count == 0) return false;
    const auto now = std::chrono::steady_clock::now();
    if (stamp != lastStamp_) {
        if ((stamp >> 32) != (lastStamp_ >> 32) || now - lastInput_ > std::chrono::milliseconds(250))
            valid_ = next_ = 0;
        lastStamp_ = stamp;
        lastInput_ = now;
        filtered_ = {};

        const float rate = rate_.load(std::memory_order_relaxed);
        const int stride = std::clamp(static_cast<int>(std::lround(rate / 12000.0f)), 1, kCapacity);
        const float fs = rate / stride;
        const int len = count / stride;
        const int minLag = std::max(2, static_cast<int>(std::floor(fs / 2093.0f)));
        const int maxLag = std::min(kCapacity / 2 - 2, static_cast<int>(std::ceil(fs / 27.5f)));
        const int compare = std::min(1024, len - maxLag - 2);
        if (maxLag <= minLag || compare < std::max(128, maxLag)) return false;

        // Box-filter before decimation. Compact in place, keeping the newest samples.
        float mean = 0.0f;
        float power = 0.0f;
        for (int i = 0; i < len; ++i) {
            float sum = 0.0f;
            const int start = count % stride + i * stride;
            for (int k = 0; k < stride; ++k) {
                const float s = samples[start + k];
                if (!std::isfinite(s)) { valid_ = next_ = 0; return false; }
                sum += s;
            }
            const float x = sum / stride;
            samples[i] = x;
            mean += x;
            power += x * x;
        }
        mean /= len;
        if (power / len - mean * mean < 1e-6f) { valid_ = next_ = 0; return false; }

        float yin[kCapacity / 2 + 1];
        yin[0] = 1.0f;
        float cumulative = 0.0f;
        const float* x = samples + len - compare - maxLag - 1;
        for (int lag = 1; lag <= maxLag + 1; ++lag) {
            float difference = 0.0f;
            for (int i = 0; i < compare; ++i) {
                const float d = x[i] - x[i + lag];
                difference += d * d;
            }
            cumulative += difference;
            yin[lag] = cumulative > 0.0f ? difference * lag / cumulative : 1.0f;
        }
        int best = 0;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            if (yin[lag] < 0.15f && yin[lag] <= yin[lag - 1] && yin[lag] < yin[lag + 1]) {
                best = lag;
                break;
            }
        }
        if (best == 0) { valid_ = next_ = 0; return false; }
        const float a = yin[best - 1], b = yin[best], c = yin[best + 1];
        // Refine the CMNDF minimum, then interpolate the adjacent rising
        // crossings one period apart when the waveform supports it. At high
        // pitches there are only a few decimated samples per period.
        float period = best + std::clamp(0.5f * (a - c) / (a - 2.0f * b + c), -0.5f, 0.5f);
        float first = -1.0f, last = -1.0f;
        int periods = 0;
        for (int i = 1; i < len; ++i) {
            const float low = samples[i - 1] - mean, high = samples[i] - mean;
            if (low > 0.0f || high <= 0.0f) continue;
            const float crossing = i - 1 - low / (high - low);
            if (last < 0.0f) first = crossing;
            else if (crossing - last < period * 0.8f || crossing - last > period * 1.2f) {
                periods = 0;
                break;
            } else ++periods;
            last = crossing;
        }
        if (periods >= 2) period = (last - first) / periods;
        const float hz = fs / period;
        if (hz < 27.45f || hz > 2098.0f) { valid_ = next_ = 0; return false; }
        recent_[next_] = {std::clamp(hz, 27.5f, 2093.0f), std::clamp(1.0f - b, 0.0f, 1.0f)};
        filtered_ = recent_[next_];
        next_ = (next_ + 1) % 3;
        valid_ = std::min(valid_ + 1, 3);
        if (valid_ == 3) {
            Pitch sorted[3] = {recent_[0], recent_[1], recent_[2]};
            std::sort(sorted, sorted + 3, [](Pitch x, Pitch y) { return x.hz < y.hz; });
            filtered_ = sorted[1];
        }
    }
    if (filtered_.hz == 0.0f || now - lastInput_ > std::chrono::milliseconds(250)) return false;
    const float note = 69.0f + 12.0f * std::log2(filtered_.hz / a4Hz());
    out = {filtered_.hz, 100.0f * (note - std::round(note)), note, filtered_.confidence};
    return true;
}

}  // namespace hsynth
