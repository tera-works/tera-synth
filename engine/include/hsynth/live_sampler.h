#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace hsynth {

class LiveSampler {
public:
    LiveSampler();
    ~LiveSampler();

    bool start(int source, float maxSec, float armDb);
    double stop();
    std::array<float, 3> state() const;
    std::vector<float> peaks(int bins) const;
    std::array<float, 7> analyze() const;
    void audition(bool on, bool loop, int startFrame, int endFrame);
    std::string exportWav(const std::string& path, int startFrame, int endFrame, float gainDb,
                          int loopStart, int loopEnd, float xfadeMs, int rootMidi) const;
    void discard();

    void prepare(float sampleRate);
    // Called only by the audio thread with valid input frames or final output.
    void capture(int source, const float* left, const float* right, int frames);
    bool auditioning() const;
    void mixAudition(float* left, float* right, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hsynth
