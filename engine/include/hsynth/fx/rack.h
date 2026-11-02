// Effect rack: kFxSlots serial slots, FX modulation, tempo clock, looper.
//
// Everything here runs on the audio thread except the constructor and
// prepare(). The engine owns one rack and feeds it the full parameter array.
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "hsynth/fx/block.h"
#include "hsynth/params.h"

namespace hsynth {
namespace fx {

struct FxGlobals {
    float modWheel = 0, lfo2 = 0, chanPressure = 0, expression = 1, breath = 0, pitchBend = 0;
    bool notesHeld = false;
};

enum class LooperCmd : int { None = 0, Tap, Stop, Clear, Undo, Play };
enum class LooperState : int { Empty = 0, Recording, Playing, Overdubbing, Stopped };

struct LooperStatus {
    LooperState state = LooperState::Empty;
    float lengthSec = 0, positionSec = 0;
    bool canUndo = false;
};

class Looper {
public:
    static constexpr float kMaxSeconds = 60.0f;
    void prepare(float sr);
    void reset();
    void command(LooperCmd c);
    // in/out: the rack output; the loop is mixed in at `level`
    void process(float* L, float* R, int n, float level);
    LooperStatus status() const;

private:
    void undoStep(int budget);
    float sr_ = 48000;
    std::vector<int16_t> buf_, undo_;  // interleaved stereo
    int cap_ = 0, len_ = 0, pos_ = 0;
    int dubStart_ = 0, dubCount_ = 0, undoLeft_ = 0, undoAt_ = 0;
    bool undoValid_ = false;
    LooperState state_ = LooperState::Empty;
    float fade_ = 0;
    std::atomic<int> stState_{0}, stLen_{0}, stPos_{0};
    std::atomic<bool> stUndo_{false};
};

class FxRack {
public:
    FxRack();
    ~FxRack();
    void prepare(float sampleRate);
    void reset();

    // Audio thread. p = full parameter array (P::kNumParams).
    void paramsChanged(const float* p, int oversample, bool highQuality);
    void noteOn(int key, float velocity, bool firstNote);
    void process(float* L, float* R, int n, const FxGlobals& g, const float* inL, const float* inR);
    void looperCommand(LooperCmd c) { looper_.command(c); }
    // target 0 = cabinet, 1 = IR reverb
    void setUserIr(int target, const ConvIr* ir);

    // audio thread: tempo / beat position for blocks outside the rack (part inserts)
    float tempo() const { return tempo_; }
    double beat() const { return beat_; }

    // any thread
    LooperStatus looperStatus() const { return looper_.status(); }
    // Blocks in processing order (duplicates and "none" removed); any thread.
    int chainSnapshot(int* out) const;

private:
    struct ModSlot { int src = 0, local = -1, block = 0; float amt = 0; };
    float sourceValue(int src, const FxGlobals& g) const;
    void applyModulation(const FxGlobals& g);
    void runChain(float* L, float* R, int n);

    float sr_ = 48000;
    std::unique_ptr<FxBlock> blocks_[BlockCount];
    int chain_[kFxSlots] = {};
    int chainLen_ = 0;
    bool inChain_[BlockCount] = {};
    float xfade_[BlockCount] = {};
    std::vector<float> base_, cur_;  // block parameters: preset value / modulated value
    std::vector<int> blockOfLocal_;
    bool dirty_[BlockCount] = {};
    ModSlot mods_[kFxModSlots];
    int numMods_ = 0;
    FxContext ctx_;
    float tempo_ = 120, macro1_ = 0, macro2_ = 0, looperLevel_ = 0.8f;
    bool retrig_ = true;
    double beat_ = 0;
    float velocity_ = 0, lastKey_ = 60, env_ = 0, held_ = 0;
    std::vector<float> tmpL_, tmpR_;
    Looper looper_;
    std::atomic<int> snap_[kFxSlots];
    std::atomic<int> snapLen_{0};
    bool first_ = true;
};

}  // namespace fx
}  // namespace hsynth
