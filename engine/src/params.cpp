#include "hsynth/params.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "hsynth/dsp/wavetable.h"

namespace hsynth {

namespace {

const std::vector<std::string> kLayerTypeNames = {"off", "va", "fm", "pcm", "wt"};
const std::vector<std::string> kVaWaveNames = {"saw", "square", "pulse", "triangle", "sine"};
const std::vector<std::string> kFilterNames = {"off", "lp12", "lp24", "bp", "hp", "notch", "ladder", "comb+", "comb-", "formant"};
const std::vector<std::string> kVfxNames = {"off", "saturate", "fold", "crush", "ring"};
const std::vector<std::string> kLfoWaveNames = {"sine", "triangle", "saw", "square", "s&h"};
const std::vector<std::string> kModSourceNames = {
    "none", "velocity", "key", "ampenv", "env2", "lfo1", "lfo2", "modwheel", "pitchbend",
    "chanpressure", "notepressure", "notetimbre", "notex", "notepitch", "random", "breath", "expression", "cc1", "cc2", "cc3", "cc4"};
const std::vector<std::string> kModDestNames = {
    "none", "pitch", "a.level", "b.level", "c.level", "morph", "a.pitch", "b.pitch", "c.pitch",
    "va.pw", "va.mix", "va.noise", "fm.index", "fm.feedback", "cutoff", "resonance", "drive",
    "amp", "pan", "lfo1.rate", "wt.pos", "wt.warp", "filter.morph", "vfx.amount"};
const std::vector<std::string> kFmAlgoNames = {
    "1: 4>3>2>1", "2: (4+3)>2>1", "3: 4>3>1 + 2>1", "4: 4>3 + 2>1", "5: 4>(3,2,1)",
    "6: 4>3>2 + 1", "7: 4>1 + 2 + 3", "8: 1+2+3+4"};

struct Builder {
    std::vector<ParamInfo> t;
    Builder() { t.resize(P::kNumParams); }

