#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "hsynth/json.h"

namespace hsynth {

class PadSampler {
public:
    static constexpr int kPads = 16;
    PadSampler();
    ~PadSampler();

    Json load(int pad, const std::string& path);
    void clear(int pad);
    void setup(int pad, float level, float semitones, int mode, int choke, int velocity,
               float start, float end);
    void trigger(int pad, float velocity);
    void release(int pad);
    void stopAll();
    std::vector<float> peaks(int pad, int bins) const;
    std::array<float, kPads> levels() const;

    // Audio thread only, except prepare (while stopped).
    void prepare(float sampleRate);
    void panic();
    bool midi(int pad, bool on, float velocity, int offset = 0);
    bool active() const;
    void mix(float* left, float* right, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hsynth
