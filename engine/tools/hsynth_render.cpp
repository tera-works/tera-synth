// Offline renderer: renders a preset with a built-in note sequence to WAV, or
// compares the render against a reference WAV (regression check for DSP
// optimisations).
//
// usage: hsynth_render --preset p.json [--library dir] [--out out.wav]
//                      [--pattern chord|scale|drums|sustain] [--seconds 6]
//                      [--sr 48000] [--block 128] [--voices 32] [--quality 0..2]
//                      [--compare ref.wav] [--max-error-db -80]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/preset.h"
#include "hsynth/wav.h"

using namespace hsynth;

int main(int argc, char** argv) {
    std::string presetPath, library = ".", out = "render.wav", pattern = "chord", compare;
    double seconds = 6.0, maxErrDb = -80.0;
    float sr = 48000.0f;
    int block = 128, voices = 32, quality = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--preset") presetPath = next();
        else if (a == "--library") library = next();
        else if (a == "--out") out = next();
        else if (a == "--pattern") pattern = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--sr") sr = (float)std::atof(next().c_str());
        else if (a == "--block") block = std::atoi(next().c_str());
        else if (a == "--voices") voices = std::atoi(next().c_str());
        else if (a == "--quality") quality = std::atoi(next().c_str());
        else if (a == "--compare") compare = next();
        else if (a == "--max-error-db") maxErrDb = std::atof(next().c_str());
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    Engine engine;
    engine.prepare(sr, block);
    engine.setMaxVoices(voices);
    engine.setQuality((Quality)quality);
    if (!presetPath.empty()) {
        Preset p;
        std::string err;
        if (!loadPresetFile(presetPath, p, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        std::vector<std::string> warnings;
        applyPreset(engine, p, library, warnings);
        for (auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        std::printf("preset: %s\n", p.name.c_str());
    }

    RenderJob job;
    job.sampleRate = sr;
    job.blockSize = block;
    job.seconds = seconds;
    if (pattern == "scale") {
        const int s[] = {60, 62, 64, 65, 67, 69, 71, 72};
        for (int i = 0; i < 8; ++i) addNote(job, 0.3 * i, 0.25, s[i], 0.4f + 0.07f * (float)i);
    } else if (pattern == "drums") {
        for (int b = 0; b < 8; ++b) {
            addNote(job, 0.25 * b, 0.1, 42, 0.7f);
            if (b % 4 == 0) addNote(job, 0.25 * b, 0.1, 36, 1.0f);
            if (b % 4 == 2) addNote(job, 0.25 * b, 0.1, 38, 0.9f);
        }
    } else if (pattern == "sustain") {
        addNote(job, 0.0, seconds * 0.7, 48, 0.8f);
        addNote(job, 0.0, seconds * 0.7, 55, 0.8f);
    } else {
        const int c1[] = {48, 60, 64, 67}, c2[] = {45, 57, 60, 64};
        for (int k : c1) addNote(job, 0.1, 1.8, k, 0.7f);
        for (int k : c2) addNote(job, 2.1, 1.8, k, 0.9f);
        job.events.push_back({1.0, Event::control(EventType::ModWheel, 1.0f)});
        job.events.push_back({2.0, Event::control(EventType::ModWheel, 0.0f)});
        std::stable_sort(job.events.begin(), job.events.end(), [](auto& a, auto& b) { return a.time < b.time; });
    }

    std::vector<float> L, R;
    const auto t0 = std::chrono::steady_clock::now();
    renderOffline(engine, job, L, R);
    const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    float peakV = 0;
    for (size_t i = 0; i < L.size(); ++i) peakV = std::max(peakV, std::max(std::fabs(L[i]), std::fabs(R[i])));
    std::printf("rendered %.2f s in %.3f s (%.1fx realtime), peak %.3f, peak voices %d\n", seconds, cpu, seconds / cpu,
                peakV, engine.stats().peakVoices);

    if (!compare.empty()) {
        WavData ref;
        std::string err;
        if (!readWav(compare, ref, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        const size_t n = std::min((size_t)ref.frames, L.size());
        double errE = 0, sigE = 0, maxAbs = 0;
        for (size_t i = 0; i < n; ++i) {
            const float rl = ref.left[i], rr = ref.channels == 2 ? ref.right[i] : rl;
            const double dl = L[i] - rl, dr = R[i] - rr;
            errE += dl * dl + dr * dr;
            sigE += (double)rl * rl + (double)rr * rr;
            maxAbs = std::max(maxAbs, std::max(std::fabs(dl), std::fabs(dr)));
        }
        const double errDb = 10.0 * std::log10((errE + 1e-30) / (sigE + 1e-30));
        std::printf("compare: frames %zu, max abs diff %.3g, error/signal %.1f dB (limit %.1f dB)\n", n, maxAbs, errDb, maxErrDb);
        if (ref.frames != (int64_t)L.size()) std::printf("warning: length differs (%lld vs %zu)\n", (long long)ref.frames, L.size());
        return errDb <= maxErrDb ? 0 : 3;
    }

    std::string err;
    if (!writeWav(out, L, &R, (int)sr, true, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    std::printf("wrote %s\n", out.c_str());
    return 0;
}
