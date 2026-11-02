// PCM sample data and key/velocity mapping.
//
// A SampleSet is built entirely on a loader (non-audio) thread: all files are
// decoded into memory before the set is handed to the engine, so the audio
// thread never performs disk I/O. Sets are immutable once published.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hsynth/dsp/wavetable.h"

namespace hsynth {

struct Sample {
    std::string path;
    int channels = 1;
    double sampleRate = 48000.0;
    int64_t frames = 0;               // valid frames (buffers are padded)
    std::vector<float> data[2];       // channel buffers, kPadFrames of zeros appended
    int64_t loopStart = -1;           // from smpl chunk
    int64_t loopEnd = -1;             // exclusive
    int unityNote = -1;

    static constexpr int kPadFrames = 4;
    const float* channel(int c) const { return data[c < channels ? c : 0].data(); }
};

enum class LoopMode : uint8_t { NoLoop, OneShot, Continuous, Sustain };

struct RegionEnv {
    bool enabled = false;
    float attack = 0.0f, decay = 0.0f, sustain = 1.0f, release = 0.0f;
};

struct Region {
    const Sample* sample = nullptr;
    int lokey = 0, hikey = 127;
    int lovel = 1, hivel = 127;
    float lorand = 0.0f, hirand = 1.0f;
    int keycenter = 60;
    float pitchKeytrack = 100.0f;  // cents per key
    float tune = 0.0f;             // cents
    int transpose = 0;
    float volumeDb = 0.0f;
    float pan = 0.0f;              // -1..1
    float ampVeltrack = 100.0f;    // percent
    LoopMode loopMode = LoopMode::NoLoop;
    int64_t loopStart = -1, loopEnd = -1;  // exclusive end
    int64_t offset = 0, end = -1;          // end exclusive, -1 = sample end
    RegionEnv ampeg;
};

struct SampleSet {
    std::string name;
    std::string sourcePath;
    std::vector<std::unique_ptr<Sample>> samples;
    std::vector<Region> regions;
    std::vector<std::string> warnings;
    std::unique_ptr<Wavetable> wavetable;  // set instead of samples for wavetable layers ("user" table)
    size_t memoryBytes() const;

    // Returns number of matching regions written to out (at most max).
    int findRegions(int key, int vel127, float rand01, const Region** out, int max) const;
};

// Load a single WAV as a set with one region spanning all keys.
std::unique_ptr<SampleSet> loadWavSampleSet(const std::string& path, std::string& error);

// Load an SFZ instrument. Unsupported opcodes are reported in warnings.
std::unique_ptr<SampleSet> loadSfzSampleSet(const std::string& path, std::string& error);

// Dispatch on file extension (.wav / .sfz).
std::unique_ptr<SampleSet> loadSampleSet(const std::string& path, std::string& error);

// Opcodes understood by the SFZ loader (for documentation / UI).
const std::vector<std::string>& supportedSfzOpcodes();

}  // namespace hsynth
