// Signal analysis helpers for tests.
#pragma once

#include <cmath>
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

}  // namespace hsynth_test