    ParamInfo& set(int id, const std::string& name, const std::string& group, const std::string& label,
                   float mn, float mx, float def, const std::string& unit = "") {
        ParamInfo& p = t[id];
        p.name = name; p.group = group; p.label = label;
        p.kind = ParamKind::Float; p.curve = ParamCurve::Linear;
        p.min = mn; p.max = mx; p.def = def; p.unit = unit;
        return p;
    }
    void fl(int id, const std::string& n, const std::string& g, const std::string& l, float mn, float mx, float d,
            const std::string& unit = "") {
        set(id, n, g, l, mn, mx, d, unit);
    }
    void ex(int id, const std::string& n, const std::string& g, const std::string& l, float mn, float mx, float d,
            const std::string& unit) {
        set(id, n, g, l, mn, mx, d, unit).curve = ParamCurve::Exp;
    }
    void in(int id, const std::string& n, const std::string& g, const std::string& l, float mn, float mx, float d,
            const std::string& unit = "") {
        set(id, n, g, l, mn, mx, d, unit).kind = ParamKind::Int;
    }
    void bo(int id, const std::string& n, const std::string& g, const std::string& l, bool d) {
        set(id, n, g, l, 0, 1, d ? 1.0f : 0.0f).kind = ParamKind::Bool;
    }
    void en(int id, const std::string& n, const std::string& g, const std::string& l,
            const std::vector<std::string>& opts, int d) {
        ParamInfo& p = set(id, n, g, l, 0, (float)opts.size() - 1, (float)d);
        p.kind = ParamKind::Enum;
        p.options = opts;
    }
};

#include "params_fx.inc"

std::vector<ParamInfo> build() {
    using namespace P;
    Builder b;
    b.fl(MasterVolume, "master.volume", "Master", "Volume", 0, 1, 0.7f);
    b.in(BendRange, "master.bendrange", "Master", "Bend Range", 0, 24, 2, "st");
    b.fl(Morph, "mix.morph", "Master", "Morph A<>B", -1, 1, 0);

    b.en(P::FilterType, "filter.type", "Filter", "Type", kFilterNames, (int)hsynth::FilterType::LP24);
    b.ex(FilterCutoff, "filter.cutoff", "Filter", "Cutoff", 20, 20000, 8000, "Hz");
    b.fl(FilterReso, "filter.reso", "Filter", "Resonance", 0, 1, 0.15f);
    b.fl(FilterDrive, "filter.drive", "Filter", "Drive", 0, 1, 0);
    b.fl(FilterKeytrack, "filter.keytrack", "Filter", "Key Track", 0, 1, 0.5f);
    b.fl(FilterEnvAmt, "filter.envamt", "Filter", "Env2 Amount", -1, 1, 0.2f);
    b.fl(FilterVelAmt, "filter.velamt", "Filter", "Vel Amount", 0, 1, 0.2f);

    b.ex(AmpAttack, "amp.attack", "Amp Env", "Attack", 0.0005f, 10, 0.003f, "s");
    b.ex(AmpDecay, "amp.decay", "Amp Env", "Decay", 0.001f, 20, 0.4f, "s");
    b.fl(AmpSustain, "amp.sustain", "Amp Env", "Sustain", 0, 1, 0.8f);
    b.ex(AmpRelease, "amp.release", "Amp Env", "Release", 0.001f, 20, 0.3f, "s");
    b.fl(AmpVelSens, "amp.velsens", "Amp Env", "Vel Sens", 0, 1, 0.6f);

    b.ex(Env2Attack, "env2.attack", "Mod Env", "Attack", 0.0005f, 10, 0.003f, "s");
    b.ex(Env2Decay, "env2.decay", "Mod Env", "Decay", 0.001f, 20, 0.5f, "s");
    b.fl(Env2Sustain, "env2.sustain", "Mod Env", "Sustain", 0, 1, 0.3f);
    b.ex(Env2Release, "env2.release", "Mod Env", "Release", 0.001f, 20, 0.4f, "s");

    b.en(Lfo1Wave, "lfo1.wave", "LFO", "LFO1 Wave", kLfoWaveNames, 0);
    b.ex(Lfo1Rate, "lfo1.rate", "LFO", "LFO1 Rate", 0.01f, 40, 5.5f, "Hz");
    b.ex(Lfo1Delay, "lfo1.delay", "LFO", "LFO1 Delay", 0.001f, 5, 0.001f, "s");
    b.bo(Lfo1KeySync, "lfo1.keysync", "LFO", "LFO1 Key Sync", true);
    b.en(Lfo2Wave, "lfo2.wave", "LFO", "LFO2 Wave", kLfoWaveNames, 1);
    b.ex(Lfo2Rate, "lfo2.rate", "LFO", "LFO2 Rate", 0.01f, 40, 0.3f, "Hz");

    b.bo(Limiter, "master.limiter", "Master", "Limiter", true);
    b.fl(P::FilterMorph, "filter.morph", "Filter", "Vowel", 0, 1, 0);
    b.en(P::VfxType, "vfx.type", "Voice FX", "Type", kVfxNames, 0);
    b.fl(P::VfxAmount, "vfx.amount", "Voice FX", "Amount", 0, 1, 0.5f);
    b.ex(P::VfxRatio, "vfx.ratio", "Voice FX", "Ring Ratio", 0.25f, 16, 2, "x");
    b.fl(P::VfxMix, "vfx.mix", "Voice FX", "Mix", 0, 1, 1);
    for (int i = 0; i < 4; ++i)
        b.in(P::CtlCc1 + i, "ctl.cc" + std::to_string(i + 1), "Controllers", "CC" + std::to_string(i + 1) + " #", 0, 127,
             (float)(16 + i));

    buildFx(b);

    for (int s = 0; s < kModSlots; ++s) {
        const std::string n = "mod" + std::to_string(s + 1);
        const std::string g = "Mod Matrix";
        const std::string l = "#" + std::to_string(s + 1) + " ";
        b.en(mod(s, ModSrc), n + ".src", g, l + "Source", kModSourceNames, 0);
        b.en(mod(s, ModVia), n + ".via", g, l + "Via", kModSourceNames, 0);
        b.en(mod(s, ModDst), n + ".dst", g, l + "Dest", kModDestNames, 0);
        b.fl(mod(s, ModAmt), n + ".amt", g, l + "Amount", -1, 1, 0);
    }

    const char* layerNames[3] = {"a", "b", "c"};
    const char* layerLabels[3] = {"Layer A", "Layer B", "Layer C"};
    for (int l = 0; l < 3; ++l) {
        const std::string n = layerNames[l];
        const std::string g = layerLabels[l];
        b.en(layer(l, LType), n + ".type", g, "Type", kLayerTypeNames,
             l == 0 ? (int)LayerType::VA : (int)LayerType::Off);
        b.fl(layer(l, LLevel), n + ".level", g, "Level", 0, 1, 0.8f);
        b.fl(layer(l, LPan), n + ".pan", g, "Pan", -1, 1, 0);
        b.in(layer(l, LCoarse), n + ".coarse", g, "Coarse", -24, 24, 0, "st");
        b.fl(layer(l, LFine), n + ".fine", g, "Fine", -100, 100, 0, "ct");
        b.fl(layer(l, LVelSens), n + ".velsens", g, "Vel->Level", 0, 1, 0);

        const std::string gv = g + " / VA";
        b.en(layer(l, VaWave1), n + ".va.wave1", gv, "Osc1 Wave", kVaWaveNames, 0);
        b.en(layer(l, VaWave2), n + ".va.wave2", gv, "Osc2 Wave", kVaWaveNames, 0);
        b.fl(layer(l, VaPw), n + ".va.pw", gv, "Pulse Width", 0.05f, 0.95f, 0.5f);
        b.in(layer(l, VaOsc2Semi), n + ".va.osc2semi", gv, "Osc2 Semi", -24, 24, 0, "st");
        b.fl(layer(l, VaDetune), n + ".va.detune", gv, "Detune", 0, 50, 7, "ct");
        b.fl(layer(l, VaMix), n + ".va.mix", gv, "Osc1<>Osc2", 0, 1, 0.5f);
        b.fl(layer(l, VaNoise), n + ".va.noise", gv, "Noise", 0, 1, 0);

        const std::string gf = g + " / FM";
        b.en(layer(l, FmAlgo), n + ".fm.algo", gf, "Algorithm", kFmAlgoNames, 0);
        b.fl(layer(l, FmFeedback), n + ".fm.feedback", gf, "Feedback", 0, 1, 0);
        b.fl(layer(l, FmIndex), n + ".fm.index", gf, "Mod Index", 0, 1, 0.5f);
        for (int op = 0; op < kFmOps; ++op) {
            const std::string on = n + ".fm.op" + std::to_string(op + 1);
            const std::string ol = "Op" + std::to_string(op + 1) + " ";
            b.in(fmOp(l, op, OpRatio), on + ".ratio", gf, ol + "Ratio", 0, 32, 1, "x");
            b.fl(fmOp(l, op, OpDetune), on + ".detune", gf, ol + "Detune", -50, 50, 0, "ct");
            b.fl(fmOp(l, op, OpLevel), on + ".level", gf, ol + "Level", 0, 1, op == 0 ? 1.0f : 0.5f);
            b.ex(fmOp(l, op, OpAttack), on + ".attack", gf, ol + "Attack", 0.0005f, 10, 0.002f, "s");
            b.ex(fmOp(l, op, OpDecay), on + ".decay", gf, ol + "Decay", 0.001f, 20, 1.0f, "s");
            b.fl(fmOp(l, op, OpSustain), on + ".sustain", gf, ol + "Sustain", 0, 1, op == 0 ? 0.8f : 0.4f);
            b.ex(fmOp(l, op, OpRelease), on + ".release", gf, ol + "Release", 0.001f, 20, 0.3f, "s");
            b.fl(fmOp(l, op, OpVelSens), on + ".velsens", gf, ol + "Vel Sens", 0, 1, op == 0 ? 0.0f : 0.5f);
        }

        const std::string gp = g + " / PCM";
        b.fl(layer(l, PcmStart), n + ".pcm.start", gp, "Start Offset", 0, 1, 0);
        b.fl(layer(l, PcmVelTrack), n + ".pcm.veltrack", gp, "SFZ Vel Track", 0, 1, 1.0f);

        const std::string gw = g + " / WT";
        std::vector<std::string> tables = builtinWavetableNames();
        tables.push_back("user");
        b.en(layer(l, WtTable), n + ".wt.table", gw, "Table", tables, 0);
        b.fl(layer(l, WtPos), n + ".wt.pos", gw, "Position", 0, 1, 0);
        b.fl(layer(l, WtPosEnv), n + ".wt.posenv", gw, "Env2 > Position", -1, 1, 0);
        b.en(layer(l, WtWarpMode), n + ".wt.warp", gw, "Warp", {"none", "sync", "bend", "mirror", "squeeze"}, 0);
        b.fl(layer(l, WtWarpAmt), n + ".wt.warpamt", gw, "Warp Amount", 0, 1, 0);
        b.in(layer(l, WtUnison), n + ".wt.unison", gw, "Unison", 1, 7, 1);
        b.fl(layer(l, WtDetune), n + ".wt.detune", gw, "Unison Detune", 0, 100, 12, "ct");
        b.fl(layer(l, WtSpread), n + ".wt.spread", gw, "Unison Spread", 0, 1, 0.7f);
    }

    // FX modulation destinations: every continuous or switch parameter of the
    // effect blocks, by name (stable in presets).
    std::vector<std::string> dstNames = {"none"};
    fxTargets().assign(1, -1);
    for (int id = kFxBlockBase; id < kNumParams; ++id) {
        const ParamInfo& p = b.t[(size_t)id];
        if (p.kind == ParamKind::Float || p.kind == ParamKind::Bool) {
            dstNames.push_back(p.name);
            fxTargets().push_back(id);
        }
    }
    for (int s = 0; s < fx::kFxModSlots; ++s) {
        const std::string n = "fxmod" + std::to_string(s + 1);
        const std::string l = "#" + std::to_string(s + 1) + " ";
        b.en(fxMod(s, FxModSrc), n + ".src", "FX Mod", l + "Source", kFxModSourceNames, 0);
        b.en(fxMod(s, FxModDst), n + ".dst", "FX Mod", l + "Target", dstNames, 0);
        b.fl(fxMod(s, FxModAmt), n + ".amt", "FX Mod", l + "Amount", -1, 1, 0);
    }

    // Default modulation: mod wheel adds LFO1 vibrato, per-note timbre opens
    // the filter, per-note pressure adds level.
    b.t[mod(0, ModSrc)].def = (float)ModSource::Lfo1;
    b.t[mod(0, ModVia)].def = (float)ModSource::ModWheel;
    b.t[mod(0, ModDst)].def = (float)ModDest::Pitch;
    b.t[mod(0, ModAmt)].def = 0.02f;
    b.t[mod(1, ModSrc)].def = (float)ModSource::NoteTimbre;
    b.t[mod(1, ModDst)].def = (float)ModDest::Cutoff;
    b.t[mod(1, ModAmt)].def = 0.3f;
    b.t[mod(2, ModSrc)].def = (float)ModSource::ChannelPressure;
    b.t[mod(2, ModDst)].def = (float)ModDest::Cutoff;
    b.t[mod(2, ModAmt)].def = 0.2f;
    return b.t;
}

}  // namespace

const std::vector<ParamInfo>& paramTable() {
    static const std::vector<ParamInfo> table = build();
    return table;
}

const std::vector<int>& fxModTargets() {
    paramTable();  // filled while the table is built
    return fxTargets();
}

namespace fx {
const char* blockKey(int block) { return block >= 0 && block < BlockCount ? kBlockKeys[block] : "none"; }
const char* blockTitle(int block) { return block >= 0 && block < BlockCount ? kBlockTitles[block] : "None"; }
}  // namespace fx

int findParam(const std::string& name) {
    static const std::unordered_map<std::string, int> index = [] {
        std::unordered_map<std::string, int> m;
        const auto& t = paramTable();
        for (int i = 0; i < (int)t.size(); ++i) m[t[i].name] = i;
        return m;
    }();
    auto it = index.find(name);
    return it == index.end() ? -1 : it->second;
}

void defaultParams(float* values) {
    const auto& t = paramTable();
    for (int i = 0; i < P::kNumParams; ++i) values[i] = t[i].def;
}

float sanitizeParam(int id, float value) {
    const auto& t = paramTable();
    if (id < 0 || id >= (int)t.size()) return value;
    const ParamInfo& p = t[id];
    if (!std::isfinite(value)) value = p.def;
    if (value < p.min) value = p.min;
    if (value > p.max) value = p.max;
    if (p.kind != ParamKind::Float) value = std::round(value);
    return value;
}

const std::vector<ParamInfo>& partMixTable() {
    static const std::vector<ParamInfo> table = [] {
        std::vector<ParamInfo> t((size_t)Mix::Count);
        auto set = [&](int id, const char* name, const char* label, ParamKind kind, float mn, float mx, float def,
                       const char* unit = "") {
            ParamInfo& p = t[(size_t)id];
            p.name = name; p.group = "Part"; p.label = label; p.kind = kind;
            p.min = mn; p.max = mx; p.def = def; p.unit = unit;
        };
        set(Mix::Volume, "volume", "Volume", ParamKind::Float, 0, 1, 1);
        set(Mix::Pan, "pan", "Pan", ParamKind::Float, -1, 1, 0);
        set(Mix::Mute, "mute", "Mute", ParamKind::Bool, 0, 1, 0);
        set(Mix::Channel, "channel", "MIDI Ch", ParamKind::Int, 1, 16, 1);
        set(Mix::KeyLow, "key.low", "Key Lo", ParamKind::Int, 0, 127, 0);
        set(Mix::KeyHigh, "key.high", "Key Hi", ParamKind::Int, 0, 127, 127);
        set(Mix::Transpose, "transpose", "Transpose", ParamKind::Int, -48, 48, 0, "st");
        set(Mix::FxSend, "fx.send", "FX Send", ParamKind::Float, 0, 1, 1);
        return t;
    }();
    return table;
}

void defaultPartMix(int part, float* values) {
    const auto& t = partMixTable();
    for (int i = 0; i < Mix::Count; ++i) values[i] = t[(size_t)i].def;
    values[Mix::Channel] = (float)(1 + std::max(0, std::min(kNumParts - 1, part)) % 16);
}

float sanitizePartMix(int id, float value) {
    if (id < 0 || id >= Mix::Count) return 0.0f;
    const ParamInfo& p = partMixTable()[(size_t)id];
    if (!std::isfinite(value)) value = p.def;
    value = std::min(p.max, std::max(p.min, value));
    if (p.kind == ParamKind::Bool) return value > 0.5f ? 1.0f : 0.0f;
    if (p.kind == ParamKind::Int) return std::round(value);
    return value;
}

}  // namespace hsynth
