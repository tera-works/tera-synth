// Parameter layout and metadata.
//
// Every editable sound parameter is a float in one flat array. The array index
// is the parameter id; metadata (name, range, enum labels, group) is available
// at runtime so hosts can build generic editors and presets can be stored by
// name (robust against future re-ordering of ids).
#pragma once

#include <string>
#include <vector>

#include "hsynth/fx/fx_params.h"

namespace hsynth {

enum class LayerType : int { Off = 0, VA, FM, PCM, WT, Count };
enum class VaWave : int { Saw = 0, Square, Pulse, Triangle, Sine, Count };
enum class FilterType : int { Off = 0, LP12, LP24, BP, HP, Notch, Count };
enum class LfoWave : int { Sine = 0, Triangle, Saw, Square, SampleHold, Count };

enum class ModSource : int {
    None = 0,
    Velocity,
    Key,            // (note - 60) / 60
    AmpEnv,
    Env2,
    Lfo1,           // per-voice LFO
    Lfo2,           // global LFO
    ModWheel,
    PitchBend,
    ChannelPressure,
    NotePressure,   // per-note pressure (MPE / MIDI 2.0 / shoulder keyboard)
    NoteTimbre,     // per-note Y / CC74
    NoteX,          // per-note X position
    NotePitch,      // per-note pitch offset, normalised by 12 semitones
    Random,         // random value per note-on
    Breath,         // CC2
    Expression,     // CC11
    Count
};

enum class ModDest : int {
    None = 0,
    Pitch,          // +-24 semitones * amount
    LayerALevel,
    LayerBLevel,
    LayerCLevel,
    Morph,          // crossfade between layer A and B
    LayerAPitch,    // +-24 semitones
    LayerBPitch,
    LayerCPitch,
    VaPulseWidth,
    VaOscMix,
    VaNoise,
    FmIndex,
    FmFeedback,
    Cutoff,         // +-5 octaves * amount
    Resonance,
    Drive,
    Amp,
    Pan,
    Lfo1Rate,
    WtPos,          // wavetable position (all WT layers), +-1
    WtWarp,         // wavetable warp amount
    Count
};

enum class ParamKind { Float, Int, Enum, Bool };
enum class ParamCurve { Linear, Exp };

struct ParamInfo {
    std::string name;    // stable id used in presets, e.g. "a.fm.op1.level"
    std::string group;   // UI grouping, e.g. "Layer A / FM"
    std::string label;   // short display label
    ParamKind kind = ParamKind::Float;
    ParamCurve curve = ParamCurve::Linear;
    float min = 0.0f, max = 1.0f, def = 0.0f;
    std::string unit;
    std::vector<std::string> options;  // enum labels
};

namespace P {

enum Global : int {
    MasterVolume = 0,
    BendRange,
    Morph,
    FilterType,
    FilterCutoff,
    FilterReso,
    FilterDrive,
    FilterKeytrack,
    FilterEnvAmt,
    FilterVelAmt,
    AmpAttack,
    AmpDecay,
    AmpSustain,
    AmpRelease,
    AmpVelSens,
    Env2Attack,
    Env2Decay,
    Env2Sustain,
    Env2Release,
    Lfo1Wave,
    Lfo1Rate,
    Lfo1Delay,
    Lfo1KeySync,
    Lfo2Wave,
    Lfo2Rate,
    Limiter,
    GlobalCount
};

constexpr int kModSlots = 8;
enum ModSlotParam : int { ModSrc = 0, ModVia, ModDst, ModAmt, ModSlotCount };
constexpr int kModBase = GlobalCount;

// FM: the operator count is a compile-time constant; algorithms are data
// tables (see dsp/fm.h) so 6/8-operator variants only need new tables.
constexpr int kFmOps = 4;
enum FmOpParam : int { OpRatio = 0, OpDetune, OpLevel, OpAttack, OpDecay, OpSustain, OpRelease, OpVelSens, FmOpCount };

enum LayerParam : int {
    LType = 0,
    LLevel,
    LPan,
    LCoarse,
    LFine,
    LVelSens,
    VaWave1,
    VaWave2,
    VaPw,
    VaOsc2Semi,
    VaDetune,
    VaMix,
    VaNoise,
    FmAlgo,
    FmFeedback,
    FmIndex,
    FmOpBase,
    PcmStart = FmOpBase + kFmOps * FmOpCount,
    PcmVelTrack,
    WtTable,
    WtPos,
    WtPosEnv,
    WtWarpMode,
    WtWarpAmt,
    WtUnison,
    WtDetune,
    WtSpread,
    LayerParamCount
};

constexpr int kLayerBase = kModBase + kModSlots * ModSlotCount;

// Effect rack (see fx/fx_params.h): chain settings, FX modulation slots, then
// the parameters of every effect block.
enum FxChainParam : int { FxTempo = 0, FxRetrig, FxMacro1, FxMacro2, LooperLevel, FxSlot1, FxChainCount = FxSlot1 + fx::kFxSlots };
enum FxModParam : int { FxModSrc = 0, FxModDst, FxModAmt, FxModCount };
constexpr int kFxChainBase = kLayerBase + 3 * LayerParamCount;
constexpr int kFxModBase = kFxChainBase + FxChainCount;
constexpr int kFxBlockBase = kFxModBase + fx::kFxModSlots * FxModCount;
constexpr int kNumParams = kFxBlockBase + fx::kBlockParamTotal;

constexpr int layer(int l, int p) { return kLayerBase + l * LayerParamCount + p; }
constexpr int fmOp(int l, int op, int p) { return layer(l, FmOpBase + op * FmOpCount + p); }
constexpr int mod(int slot, int p) { return kModBase + slot * ModSlotCount + p; }
constexpr int fxChain(int p) { return kFxChainBase + p; }
constexpr int fxSlot(int slot) { return kFxChainBase + FxSlot1 + slot; }
constexpr int fxMod(int slot, int p) { return kFxModBase + slot * FxModCount + p; }
constexpr int fxp(int block, int local) { return kFxBlockBase + fx::blockOffset(block) + local; }

}  // namespace P

// Metadata for all parameters, indexed by id. Built once (thread-safe static).
const std::vector<ParamInfo>& paramTable();

// Look up id by name; returns -1 if unknown.
int findParam(const std::string& name);

// Parameter ids an FX modulation slot can target (index = enum value of
// "fxmodN.dst"; entry 0 is "none" = -1).
const std::vector<int>& fxModTargets();

// Fill array with default values (size kNumParams).
void defaultParams(float* values);

// Clamp and quantise a value according to the metadata.
float sanitizeParam(int id, float value);

}  // namespace hsynth
