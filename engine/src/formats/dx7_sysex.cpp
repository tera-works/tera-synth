#include "hsynth/formats/dx7_sysex.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hsynth/common.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/fileio.h"
#include "hsynth/params.h"

namespace hsynth {

namespace {

// Unpacked DX7 voice (155 bytes, OP6..OP1 stored at offsets 0..105).
struct Dx7Op {
    int r[4] = {99, 99, 99, 99};
    int l[4] = {99, 99, 99, 0};
    int breakPoint = 39;
    int leftDepth = 0;
    int rightDepth = 0;
    int leftCurve = 0;
    int rightCurve = 0;
    int rateScale = 0;
    int ams = 0;
    int velSens = 0;
    int level = 0;
    int mode = 0;     // 0 = ratio, 1 = fixed
    int coarse = 1;   // 0..31
    int fine = 0;     // 0..99
    int detune = 7;   // 0..14 (7 = center)
};

struct Dx7Voice {
    Dx7Op op[6];      // op[0] = DX7 OP1 .. op[5] = DX7 OP6
    int pegR[4] = {99, 99, 99, 99};
    int pegL[4] = {50, 50, 50, 50};
    int algorithm = 0;  // 0..31 (DX7 1..32)
    int feedback = 0;   // 0..7
    int oscKeySync = 1;
    int lfoSpeed = 35;
    int lfoDelay = 0;
    int lfoPmd = 0;
    int lfoAmd = 0;
    int lfoKeySync = 1;
    int lfoWave = 0;
    int lfoPms = 3;
    int transpose = 24;  // 24 = C3 (0 semitones)
    std::string name;
};

std::string cleanName(const uint8_t* p, size_t len) {
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = p[i] & 0x7Fu;
        s.push_back((c >= 32 && c < 127) ? (char)c : ' ');
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t lead = 0;
    while (lead < s.size() && s[lead] == ' ') ++lead;
    return s.substr(lead);
}

Dx7Voice unpackVced(const uint8_t* d) {
    Dx7Voice v;
    // DX7 stores OP6 at index 0 .. OP1 at index 5 (21 bytes each)
    for (int k = 0; k < 6; ++k) {
        const int opIdx = 5 - k;
        const uint8_t* p = d + (size_t)k * 21;
        Dx7Op& o = v.op[opIdx];
        for (int i = 0; i < 4; ++i) {
            o.r[i] = std::min(99, (int)(p[i] & 0x7F));
            o.l[i] = std::min(99, (int)(p[4 + i] & 0x7F));
        }
        o.breakPoint = std::min(99, (int)(p[8] & 0x7F));
        o.leftDepth = std::min(99, (int)(p[9] & 0x7F));
        o.rightDepth = std::min(99, (int)(p[10] & 0x7F));
        o.leftCurve = p[11] & 0x03;
        o.rightCurve = p[12] & 0x03;
        o.rateScale = p[13] & 0x07;
        o.ams = p[14] & 0x03;
        o.velSens = p[15] & 0x07;
        o.level = std::min(99, (int)(p[16] & 0x7F));
        o.mode = p[17] & 0x01;
        o.coarse = p[18] & 0x1F;
        o.fine = std::min(99, (int)(p[19] & 0x7F));
        o.detune = std::min(14, (int)(p[20] & 0x0F));
    }
    for (int i = 0; i < 4; ++i) {
        v.pegR[i] = std::min(99, (int)(d[126 + i] & 0x7F));
        v.pegL[i] = std::min(99, (int)(d[130 + i] & 0x7F));
    }
    v.algorithm = d[134] & 0x1F;
    v.feedback = d[135] & 0x07;
    v.oscKeySync = d[136] & 0x01;
    v.lfoSpeed = std::min(99, (int)(d[137] & 0x7F));
    v.lfoDelay = std::min(99, (int)(d[138] & 0x7F));
    v.lfoPmd = std::min(99, (int)(d[139] & 0x7F));
    v.lfoAmd = std::min(99, (int)(d[140] & 0x7F));
    v.lfoKeySync = d[141] & 0x01;
    v.lfoWave = std::min(5, (int)(d[142] & 0x07));
    v.lfoPms = d[143] & 0x07;
    v.transpose = std::min(48, (int)(d[144] & 0x7F));
    v.name = cleanName(d + 145, 10);
    return v;
}

Dx7Voice unpackVmemVoice(const uint8_t* d) {
    Dx7Voice v;
    for (int k = 0; k < 6; ++k) {
        const int opIdx = 5 - k;
        const uint8_t* p = d + (size_t)k * 17;
        Dx7Op& o = v.op[opIdx];
        for (int i = 0; i < 4; ++i) {
            o.r[i] = std::min(99, (int)(p[i] & 0x7F));
            o.l[i] = std::min(99, (int)(p[4 + i] & 0x7F));
        }
        o.breakPoint = std::min(99, (int)(p[8] & 0x7F));
        o.leftDepth = std::min(99, (int)(p[9] & 0x7F));
        o.rightDepth = std::min(99, (int)(p[10] & 0x7F));
        o.leftCurve = p[11] & 0x03;
        o.rightCurve = (p[11] >> 2) & 0x03;
        o.rateScale = p[12] & 0x07;
        o.detune = std::min(14, (int)((p[12] >> 3) & 0x0F));
        o.ams = p[13] & 0x03;
        o.velSens = (p[13] >> 2) & 0x07;
        o.level = std::min(99, (int)(p[14] & 0x7F));
        o.mode = p[15] & 0x01;
        o.coarse = (p[15] >> 1) & 0x1F;
        o.fine = std::min(99, (int)(p[16] & 0x7F));
    }
    for (int i = 0; i < 4; ++i) {
        v.pegR[i] = std::min(99, (int)(d[102 + i] & 0x7F));
        v.pegL[i] = std::min(99, (int)(d[106 + i] & 0x7F));
    }
    v.algorithm = d[110] & 0x1F;
    v.feedback = d[111] & 0x07;
    v.oscKeySync = (d[111] >> 3) & 0x01;
    v.lfoSpeed = std::min(99, (int)(d[112] & 0x7F));
    v.lfoDelay = std::min(99, (int)(d[113] & 0x7F));
    v.lfoPmd = std::min(99, (int)(d[114] & 0x7F));
    v.lfoAmd = std::min(99, (int)(d[115] & 0x7F));
    v.lfoKeySync = d[116] & 0x01;
    v.lfoWave = std::min(5, (int)((d[116] >> 1) & 0x07));
    v.lfoPms = (d[116] >> 4) & 0x07;
    v.transpose = std::min(48, (int)(d[117] & 0x7F));
    v.name = cleanName(d + 118, 10);
    return v;
}

// Lookup table mapping ALL 32 DX7 algorithms (index 0..31 -> DX7 1..32) to
// HSynth kFmAlgorithms indices (0..87) plus a 6-operator permutation array:
// opMap[dx7Op 0..5] = hsynthOp 0..7 (unused HSynth ops are silenced at OpLevel = 0).
//
// HSynth algorithm index reference (see engine/src/fm.cpp):
//   8  : 6op  6>5>4>3 + 2>1 (fb 6)
//   9  : 6op  6>5>4>3 + 2>1 (fb 2)
//  10  : 6op  6>5 + 4>3 + 2>1 (fb 6)
//  11  : 6op  6>5>3, 4>3, 2>1 (fb 6)
//  12  : 6op  (2 + 4>3 + 6>5)>1 (fb 6)
//  13  : 6op  6>(5,4,3) + 2>1 (fb 6)
//  14  : 6op  6>5 + 1..4 (fb 6)
//  15  : 6op  1+2+3+4+5+6 (fb 6)
//  20  : 8op  8>7>6 + 5>4>3 + 2>1 (fb 8)
//  26  : 8op  8>7 + 6>5 + 4>3 + 2 + 1 (fb 8)
//  29  : 8op  8>7>6 + 5>4>3 + 2 + 1 (fb 8)
//  31  : 8op  8>7>6 + 5>4 + 3>2 + 1 (fb 8)
//  34  : 8op  8>7>6>1 + 5>4>3>1 + 2>1 (fb 8)
//  39  : 8op  8>(7,6,5,4) + 3>2>1 (fb 8)
//  46  : 8op  (8,7)>6>5 + (4,3)>2>1 (fb 8)
//  54  : 8op  8>(7,6) + 5>(4,3) + 2>1 (fb 8)
//  59  : 8op  8>(7,6,5) + 4>(3,2,1) (fb 8)
//  62  : 8op  (8,7,6)>5 + 4>3>2>1 (fb 8)
//  64  : 8op  (8,7)>4 + (6,5)>3 + 2>1 (fb 8)
//  85  : 8op  (8,7)>(6,5,4) + 3 + 2 + 1 (fb 8)
struct AlgoMapEntry {
    int hsynthAlgo;
    int opMap[6];  // DX7 OP1..OP6 (0..5) -> HSynth op slot (0..7)
};

constexpr AlgoMapEntry kDx7AlgoTable[32] = {
    {8,  {0, 1, 2, 3, 4, 5}},  // DX7  1: 6(fb)>5>4>3 + 2>1          -> exact 6-op #8
    {9,  {0, 1, 2, 3, 4, 5}},  // DX7  2: 6>5>4>3 + 2(fb)>1          -> exact 6-op #9
    {20, {2, 3, 4, 5, 6, 7}},  // DX7  3: 6(fb)>5>4 + 3>2>1          -> 8-op #20 (8>7>6 + 5>4>3, ops 1..2 off)
    {20, {2, 3, 4, 5, 6, 7}},  // DX7  4: 6>5>4 + 3>2>1 (4->6 fb)    -> 8-op #20 (approx fb on top op)
    {10, {0, 1, 2, 3, 4, 5}},  // DX7  5: 6(fb)>5 + 4>3 + 2>1        -> exact 6-op #10
    {10, {0, 1, 2, 3, 4, 5}},  // DX7  6: 6>5 + 4>3 + 2>1 (5->6 fb)  -> 6-op #10 (approx fb on 6)
    {11, {0, 1, 2, 3, 4, 5}},  // DX7  7: (6(fb)>5 + 4)>3 + 2>1      -> exact 6-op #11
    {11, {0, 1, 2, 3, 4, 5}},  // DX7  8: (6>5 + 4(fb))>3 + 2>1      -> 6-op #11 (approx fb on 6)
    {11, {0, 1, 2, 3, 4, 5}},  // DX7  9: (6>5 + 4)>3 + 2(fb)>1      -> 6-op #11 (approx fb on 6)
    {46, {4, 5, 6, 0, 1, 7}},  // DX7 10: (6+5)>4 + 3(fb)>2>1        -> 8-op #46 (mapped carriers 5,1)
    {46, {0, 1, 4, 5, 6, 7}},  // DX7 11: (6(fb)+5)>4 + 3>2>1        -> 8-op #46 (mapped carriers 5,1)
    {62, {0, 1, 4, 5, 6, 7}},  // DX7 12: (6+5+4)>3 + 2(fb)>1        -> 8-op #62 ((8,7,6)>5 + 2>1, ops 3..4 off)
    {62, {0, 1, 4, 5, 6, 7}},  // DX7 13: (6(fb)+5+4)>3 + 2>1        -> exact topology 8-op #62
    {46, {0, 1, 4, 5, 6, 7}},  // DX7 14: (6(fb)+5)>4>3 + 2>1        -> exact topology 8-op #46 (ops 3..4 off)
    {46, {0, 1, 4, 5, 6, 7}},  // DX7 15: (6+5)>4>3 + 2(fb)>1        -> 8-op #46 (ops 3..4 off)
    {12, {0, 1, 2, 3, 4, 5}},  // DX7 16: (6(fb)>5 + 4>3 + 2)>1      -> exact 6-op #12
    {12, {0, 1, 2, 3, 4, 5}},  // DX7 17: (6>5 + 4>3 + 2(fb))>1      -> 6-op #12 (approx fb on 6)
    {34, {0, 1, 2, 5, 6, 7}},  // DX7 18: (6>5>4 + 3(fb) + 2)>1      -> 8-op #34 (8>7>6>1 + 3>1 + 2>1, ops 4..5 off)
    {39, {0, 1, 2, 5, 6, 7}},  // DX7 19: 6(fb)>(5,4) + 3>2>1        -> exact topology 8-op #39 (ops 4..5 off)
    {64, {0, 1, 2, 3, 6, 7}},  // DX7 20: (6(fb)+5)>4 + 3>(2,1)      -> 8-op #64 ((8,7)>4 + (6,5)>3 + 2>1)
    {59, {1, 2, 3, 5, 6, 7}},  // DX7 21: 6(fb)>(5,4) + 3>(2,1)      -> exact topology 8-op #59 (ops 1,5 off)
    {13, {0, 1, 2, 3, 4, 5}},  // DX7 22: 6(fb)>(5,4,3) + 2>1        -> exact 6-op #13
    {54, {0, 2, 4, 5, 6, 7}},  // DX7 23: 6(fb)>(5,4) + 3>2 + 1      -> exact topology 8-op #54 (ops 2,4 off)
    {85, {0, 1, 3, 4, 5, 7}},  // DX7 24: 6(fb)>(5,4,3) + 2 + 1      -> exact topology 8-op #85 (ops 3,7 off)
    {85, {0, 1, 2, 4, 5, 7}},  // DX7 25: 6(fb)>(5,4) + 3 + 2 + 1    -> exact topology 8-op #85 (ops 4,7 off)
    {64, {0, 2, 5, 3, 6, 7}},  // DX7 26: (6(fb)+5)>4 + 3>2 + 1      -> exact topology 8-op #64 (ops 2,5 off)
    {64, {0, 2, 5, 3, 6, 7}},  // DX7 27: (6+5)>4 + 3(fb)>2 + 1      -> 8-op #64 (ops 2,5 off)
    {31, {0, 3, 4, 5, 6, 7}},  // DX7 28: 6>5>4 + 3(fb)>2 + 1        -> 8-op #31 (ops 2,3 off)
    {26, {0, 1, 4, 5, 6, 7}},  // DX7 29: 6(fb)>5 + 4>3 + 2 + 1      -> exact topology 8-op #26 (ops 3,4 off)
    {29, {0, 1, 2, 5, 6, 7}},  // DX7 30: 6(fb)>5>4 + 3 + 2 + 1      -> exact topology 8-op #29 (ops 4,5 off)
    {14, {0, 1, 2, 3, 4, 5}},  // DX7 31: 6(fb)>5 + 4 + 3 + 2 + 1    -> exact 6-op #14
    {15, {0, 1, 2, 3, 4, 5}},  // DX7 32: 6(fb) + 5 + 4 + 3 + 2 + 1  -> exact 6-op #15
};

// DX7 output level (0..99) -> linear 0..1 (~0.75 dB per step from 99 down).
float dx7LevelToGain(int level) {
    if (level <= 0) return 0.0f;
    if (level >= 99) return 1.0f;
    float g = std::exp2((float)(level - 99) / 8.0f);
    if (level < 20) g *= (float)level / 20.0f;
    return clampf(g, 0.0f, 1.0f);
}

// DX7 EG rate (0..99) + level delta (0..99) -> approximate seconds.
float dx7RateToSeconds(int rate, int deltaLevel, float minSec, float maxSec) {
    const float base = 0.0005f * std::exp2((float)(99 - std::min(99, std::max(0, rate))) * 0.145f);
    const float span = std::max(0.08f, (float)std::abs(deltaLevel) / 99.0f);
    return clampf(base * span, minSec, maxSec);
}

Preset dx7VoiceToPreset(const Dx7Voice& v, int index) {
    Preset p;
    p.name = v.name.empty() ? ("DX7 Voice " + std::to_string(index + 1)) : v.name;
    p.category = "DX7 FM";
    p.tags = {"DX7", "FM"};

    const AlgoMapEntry& entry = kDx7AlgoTable[v.algorithm & 31];
    const FmAlgorithm& hAlgo = fmAlgorithm(entry.hsynthAlgo);

    // Silence global subtractive filter and voice-level amp env (per-op FM EGs govern envelope).
    p.params[(size_t)P::FilterType] = (float)FilterType::Off;
    p.params[(size_t)P::AmpAttack] = 0.0005f;
    p.params[(size_t)P::AmpDecay] = 0.001f;
    p.params[(size_t)P::AmpSustain] = 1.0f;
    p.params[(size_t)P::AmpRelease] = 20.0f;
    p.params[(size_t)P::AmpVelSens] = 0.0f;

    // Count active vs algorithm carriers so level matches when unused 8-op slots exist.
    int algoCarriers = 0;
    for (int i = 0; i < hAlgo.numOps; ++i)
        if (hAlgo.carriers & (1 << i)) ++algoCarriers;
    int activeCarriers = 0;
    for (int dx = 0; dx < 6; ++dx)
        if (hAlgo.carriers & (1 << entry.opMap[dx])) ++activeCarriers;
    const float carrierComp = (activeCarriers > 0 && algoCarriers > activeCarriers)
                                  ? (float)algoCarriers / (float)activeCarriers
                                  : 1.0f;

    p.params[(size_t)P::layer(0, P::LType)] = (float)LayerType::FM;
    p.params[(size_t)P::layer(0, P::LLevel)] = clampf(0.8f * carrierComp, 0.0f, 1.0f);
    p.params[(size_t)P::layer(0, P::LCoarse)] = clampf((float)(v.transpose - 24), -24.0f, 24.0f);
    p.params[(size_t)P::layer(0, P::FmAlgo)] = (float)entry.hsynthAlgo;
    p.params[(size_t)P::layer(0, P::FmFeedback)] = clampf((float)v.feedback / 7.0f, 0.0f, 1.0f);
    p.params[(size_t)P::layer(0, P::FmIndex)] = 0.85f;

    // Pitch EG
    const int pegPeakDiff = std::abs(v.pegL[0] - v.pegL[2]) >= std::abs(v.pegL[1] - v.pegL[2])
                                ? (v.pegL[0] - 50)
                                : (v.pegL[1] - 50);
    p.params[(size_t)P::layer(0, P::FmPegAmt)] = clampf((float)pegPeakDiff * 0.5f, -24.0f, 24.0f);
    p.params[(size_t)P::layer(0, P::FmPegTime)] = dx7RateToSeconds(v.pegR[0], std::abs(pegPeakDiff), 0.001f, 10.0f);

    // Zero all 8 HSynth operators first (disables Op7/Op8 and any unmapped slots).
    for (int op = 0; op < P::kFmOps; ++op) {
        p.params[(size_t)P::fmOp(0, op, P::OpLevel)] = 0.0f;
        p.params[(size_t)P::fmOp(0, op, P::OpAttack)] = 0.001f;
        p.params[(size_t)P::fmOp(0, op, P::OpDecay)] = 0.01f;
        p.params[(size_t)P::fmOp(0, op, P::OpSustain)] = 0.0f;
        p.params[(size_t)P::fmOp(0, op, P::OpRelease)] = 0.01f;
    }

    // LFO
    static constexpr LfoWave kWaveMap[6] = {
        LfoWave::Triangle, LfoWave::Saw, LfoWave::Saw, LfoWave::Square, LfoWave::Sine, LfoWave::SampleHold};
    p.params[(size_t)P::Lfo1Wave] = (float)kWaveMap[std::min(5, v.lfoWave)];
    p.params[(size_t)P::Lfo1Rate] = clampf(0.06f * std::exp2((float)v.lfoSpeed * 0.085f), 0.01f, 40.0f);
    p.params[(size_t)P::Lfo1Delay] = clampf((float)v.lfoDelay / 99.0f * 3.0f, 0.001f, 5.0f);
    p.params[(size_t)P::Lfo1KeySync] = v.lfoKeySync ? 1.0f : 0.0f;

    if (v.lfoPmd > 0 && v.lfoPms > 0) {
        const float depthSemis = ((float)v.lfoPmd / 99.0f) * ((float)v.lfoPms / 7.0f) * 1.5f;
        p.params[(size_t)P::mod(0, P::ModSrc)] = (float)ModSource::Lfo1;
        p.params[(size_t)P::mod(0, P::ModDst)] = (float)ModDest::Pitch;
        p.params[(size_t)P::mod(0, P::ModAmt)] = clampf(depthSemis / 24.0f, -1.0f, 1.0f);
    }

    // Operators: DX7 OP1..OP6 -> mapped HSynth operator slots
    for (int dx = 0; dx < 6; ++dx) {
        const Dx7Op& o = v.op[dx];
        const int dst = entry.opMap[dx];

        const float baseRatio = o.coarse == 0 ? 0.5f : (float)o.coarse;
        const float fineMul = 1.0f + (float)o.fine / 100.0f;
        const float dtCents = (float)(o.detune - 7) * 1.5f;

        if (o.mode == 1) {
            const float hz = std::pow(10.0f, (float)(o.coarse % 4)) * std::pow(10.0f, (float)o.fine / 100.0f);
            p.params[(size_t)P::fmOp(0, dst, P::OpFixed)] = clampf(hz, 1.0f, 8000.0f);
            p.params[(size_t)P::fmOp(0, dst, P::OpRatio)] = 1.0f;
            p.params[(size_t)P::fmOp(0, dst, P::OpDetune)] = clampf(dtCents, -50.0f, 50.0f);
        } else {
            p.params[(size_t)P::fmOp(0, dst, P::OpFixed)] = 0.0f;
            const float exactRatio = baseRatio * fineMul;
            int outCoarse = o.coarse;
            float fineCents = 1200.0f * std::log2(fineMul);
            if (o.fine > 0) {
                if (exactRatio < 0.707f) {
                    outCoarse = 0;
                    fineCents = 1200.0f * std::log2(exactRatio / 0.5f);
                } else {
                    outCoarse = std::max(1, std::min(32, (int)std::lround(exactRatio)));
                    fineCents = 1200.0f * std::log2(exactRatio / (float)outCoarse);
                }
            }
            p.params[(size_t)P::fmOp(0, dst, P::OpRatio)] = (float)outCoarse;
            p.params[(size_t)P::fmOp(0, dst, P::OpDetune)] = clampf(dtCents + fineCents, -50.0f, 50.0f);
        }

        p.params[(size_t)P::fmOp(0, dst, P::OpLevel)] = dx7LevelToGain(o.level);
        p.params[(size_t)P::fmOp(0, dst, P::OpWave)] = (float)FmWave::Sine;

        // 4-stage EG -> ADSR
        const float atk = dx7RateToSeconds(o.r[0], o.l[0] - o.l[3], 0.0005f, 10.0f);
        const float d2 = dx7RateToSeconds(o.r[1], o.l[0] - o.l[1], 0.001f, 20.0f);
        const float d3 = dx7RateToSeconds(o.r[2], o.l[1] - o.l[2], 0.001f, 20.0f);
        const float dec = clampf(d2 + d3, 0.001f, 20.0f);
        const float sus = (o.l[0] == 0 && o.l[1] == 0 && o.l[2] == 0) ? 0.0f : dx7LevelToGain(o.l[2]);
        const float rel = dx7RateToSeconds(o.r[3], o.l[2] - o.l[3], 0.001f, 20.0f);

        p.params[(size_t)P::fmOp(0, dst, P::OpAttack)] = atk;
        p.params[(size_t)P::fmOp(0, dst, P::OpDecay)] = dec;
        p.params[(size_t)P::fmOp(0, dst, P::OpSustain)] = sus;
        p.params[(size_t)P::fmOp(0, dst, P::OpRelease)] = rel;

        p.params[(size_t)P::fmOp(0, dst, P::OpVelSens)] = clampf((float)o.velSens / 7.0f, 0.0f, 1.0f);
        p.params[(size_t)P::fmOp(0, dst, P::OpRateScale)] = clampf((float)o.rateScale / 7.0f, 0.0f, 1.0f);
        p.params[(size_t)P::fmOp(0, dst, P::OpAms)] =
            clampf(((float)o.ams / 3.0f) * ((float)v.lfoAmd / 99.0f), 0.0f, 1.0f);

        // Keyboard level scaling -> approximate dB/oct slope
        const float rightSlope = ((o.rightCurve >= 2) ? 1.0f : -1.0f) * ((float)o.rightDepth / 99.0f) * 12.0f;
        const float leftSlope = ((o.leftCurve >= 2) ? -1.0f : 1.0f) * ((float)o.leftDepth / 99.0f) * 12.0f;
        p.params[(size_t)P::fmOp(0, dst, P::OpKeyScale)] = clampf(0.5f * (rightSlope + leftSlope), -12.0f, 12.0f);
    }

    return p;
}

}  // namespace

bool parseDx7Sysex(const uint8_t* data, size_t size, std::vector<Preset>& outPresets, std::string& error,
                   std::vector<Dx7VoiceSummary>* summaries) {
    outPresets.clear();
    if (summaries) summaries->clear();
    if (!data || size == 0) {
        error = "empty DX7 SysEx buffer";
        return false;
    }
    const auto addVoice = [&](const Dx7Voice& voice) {
        const int index = static_cast<int>(outPresets.size());
        outPresets.push_back(dx7VoiceToPreset(voice, index));
        if (summaries) summaries->push_back({index, outPresets.back().name, (voice.algorithm & 31) + 1});
    };

    // Headerless raw dumps
    if (size == 4096) {
        for (int i = 0; i < 32; ++i)
            addVoice(unpackVmemVoice(data + (size_t)i * 128));
        return true;
    }
    if (size == 155) {
        addVoice(unpackVced(data));
        return true;
    }

    // Scan for Yamaha DX7 SysEx messages (F0 43 ... F7)
    size_t pos = 0;
    while (pos < size) {
        if (data[pos] != 0xF0) {
            ++pos;
            continue;
        }
        size_t end = pos + 1;
        while (end < size && data[end] != 0xF7) ++end;
        if (end >= size) break;

        const size_t msgLen = end - pos + 1;  // includes F0 and F7
        const uint8_t* m = data + pos;
        if (msgLen >= 8 && m[1] == 0x43 && (m[2] & 0xF0) == 0x00) {
            const uint8_t format = m[3];
            if (format == 0x09 && msgLen >= 4104) {
                // 32-voice VMEM (F0 43 0x 09 20 00 + 4096 + csum + F7)
                const uint8_t* payload = m + 6;
                for (int i = 0; i < 32; ++i)
                    addVoice(unpackVmemVoice(payload + (size_t)i * 128));
            } else if (format == 0x00 && msgLen >= 163) {
                // 1-voice VCED (F0 43 0x 00 01 1B + 155 + csum + F7)
                const uint8_t* payload = m + 6;
                addVoice(unpackVced(payload));
            }
        }
        pos = end + 1;
    }

    if (outPresets.empty()) {
        error = "unrecognized DX7 SysEx data (expected VMEM 4104/4096 bytes or VCED 163/155 bytes)";
        return false;
    }
    return true;
}

bool loadDx7SysexFile(const std::string& path, std::vector<Preset>& outPresets, std::string& error,
                     std::vector<Dx7VoiceSummary>* summaries) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "rb"), &std::fclose);
    if (!f) {
        error = "cannot open DX7 SysEx file: " + path;
        return false;
    }
    std::fseek(f.get(), 0, SEEK_END);
    const long sz = std::ftell(f.get());
    if (sz <= 0) {
        error = "empty DX7 SysEx file: " + path;
        return false;
    }
    std::rewind(f.get());
    std::vector<uint8_t> bytes((size_t)sz);
    if (std::fread(bytes.data(), 1, bytes.size(), f.get()) != bytes.size()) {
        error = "failed to read DX7 SysEx file: " + path;
        return false;
    }
    return parseDx7Sysex(bytes.data(), bytes.size(), outPresets, error, summaries);
}

}  // namespace hsynth
