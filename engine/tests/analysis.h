// Signal analysis helpers for tests.
#pragma once

#include <cmath>
#include <complex>
#include <vector>

namespace hsynth_test {

inline double rms(const std::vector<float>& x, size_t from = 0, size_t to = (size_t)-1) {
    if (to > x.size()) to = x.size();
    if (from >= to) return 0.0;
    double s = 0;
    for (size_t i = from; i < to; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / (double)(to - from));
}

inline double peak(const std::vector<float>& x) {
    double p = 0;
    for (float v : x) p = std::fmax(p, std::fabs((double)v));
    return p;
}

inline bool allFinite(const std::vector<float>& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

// Frequency by counting rising zero crossings with linear interpolation.
inline double zeroCrossFreq(const std::vector<float>& x, double sr, size_t from, size_t to) {
    if (to > x.size()) to = x.size();
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = (double)(i - 1) + (double)(-x[i - 1]) / (double)(x[i] - x[i - 1]);
            if (first < 0) first = t; else ++count;
            last = t;
        }
    }
    if (count < 1) return 0;
    return sr * count / (last - first);
}

// Magnitude of a single DFT bin at frequency f (Goertzel), normalised.
inline double goertzel(const std::vector<float>& x, double sr, double f, size_t from, size_t n) {
    const double w = 2.0 * 3.14159265358979323846 * f / sr;
    const double c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n && from + i < x.size(); ++i) {
        // Hann window
        const double win = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * (double)i / (double)(n - 1));
        const double s0 = x[from + i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (double)n * 4.0;
}

// Alias-to-harmonic energy ratio (dB) of a steady periodic tone with
// fundamental f0: 32768-point FFT (4-term Blackman-Harris) from `from`;
// bins within 6 bins of a harmonic below Nyquist count as signal, the rest as
// alias / noise. maxHz limits the alias band (e.g. 10 kHz for the audible
// part); returns the ratio for everything below maxHz.
inline double aliasDb(const std::vector<float>& x, size_t from, double f0, double sr = 48000.0, double maxHz = 1e9) {
    const size_t N = 32768;
    std::vector<std::complex<double>> a(N);
    for (size_t i = 0; i < N; ++i) {
        const double t = 2.0 * 3.14159265358979323846 * (double)i / (double)(N - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        a[i] = from + i < x.size() ? x[from + i] * w : 0.0;
    }
    for (size_t i = 1, j = 0; i < N; ++i) {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= N; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < N; i += len) {
            std::complex<double> w(1.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    const double bin = sr / (double)N;
    double harm = 0, alias = 0;
    for (size_t k = (size_t)(20.0 / bin); k < N / 2; ++k) {
        const double f = (double)k * bin, p = std::norm(a[k]);
        const double h = std::round(f / f0);
        if (h >= 1 && std::fabs(f - h * f0) <= 6 * bin && h * f0 < sr / 2) harm += p;
        else if (f < maxHz) alias += p;
    }
    return 10.0 * std::log10(alias / harm + 1e-30);
}

}  // namespace hsynth_test
