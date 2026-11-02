// A single polyphonic voice: up to kNumLayers sound generators (VA / FM / PCM)
// mixed into a stereo multimode filter and amp envelope. Each voice carries its
// own per-note expression state (pitch, pressure, timbre, X) so that every key
// can be modulated independently.
#pragma once

#include <cstdint>

#include "hsynth/common.h"
#include "hsynth/dsp/envelope.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/dsp/lfo.h"
#include "hsynth/dsp/oscillators.h"
#include "hsynth/dsp/svf.h"
#include "hsynth/dsp/wavetable.h"
#include "hsynth/event.h"
#include "hsynth/params.h"
#include "hsynth/sample_set.h"

namespace hsynth {

// Shared, per-control-block state supplied by the engine.
struct VoiceContext {
    const float* params = nullptr;
    float sampleRate = 48000.0f;
    float pitchBendSemis = 0.0f;   // already scaled by bend range
    float pitchBendNorm = 0.0f;    // -1..1
    float modWheel = 0.0f;
    float chanPressure = 0.0f;
    float breath = 0.0f;
    float expression = 1.0f;
    float lfo2 = 0.0f;
    const SampleSet* sampleSets[kNumLayers] = {};
};

// Plays one SFZ region / WAV sample.
class PcmPlayer {
public:
    void start(const Region* r, int key, float velocity, float startFrac, float velTrackScale, float sampleRate);
    void release();
    void stop() { region_ = nullptr; }
    bool active() const { return region_ != nullptr; }
    bool oneShot() const { return region_ && region_->loopMode == LoopMode::OneShot; }
    const Region* region() const { return region_; }
    // pitchSemis: extra transposition (bend, modulation, layer coarse/fine)
    void setPitch(float pitchSemis, float engineRate);
    void render(float* L, float* R, int n, float gain);

private:
    float at(int c, int64_t idx) const;
    const Region* region_ = nullptr;
    double pos_ = 0.0;
    double step_ = 1.0;
    double baseRatio_ = 1.0;
    int64_t end_ = 0;
    bool released_ = false;
    bool looping_ = false;
    float gainL_ = 1.0f, gainR_ = 1.0f;
    Adsr env_;
    bool useEnv_ = false;
};

class Voice {
public:
    static constexpr int kMaxRegionsPerLayer = 4;

    void prepare(float sampleRate);

    void start(const Event& e, const VoiceContext& ctx, uint64_t age, float random);
    void release();
    void steal();            // fast fade-out
    void stopLayer(int l);   // immediately stop PCM playback of a layer (sample set swap)
    void hardStop() { active_ = false; }
    void applyExpression(EventType type, float value);

    // Render n <= kControlBlock samples, adding into L/R.
    void render(float* L, float* R, int n, const VoiceContext& ctx);

    bool active() const { return active_; }
    bool released() const { return released_; }
    bool stolen() const { return stolen_; }
    uint32_t noteId() const { return noteId_; }
    int key() const { return key_; }
    uint64_t age() const { return age_; }
    float level() const { return ampEnv_.level(); }
    bool sustained = false;  // note-off arrived while sustain pedal / hold active
    bool hold = false;       // per-note hard-press hold (NoteHold event)
    int part = 0;            // owning part (multi-timbral), set by the engine
    int matchKey = 60;       // key as received (before part transpose), for key-matched events

private:
    struct Layer {
        LayerType type = LayerType::Off;
        VaOsc osc1, osc2;
        FmVoice fm;
        WtOsc wt;
        PcmPlayer pcm[kMaxRegionsPerLayer];
        int pcmCount = 0;
        float dt1 = 0, dt2 = 0;      // current oscillator increments (ramped)
        float gain = 0;              // smoothed layer gain
        bool finished = false;
    };

    float modSource(ModSource s, const VoiceContext& ctx) const;

    float sampleRate_ = 48000.0f;
    bool active_ = false, released_ = false, stolen_ = false;
    uint32_t noteId_ = 0;
    int key_ = 60;
    float velocity_ = 1.0f;
    float random_ = 0.0f;
    uint64_t age_ = 0;
    float timeSinceOn_ = 0.0f;

    // per-note expression (targets and smoothed values)
    float notePitchT_ = 0, notePressureT_ = 0, noteTimbreT_ = 0, noteXT_ = 0;
    float notePitch_ = 0, notePressure_ = 0, noteTimbre_ = 0, noteX_ = 0;
    bool firstBlock_ = true;
    bool stereo_ = false;

    Adsr ampEnv_, env2_;
    Lfo lfo1_;
    float lfo1Value_ = 0.0f;
    Rng rng_;
    VoiceFilter filterL_, filterR_;
    float ampGain_ = 0.0f;           // smoothed
    float panL_ = 1.0f, panR_ = 1.0f;
    Layer layers_[kNumLayers];
};

}  // namespace hsynth
