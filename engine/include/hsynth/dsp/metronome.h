#pragma once

#include <atomic>
#include <cstdint>

namespace hsynth {

class Json;

struct MetronomeConfig {
    bool enabled = false;
    uint8_t numerator = 4, denominator = 4, subdivision = 1, countInBars = 0, route = 0;
    float levelDb = -18.0f, accentDb = 4.0f;
};

MetronomeConfig sanitizeMetronome(MetronomeConfig config);
Json metronomeToJson(const MetronomeConfig& config);
bool metronomeFromJson(const Json& json, MetronomeConfig& config);

class Metronome {
public:
    void prepare(float sampleRate);
    bool configure(const MetronomeConfig& config);  // true when the beat grid changes
    void restart();
    void pause();
    void alignToClock(double quarterBeat, float bpm);
    void beginCountIn();
    void cancelCountIn();
    bool countingIn() const { return countIn_ > 0; }
    int currentBeat() const { return beat_.load(std::memory_order_relaxed); }
    int route() const { return config_.route; }

    // Replaces a mono click buffer. Returns the frame where count-in ended,
    // or -1. The first tick of a run is always on frame zero.
    int render(float* click, int frames, float bpm, bool enabled, bool externalStopped);

private:
    struct Click {
        int age = -1;
        float gain = 0.0f;
        double sine = 0.0, cosine = 1.0;
        double sineStep = 0.0, cosineStep = 1.0;
    };

    MetronomeConfig config_;
    double untilTick_ = 0.0;  // fractional frames, carried across callbacks
    double lastPeriod_ = 0.0;
    double sampleRate_ = 48000.0;
    int attack_ = 24, duration_ = 768;
    float decay_ = 1.0f, level_ = 0.12589254f, accent_ = 1.5848932f;
    int tick_ = 0, countIn_ = 0, countInTick_ = 0;
    unsigned countInNumerator_ = 4, countInDenominator_ = 4, countInSubdivision_ = 1;
    int beatNow_ = -1;
    std::atomic<int> beat_{-1};
    double sineSteps_[3]{}, cosineSteps_[3]{};
    Click click_;
};

}  // namespace hsynth
