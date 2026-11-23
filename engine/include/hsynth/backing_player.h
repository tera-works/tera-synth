#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "hsynth/json.h"

namespace hsynth {

class BackingPlayer {
public:
    BackingPlayer();
    ~BackingPlayer();

    Json load(const std::string& path);
    // Vocals, drums, bass, other. All four must be stereo and share a timeline.
    Json loadStems(const std::string paths[4]);
    void setStemMix(int stem, float gain, bool mute, bool solo);
    bool isStemsLoaded() const;
    Json stemStateJson() const;
    void play(bool on);
    void seek(double seconds);
    void setLoop(double a, double b, bool on);
    void setVolume(float volume);
    // Speed advances source time independently of the pitch shift in semitones.
    void setSpeed(float speed);
    void setPitch(float semitones);
    void setRoute(bool directBypassFx);
    bool direct() const;
    std::array<double, 6> state() const;
    std::vector<float> peaks(int bins) const;

    void prepare(float sampleRate);
    bool active() const;
    // Single consumer, the engine audio callback.
    void mix(float* left, float* right, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hsynth
