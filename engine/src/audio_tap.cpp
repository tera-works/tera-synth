#include "hsynth/audio_tap.h"

#include <cmath>
#include <complex>

namespace hsynth {

namespace {

using Complex = std::complex<float>;
constexpr float kTau = 6.2831853071795864769f;

void fft(Complex* x) {
    constexpr int n = AudioTap::kFftSize;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (int width = 2; width <= n; width *= 2) {
        const Complex step = std::polar(1.0f, -kTau / width);
        for (int i = 0; i < n; i += width) {
            Complex w(1.0f, 0.0f);
            for (int k = 0; k < width / 2; ++k) {
                const Complex a = x[i + k], b = w * x[i + k + width / 2];
                x[i + k] = a + b;
                x[i + k + width / 2] = a - b;
                w *= step;
            }
        }
    }
}

float fundamental(const float* x, int n, float rate) {
    const int span = std::min(1024, n / 2);
    const int minLag = std::max(2, int(rate / 5000.0f));
    const int maxLag = std::min(n - span - 1, int(rate / 20.0f));
    if (span < 64 || maxLag <= minLag) return 0.0f;
    const float* recent = x + n - span;
    float energy = 0.0f;
    for (int i = 0; i < span; ++i) energy += recent[i] * recent[i];
    if (energy < 1e-9f * span) return 0.0f;
    float prev = -1.0f, mid = -1.0f;
    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag) {
        float dot = 0.0f, other = 0.0f;
        const float* delayed = recent - lag;
        for (int i = 0; i < span; ++i) {
            dot += recent[i] * delayed[i];
            other += delayed[i] * delayed[i];
        }
        const float corr = other > 0.0f ? dot / std::sqrt(energy * other) : 0.0f;
        if (lag > minLag && mid >= 0.85f && mid >= prev && mid > corr) {
            const float frac = 0.5f * (prev - corr) / (prev - 2.0f * mid + corr);
            return rate / (lag - 1 + frac);
        }
        prev = mid;
        mid = corr;
    }
    return 0.0f;
}

}  // namespace

void AudioTap::prepare(float sampleRate) {
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    history_.reset();
    silent_.store(true, std::memory_order_release);
}

void AudioTap::setEnabled(bool enabled) {
    // Invalidate on each enable so a newly opened scope cannot see an old frame.
    const bool old = observerActive_.exchange(enabled, std::memory_order_acq_rel);
    if (old != enabled) history_.reset();
}

void AudioTap::capture(const float* left, const float* right, int frames, bool silentIdle) {
    if (silentIdle) {
        if (!silent_.exchange(true, std::memory_order_release)) history_.reset();
        return;
    }
    history_.push(left, right, frames);
    silent_.store(false, std::memory_order_release);
}

float AudioTap::readFrame(float* wave, int waveLen, float* spec, int specLen) const {
    waveLen = wave ? std::max(0, waveLen) : 0;
    specLen = spec ? std::max(0, specLen) : 0;
    if (waveLen) std::fill_n(wave, waveLen, 0.0f);
    if (specLen) std::fill_n(spec, specLen, 0.0f);
    if (!enabled() || silent_.load(std::memory_order_acquire)) return 0.0f;
    float samples[kCapacity];
    uint64_t stamp;
    const int count = history_.latest(samples, kCapacity, stamp);
    if (count == 0) return 0.0f;
    float mean = 0.0f;
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(samples[i])) return 0.0f;
        mean += samples[i];
    }
    mean /= count;
    const float rate = sampleRate_.load(std::memory_order_relaxed);
    if (waveLen) {
        int start = std::max(0, count - waveLen);
        for (int i = start; i > 0; --i) {
            if (samples[i - 1] <= 0.0f && samples[i] > 0.0f) { start = i; break; }
        }
        std::copy_n(samples + start, std::min(count - start, waveLen), wave);
    }
    if (specLen && count >= kFftSize) {
        Complex bins[kFftSize];
        for (int i = 0; i < kFftSize; ++i) {
            const float hann = 0.5f - 0.5f * std::cos(kTau * i / (kFftSize - 1));
            bins[i] = (samples[count - kFftSize + i] - mean) * hann;
        }
        fft(bins);
        for (int i = 1; i <= kFftSize / 2; ++i) {
            const float hz = rate * i / kFftSize;
            if (hz < 20.0f || hz > 20000.0f) continue;
            const int band = std::min(specLen - 1, int(specLen * std::log(hz / 20.0f) / std::log(1000.0f)));
            const float amp = std::abs(bins[i]) * (4.0f / (kFftSize - 1));
            const float db = 20.0f * std::log10(std::max(amp, 0.0002511886f));
            spec[band] = std::max(spec[band], std::clamp((db + 72.0f) / 72.0f, 0.0f, 1.0f));
        }
    }
    for (int i = 0; i < count; ++i) samples[i] -= mean;
    return fundamental(samples, count, rate);
}

}  // namespace hsynth
