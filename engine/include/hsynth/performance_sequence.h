#pragma once

#include "hsynth/common.h"
#include "hsynth/event.h"

#include <array>
#include <cstdint>

namespace hsynth {

class Json;

struct ArpStep {
    uint8_t gate = 1, accent = 100;
    int8_t oct = 0;
    bool chord = false;
};

struct ArpPattern {
    uint8_t length = 16;
    std::array<ArpStep, 16> steps{};
    bool isDefault() const;
};

Json arpPatternToJson(const ArpPattern& pattern);
bool arpPatternFromJson(const Json& json, ArpPattern& pattern);

struct StrumState {
    static constexpr int kCapacity = 32;
    struct Note { Event event; uint64_t due; };
    std::array<Note, kCapacity> pending{};
    std::array<Event, 8> gathering{};
    int count = 0, gathered = 0;
    uint64_t deadline = 0;
    bool alternate = false;
    Rng rng{0x51ab23u};

    bool collect(const Event& event, uint64_t frame, const float* params, float sr);
    void chord(Event* notes, int size, uint64_t frame, const float* params, float sr);
    void flush(uint64_t frame, const float* params, float sr);
    void cancel(const Event& event);
    void clear() { count = gathered = 0; }

private:
    void space(Event* notes, int size, uint64_t frame, const float* params, float sr);
};

struct RhythmConfig {
    bool enabled = false;
    int pattern = 0;
    float levelDb = -12.0f, swing = 0.0f;
};

Json rhythmToJson(const RhythmConfig& config, bool fill = false);
bool rhythmFromJson(const Json& json, RhythmConfig& config, bool& fill);
Json rhythmPatternsJson();

class RhythmPlayer {
public:
    void reset(float sampleRate);
    void process(float* left, float* right, int frames, double beat, double beatsPerFrame,
                 bool running, const RhythmConfig& config, bool fill);

private:
    struct Drum {
        float phase = 0, inc = 0, env = 0, sweep = 0, decay = 0, noise = 0;
        int age = 0, choke = 0;
    } drums_[8];
    void hit(int lane, float velocity);
    float tick(int lane, Drum& drum);
    float sr_ = 48000.0f, gain_ = 0;
    int64_t step_ = 0;
    int pattern_ = 0;
    double previousBeat_ = 0.0;
    bool running_ = false;
    Rng rng_{0x122db7u};
};

}  // namespace hsynth
