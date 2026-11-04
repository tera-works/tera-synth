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
enum class FilterType : int { Off = 0, LP12, LP24, BP, HP, Notch, Ladder, CombPlus, CombMinus, Formant, Count };
// Voice FX: per-voice insert after the filter, before the amp envelope.
enum class VfxType : int { Off = 0, Saturate, Fold, Crush, Ring, Count };
enum class VoiceMode : int { Poly = 0, Mono, Legato, Count };
enum class ArpMode : int { Up = 0, Down, UpDown, Random, Order, Chord, Count };
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
    Cc1,            // assignable MIDI controllers (ctl.cc1..4: CC numbers), e.g. neck sensors
    Cc2,
    Cc3,
    Cc4,
    Lfo3,           // second per-voice LFO
    Env3,           // third envelope (per voice)
    Constant,       // 1.0 (offsets, or scaled by "via")
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
    FilterMorph,    // formant vowel position, +-1
    VfxAmount,      // voice FX amount, +-1
    LayerAPan,      // +-1
    LayerBPan,
    LayerCPan,
    FmOp1Level,     // FM operator level (all FM layers), x(1 + amount), 0..2
    FmOp2Level,
    FmOp3Level,
    FmOp4Level,
    AmpAttack,      // amp envelope times, x8^amount
    AmpDecay,
    AmpRelease,
    UnisonDetune,   // x(1 + amount)
    VfxMix,         // +-1
    Lfo3Rate,       // +-4 octaves
    FmOp5Level,
    FmOp6Level,
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
    FilterMorph,    // formant filter: vowel a-e-i-o-u (0..1)
    VfxType,
    VfxAmount,
    VfxRatio,       // ring modulator frequency / note frequency
    VfxMix,
    CtlCc1,         // CC numbers feeding the mod sources cc1..cc4
    CtlCc2,
    CtlCc3,
    CtlCc4,
    VoiceMode,      // poly / mono (retrigger) / legato
    Glide,          // portamento time, seconds (0 = off)
    GlideMode,      // always / legato (only between overlapping notes)
    Unison,         // voices per note 1..8
    UnisonDetune,   // cents, total spread
    UnisonSpread,   // stereo spread 0..1
    Lfo1Sync,       // tempo sync division (fx::SyncDiv, 0 = off: rate in Hz)
    Lfo2Sync,
    Lfo3Wave,
    Lfo3Rate,
    Lfo3Sync,
    Lfo3KeySync,
    Env3Attack,
    Env3Decay,
    Env3Sustain,
    Env3Release,
    MasterTune,     // global (master section): cents, A4 = 440 Hz * 2^(tune / 1200)
    VelCurve,       // velocity curve -1 (soft) .. 0 (linear) .. 1 (hard)
    TuneC,          // scale tuning: cents offset per pitch class C .. B (12 params)
    TuneLast = TuneC + 11,
    ArpOn,          // arpeggiator (per part)
    ArpMode,        // ArpMode
    ArpRate,        // division: option k = fx::SyncDiv k + 1 (1/1 .. 1/32, no "off")
    ArpOctaves,     // 1..4
    ArpGate,        // note length as a fraction of the step
    ArpSwing,       // delay of every second step, fraction of a step 0..0.5
    ArpLatch,       // keep playing after the keys are released (until a new chord)
    GlobalCount
};

constexpr int kModSlots = 16;
enum ModSlotParam : int { ModSrc = 0, ModVia, ModDst, ModAmt, ModSlotCount };
constexpr int kModBase = GlobalCount;

// FM: the operator count is a compile-time constant; algorithms are data
// tables (see dsp/fm.h) so 6/8-operator variants only need new tables.
constexpr int kFmOps = 6;  // algorithms 1-8 use ops 1-4, 9-16 all six
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
enum FxChainParam : int { FxTempo = 0, FxRetrig, FxMacro1, FxMacro2, LooperLevel, FxClockSync, FxSlot1, FxChainCount = FxSlot1 + fx::kFxSlots };
enum FxModParam : int { FxModSrc = 0, FxModDst, FxModAmt, FxModCount };
// Part insert effects: slot params (enum: 0 = none, k = kInsertBlockList[k-1])
// followed by the parameters of every insert block.
constexpr int kInsBase = kLayerBase + 3 * LayerParamCount;
constexpr int kInsBlockBase = kInsBase + fx::kInsertSlots;
constexpr int kFxChainBase = kInsBlockBase + fx::kInsertParamTotal;
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
constexpr int insSlot(int slot) { return kInsBase + slot; }
// parameter of insert block `block` (must be in fx::kInsertBlockList)
constexpr int insp(int block, int local) { return kInsBlockBase + fx::insertOffset(fx::insertIndex(block)) + local; }

// Multi-timbral parts: ids [0, kPartParams) form one part's patch (voice,
// layers, mod matrix). MasterVolume and Limiter fall in that range but belong
// to the master section: they are only read from the global array.
constexpr int kPartParams = kFxChainBase;

}  // namespace P

constexpr int kNumParts = 16;

// True for ids stored per part (everything below the effect rack except the
// master volume, limiter switch and master tune).
constexpr bool isPartParam(int id) {
    return id >= 0 && id < P::kPartParams && id != P::MasterVolume && id != P::Limiter && id != P::MasterTune;
}

// Per-part mix settings (multi-timbral mode only; see engine.h).
namespace Mix {
enum : int {
    Volume = 0,  // 0..1, gain = volume^2 (same taper as master volume)
    Pan,         // -1..1, balance (centre = unity on both sides)
    Mute,        // bool: muted parts ignore note-ons and fade out
    Channel,     // MIDI channel 1..16 the part listens to (several parts may share one = layer)
    KeyLow,      // key range 0..127 (inclusive, before transpose)
    KeyHigh,
    Transpose,   // semitones -48..48
    FxSend,      // 0 = dry (bypasses the rack), 1 = all into the shared effect rack
    Count
};
}

// Metadata of the mix settings (names: "volume", "pan", "mute", "channel",
// "key.low", "key.high", "transpose", "fx.send"). def of "channel" is 1; use
// defaultPartMix() for the per-part default (part N -> channel N+1).
const std::vector<ParamInfo>& partMixTable();
void defaultPartMix(int part, float* values);  // Mix::Count values
float sanitizePartMix(int id, float value);

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
