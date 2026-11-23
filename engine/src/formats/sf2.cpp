#include "hsynth/formats/sf2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hsynth/common.h"
#include "hsynth/fileio.h"

namespace hsynth {

namespace {

inline uint16_t readU16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

inline int16_t readI16(const uint8_t* p) {
    return (int16_t)readU16(p);
}

inline uint32_t readU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

std::string readFixedName(const uint8_t* p, size_t maxLen) {
    size_t n = 0;
    while (n < maxLen && p[n] != '\0') ++n;
    while (n > 0 && p[n - 1] == ' ') --n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "rb"), &std::fclose);
    if (!f) {
        error = "cannot open SF2/SF3 file: " + path;
        return false;
    }
    if (std::fseek(f.get(), 0, SEEK_END) != 0) {
        error = "cannot seek SF2/SF3 file: " + path;
        return false;
    }
    const long sz = std::ftell(f.get());
    if (sz < 12) {
        error = "file too small to be SF2/SF3: " + path;
        return false;
    }
    std::rewind(f.get());
    out.resize((size_t)sz);
    if (std::fread(out.data(), 1, out.size(), f.get()) != out.size()) {
        error = "failed to read SF2/SF3 file: " + path;
        return false;
    }
    return true;
}

struct ChunkSlice {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct Phdr {
    std::string name;
    uint16_t preset = 0;
    uint16_t bank = 0;
    uint16_t bagNdx = 0;
};

struct Bag {
    uint16_t genNdx = 0;
    uint16_t modNdx = 0;
};

struct Gen {
    uint16_t oper = 0;
    uint16_t u16 = 0;
    int16_t i16 = 0;
    uint8_t lo = 0;
    uint8_t hi = 0;
};

struct Inst {
    std::string name;
    uint16_t bagNdx = 0;
};

struct Shdr {
    std::string name;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t startLoop = 0;
    uint32_t endLoop = 0;
    uint32_t sampleRate = 44100;
    uint8_t originalKey = 60;
    int8_t correction = 0;
    uint16_t sampleLink = 0;
    uint16_t sampleType = 1;
};

struct Sf2File {
    std::vector<uint8_t> raw;
    ChunkSlice smpl;
    ChunkSlice sm24;
    std::vector<Phdr> phdrs;
    std::vector<Bag> pbags;
    std::vector<Gen> pgens;
    std::vector<Inst> insts;
    std::vector<Bag> ibags;
    std::vector<Gen> igens;
    std::vector<Shdr> shdrs;
};

bool parsePdtaSubchunks(const uint8_t* data, size_t size, Sf2File& sf, std::string& error) {
    ChunkSlice phdr, pbag, pgen, inst, ibag, igen, shdr;
    for (size_t pos = 0; pos + 8 <= size;) {
        const uint32_t csize = readU32(data + pos + 4);
        const size_t bodyPos = pos + 8;
        const size_t avail = bodyPos <= size ? std::min<size_t>(csize, size - bodyPos) : 0;
        const ChunkSlice slice{data + bodyPos, avail};
        if (std::memcmp(data + pos, "phdr", 4) == 0) phdr = slice;
        else if (std::memcmp(data + pos, "pbag", 4) == 0) pbag = slice;
        else if (std::memcmp(data + pos, "pgen", 4) == 0) pgen = slice;
        else if (std::memcmp(data + pos, "inst", 4) == 0) inst = slice;
        else if (std::memcmp(data + pos, "ibag", 4) == 0) ibag = slice;
        else if (std::memcmp(data + pos, "igen", 4) == 0) igen = slice;
        else if (std::memcmp(data + pos, "shdr", 4) == 0) shdr = slice;
        pos = bodyPos + (size_t)csize + (csize & 1u);
    }
    if (!phdr.data || !pbag.data || !pgen.data || !inst.data || !ibag.data || !igen.data || !shdr.data) {
        error = "missing required pdta subchunks in SF2/SF3";
        return false;
    }

    const size_t nPhdr = phdr.size / 38;
    sf.phdrs.reserve(nPhdr);
    for (size_t i = 0; i < nPhdr; ++i) {
        const uint8_t* p = phdr.data + i * 38;
        Phdr h;
        h.name = readFixedName(p, 20);
        h.preset = readU16(p + 20);
        h.bank = readU16(p + 22);
        h.bagNdx = readU16(p + 24);
        sf.phdrs.push_back(std::move(h));
    }

    auto readBags = [](const ChunkSlice& s, std::vector<Bag>& out) {
        const size_t n = s.size / 4;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* p = s.data + i * 4;
            out.push_back({readU16(p), readU16(p + 2)});
        }
    };
    auto readGens = [](const ChunkSlice& s, std::vector<Gen>& out) {
        const size_t n = s.size / 4;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* p = s.data + i * 4;
            Gen g;
            g.oper = readU16(p);
            g.u16 = readU16(p + 2);
            g.i16 = readI16(p + 2);
            g.lo = p[2];
            g.hi = p[3];
            out.push_back(g);
        }
    };

    readBags(pbag, sf.pbags);
    readGens(pgen, sf.pgens);

    const size_t nInst = inst.size / 22;
    sf.insts.reserve(nInst);
    for (size_t i = 0; i < nInst; ++i) {
        const uint8_t* p = inst.data + i * 22;
        sf.insts.push_back({readFixedName(p, 20), readU16(p + 20)});
    }

    readBags(ibag, sf.ibags);
    readGens(igen, sf.igens);

    const size_t nShdr = shdr.size / 46;
    sf.shdrs.reserve(nShdr);
    for (size_t i = 0; i < nShdr; ++i) {
        const uint8_t* p = shdr.data + i * 46;
        Shdr s;
        s.name = readFixedName(p, 20);
        s.start = readU32(p + 20);
        s.end = readU32(p + 24);
        s.startLoop = readU32(p + 28);
        s.endLoop = readU32(p + 32);
        s.sampleRate = readU32(p + 36);
        s.originalKey = p[40];
        s.correction = (int8_t)p[41];
        s.sampleLink = readU16(p + 42);
        s.sampleType = readU16(p + 44);
        sf.shdrs.push_back(std::move(s));
    }

    if (sf.phdrs.size() < 2) {
        error = "no presets in SF2/SF3";
        return false;
    }
    return true;
}

bool parseSf2Riff(const std::string& path, Sf2File& sf, std::string& error, bool needSamples) {
    if (!readFileBytes(path, sf.raw, error)) return false;
    const uint8_t* d = sf.raw.data();
    const size_t n = sf.raw.size();
    if (std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "sfbk", 4) != 0) {
        error = "not a RIFF sfbk file: " + path;
        return false;
    }
    const uint32_t riffLen = readU32(d + 4);
    const size_t end = std::min<size_t>(n, (size_t)riffLen + 8);

    bool hasPdta = false;
    for (size_t pos = 12; pos + 8 <= end;) {
        const uint32_t csize = readU32(d + pos + 4);
        const size_t bodyPos = pos + 8;
        const size_t bodyLen = std::min<size_t>(csize, end - bodyPos);
        if (std::memcmp(d + pos, "LIST", 4) == 0 && bodyLen >= 4) {
            const uint8_t* ltype = d + bodyPos;
            const uint8_t* ldata = d + bodyPos + 4;
            const size_t lsize = bodyLen - 4;
            if (std::memcmp(ltype, "sdta", 4) == 0 && needSamples) {
                for (size_t sp = 0; sp + 8 <= lsize;) {
                    const uint32_t ssize = readU32(ldata + sp + 4);
                    const size_t sb = sp + 8;
                    const size_t sl = sb <= lsize ? std::min<size_t>(ssize, lsize - sb) : 0;
                    if (std::memcmp(ldata + sp, "smpl", 4) == 0) sf.smpl = {ldata + sb, sl};
                    else if (std::memcmp(ldata + sp, "sm24", 4) == 0) sf.sm24 = {ldata + sb, sl};
                    sp = sb + (size_t)ssize + (ssize & 1u);
                }
            } else if (std::memcmp(ltype, "pdta", 4) == 0) {
                if (!parsePdtaSubchunks(ldata, lsize, sf, error)) return false;
                hasPdta = true;
            }
        }
        pos = bodyPos + (size_t)csize + (csize & 1u);
    }
    if (!hasPdta) {
        error = "missing pdta chunk in SF2/SF3: " + path;
        return false;
    }
    return true;
}

// SF2 generator IDs
enum Sf2GenOp : uint16_t {
    GenStartAddrsOffset = 0,
    GenEndAddrsOffset = 1,
    GenStartloopAddrsOffset = 2,
    GenEndloopAddrsOffset = 3,
    GenStartAddrsCoarseOffset = 4,
    GenInitialFilterFc = 8,
    GenInitialFilterQ = 9,
    GenEndAddrsCoarseOffset = 12,
    GenPan = 17,
    GenDelayVolEnv = 33,
    GenAttackVolEnv = 34,
    GenHoldVolEnv = 35,
    GenDecayVolEnv = 36,
    GenSustainVolEnv = 37,
    GenReleaseVolEnv = 38,
    GenInstrument = 41,
    GenKeyRange = 43,
    GenVelRange = 44,
    GenStartloopAddrsCoarseOffset = 45,
    GenInitialAttenuation = 48,
    GenEndloopAddrsCoarseOffset = 50,
    GenCoarseTune = 51,
    GenFineTune = 52,
    GenSampleID = 53,
    GenSampleModes = 54,
    GenScaleTuning = 56,
    GenExclusiveClass = 57,
    GenOverridingRootKey = 58,
};

struct GenValues {
    int keyLo = 0;
    int keyHi = 127;
    int velLo = 0;
    int velHi = 127;
    int overridingRootKey = -1;
    int coarseTune = 0;
    int fineTune = 0;
    int scaleTuning = 100;
    int initialAttenuation = 0;  // centibels
    int pan = 0;                 // -500..500
    bool panExplicit = false;
    int sampleModes = 0;
    int32_t startOffset = 0;
    int32_t endOffset = 0;
    int32_t startloopOffset = 0;
    int32_t endloopOffset = 0;
    int delayVolEnv = -12000;    // timecents
    int attackVolEnv = -12000;
    int holdVolEnv = -12000;
    int decayVolEnv = -12000;
    int sustainVolEnv = 0;       // centibels attenuation
    int releaseVolEnv = -12000;
    int initialFilterFc = 13500; // cents
    int initialFilterQ = 0;      // centibels
    int exclusiveClass = 0;
    int instId = -1;
    int sampleId = -1;
};

GenValues defaultInstGens() { return GenValues{}; }

GenValues defaultPresetDelta() {
    GenValues g;
    g.scaleTuning = 0;
    g.initialFilterFc = 0;
    g.delayVolEnv = 0;
    g.attackVolEnv = 0;
    g.holdVolEnv = 0;
    g.decayVolEnv = 0;
    g.sustainVolEnv = 0;
    g.releaseVolEnv = 0;
    return g;
}

void applyGen(GenValues& v, const Gen& g, bool isPreset) {
    switch (g.oper) {
        case GenStartAddrsOffset: v.startOffset += g.i16; break;
        case GenEndAddrsOffset: v.endOffset += g.i16; break;
        case GenStartloopAddrsOffset: v.startloopOffset += g.i16; break;
        case GenEndloopAddrsOffset: v.endloopOffset += g.i16; break;
        case GenStartAddrsCoarseOffset: v.startOffset += (int32_t)g.i16 * 32768; break;
        case GenEndAddrsCoarseOffset: v.endOffset += (int32_t)g.i16 * 32768; break;
        case GenStartloopAddrsCoarseOffset: v.startloopOffset += (int32_t)g.i16 * 32768; break;
        case GenEndloopAddrsCoarseOffset: v.endloopOffset += (int32_t)g.i16 * 32768; break;
        case GenInitialFilterFc: v.initialFilterFc = g.i16; break;
        case GenInitialFilterQ: v.initialFilterQ = g.i16; break;
        case GenPan: v.pan = g.i16; v.panExplicit = true; break;
        case GenDelayVolEnv: v.delayVolEnv = g.i16; break;
        case GenAttackVolEnv: v.attackVolEnv = g.i16; break;
        case GenHoldVolEnv: v.holdVolEnv = g.i16; break;
        case GenDecayVolEnv: v.decayVolEnv = g.i16; break;
        case GenSustainVolEnv: v.sustainVolEnv = g.i16; break;
        case GenReleaseVolEnv: v.releaseVolEnv = g.i16; break;
        case GenInstrument: v.instId = g.u16; break;
        case GenKeyRange: v.keyLo = g.lo; v.keyHi = g.hi; break;
        case GenVelRange: v.velLo = g.lo; v.velHi = g.hi; break;
        case GenInitialAttenuation: v.initialAttenuation = g.i16; break;
        case GenCoarseTune: v.coarseTune = g.i16; break;
        case GenFineTune: v.fineTune = g.i16; break;
        case GenSampleID: v.sampleId = g.u16; break;
        case GenSampleModes:
            if (!isPreset) v.sampleModes = g.u16;
            break;
        case GenScaleTuning: v.scaleTuning = g.i16; break;
        case GenExclusiveClass:
            if (!isPreset) v.exclusiveClass = g.u16;
            break;
        case GenOverridingRootKey:
            if (!isPreset) v.overridingRootKey = g.i16;
            break;
        default: break;
    }
}

GenValues combinePresetAndInst(const GenValues& preset, const GenValues& inst) {
    GenValues c = inst;
    c.keyLo = std::max(inst.keyLo, preset.keyLo);
    c.keyHi = std::min(inst.keyHi, preset.keyHi);
    c.velLo = std::max(inst.velLo, preset.velLo);
    c.velHi = std::min(inst.velHi, preset.velHi);
    c.coarseTune += preset.coarseTune;
    c.fineTune += preset.fineTune;
    c.scaleTuning = std::max(0, inst.scaleTuning + preset.scaleTuning);
    c.initialAttenuation += preset.initialAttenuation;
    c.pan += preset.pan;
    c.panExplicit = inst.panExplicit || preset.panExplicit;
    c.startOffset += preset.startOffset;
    c.endOffset += preset.endOffset;
    c.startloopOffset += preset.startloopOffset;
    c.endloopOffset += preset.endloopOffset;
    c.delayVolEnv += preset.delayVolEnv;
    c.attackVolEnv += preset.attackVolEnv;
    c.holdVolEnv += preset.holdVolEnv;
    c.decayVolEnv += preset.decayVolEnv;
    c.sustainVolEnv += preset.sustainVolEnv;
    c.releaseVolEnv += preset.releaseVolEnv;
    c.initialFilterFc += preset.initialFilterFc;
    c.initialFilterQ += preset.initialFilterQ;
    return c;
}

bool zoneHasOp(const std::vector<Gen>& gens, uint16_t begin, uint16_t end, uint16_t op) {
    const size_t limit = std::min<size_t>(end, gens.size());
    for (size_t i = begin; i < limit; ++i)
        if (gens[i].oper == op) return true;
    return false;
}

struct DecodedMono {
    bool ok = false;
    bool isSf3 = false;
    double sampleRate = 44100.0;
    std::vector<float> pcm;  // raw decoded samples (without kPadFrames yet)
    int64_t baseStart = 0;   // subtracted from loop points for SF2
};

DecodedMono decodeShdrMono(const Sf2File& sf, size_t shdrIdx, const OggDecodeFn& oggDecode,
                           std::vector<std::string>& warnings) {
    DecodedMono res;
    if (shdrIdx >= sf.shdrs.size()) return res;
    const Shdr& sh = sf.shdrs[shdrIdx];
    const bool isSf3 = (sh.sampleType & 0x10u) != 0;
    const bool isRom = (sh.sampleType & 0x8000u) != 0;
    if (isRom) {
        warnings.push_back("SF2 ROM sample '" + sh.name + "' skipped (unsupported)");
        return res;
    }
    res.isSf3 = isSf3;

    if (isSf3) {
        if (!oggDecode) {
            warnings.push_back("SF3 Ogg Vorbis sample '" + sh.name + "' skipped (no Ogg decoder provided)");
            return res;
        }
        if (sh.end <= sh.start || sh.end > sf.smpl.size || !sf.smpl.data) {
            warnings.push_back("SF3 sample '" + sh.name + "' has invalid smpl byte bounds");
            return res;
        }
        const uint8_t* oggPtr = sf.smpl.data + sh.start;
        const size_t oggLen = (size_t)(sh.end - sh.start);
        int outRate = (int)sh.sampleRate;
        if (!oggDecode(oggPtr, oggLen, res.pcm, outRate) || res.pcm.empty()) {
            warnings.push_back("SF3 Ogg Vorbis decode failed for sample '" + sh.name + "'");
            return res;
        }
        res.sampleRate = outRate > 0 ? (double)outRate : (sh.sampleRate > 0 ? (double)sh.sampleRate : 44100.0);
        res.baseStart = 0;  // SF3 loop points are relative to decoded sample start
        res.ok = true;
        return res;
    }

    // Standard SF2 16-bit (or 24-bit with sm24) PCM
    if (!sf.smpl.data || sh.end <= sh.start) return res;
    const size_t totalSmplFrames = sf.smpl.size / 2;
    if (sh.end > totalSmplFrames) {
        warnings.push_back("SF2 sample '" + sh.name + "' exceeds smpl bounds");
        return res;
    }
    const size_t count = (size_t)(sh.end - sh.start);
    const bool has24 = sf.sm24.data && sf.sm24.size >= totalSmplFrames;
    res.pcm.resize(count);
    const uint8_t* s16 = sf.smpl.data + (size_t)sh.start * 2;
    if (has24) {
        const uint8_t* s24 = sf.sm24.data + sh.start;
        constexpr float kScale24 = 1.0f / 8388608.0f;
        for (size_t i = 0; i < count; ++i) {
            const int32_t hi = (int32_t)readI16(s16 + i * 2);
            const int32_t v = (hi << 8) | (int32_t)s24[i];
            res.pcm[i] = (float)v * kScale24;
        }
    } else {
        constexpr float kScale16 = 1.0f / 32768.0f;
        for (size_t i = 0; i < count; ++i) {
            res.pcm[i] = (float)readI16(s16 + i * 2) * kScale16;
        }
    }
    res.sampleRate = sh.sampleRate > 0 ? (double)sh.sampleRate : 44100.0;
    res.baseStart = (int64_t)sh.start;
    res.ok = true;
    return res;
}

float timecentsToSec(int tc) {
    if (tc <= -12000) return 0.0f;
    return std::exp2((float)tc / 1200.0f);
}

struct PendingZone {
    GenValues g;
    int shdrIdx = -1;
    bool pairedRightConsumed = false;
};

}  // namespace

bool listSf2Presets(const std::string& path, std::vector<Sf2PresetInfo>& out, std::string& error) {
    out.clear();
    Sf2File sf;
    if (!parseSf2Riff(path, sf, error, false)) return false;
    const size_t count = sf.phdrs.size() - 1;  // exclude terminal EOP
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Sf2PresetInfo info;
        info.bank = sf.phdrs[i].bank;
        info.program = sf.phdrs[i].preset;
        info.name = sf.phdrs[i].name;
        info.presetIndex = (int)i;
        out.push_back(std::move(info));
    }
    return true;
}

bool loadSf2Preset(const std::string& path, int bank, int program, SampleSet& out, std::string& error,
                   std::vector<std::string>& warnings, OggDecodeFn oggDecode) {
    out = SampleSet{};
    out.sourcePath = path;
    warnings.clear();

    Sf2File sf;
    if (!parseSf2Riff(path, sf, error, true)) return false;

    const size_t numPresets = sf.phdrs.size() - 1;
    size_t presetIdx = numPresets;
    if (bank < 0 || program < 0) {
        if (numPresets > 0) presetIdx = 0;
    } else {
        for (size_t i = 0; i < numPresets; ++i) {
            if (sf.phdrs[i].bank == (uint16_t)bank && sf.phdrs[i].preset == (uint16_t)program) {
                presetIdx = i;
                break;
            }
        }
    }
    if (presetIdx >= numPresets) {
        error = "preset not found (bank=" + std::to_string(bank) + ", program=" + std::to_string(program) + ") in " + path;
        return false;
    }

    out.name = sf.phdrs[presetIdx].name;

    const uint16_t pBagStart = sf.phdrs[presetIdx].bagNdx;
    const uint16_t pBagEnd = sf.phdrs[presetIdx + 1].bagNdx;
    if (pBagEnd > sf.pbags.size() || pBagStart >= pBagEnd) {
        error = "preset '" + out.name + "' has no zones";
        return false;
    }

    // Check for global preset zone (first bag lacks GenInstrument).
    GenValues pGlobal = defaultPresetDelta();
    uint16_t pFirstLocal = pBagStart;
    {
        const uint16_t g0 = sf.pbags[pBagStart].genNdx;
        const uint16_t g1 = ((size_t)pBagStart + 1 < sf.pbags.size()) ? sf.pbags[pBagStart + 1].genNdx : (uint16_t)sf.pgens.size();
        if (!zoneHasOp(sf.pgens, g0, g1, GenInstrument)) {
            const size_t lim = std::min<size_t>(g1, sf.pgens.size());
            for (size_t gi = g0; gi < lim; ++gi) applyGen(pGlobal, sf.pgens[gi], true);
            pFirstLocal = pBagStart + 1;
        }
    }

    std::vector<PendingZone> zones;
    for (uint16_t pb = pFirstLocal; pb < pBagEnd && (size_t)pb + 1 < sf.pbags.size(); ++pb) {
        const uint16_t pg0 = sf.pbags[pb].genNdx;
        const uint16_t pg1 = sf.pbags[pb + 1].genNdx;
        if (!zoneHasOp(sf.pgens, pg0, pg1, GenInstrument)) continue;
        GenValues pLocal = pGlobal;
        const size_t plim = std::min<size_t>(pg1, sf.pgens.size());
        for (size_t gi = pg0; gi < plim; ++gi) {
            applyGen(pLocal, sf.pgens[gi], true);
            if (sf.pgens[gi].oper == GenInstrument) break;
        }
        if (pLocal.instId < 0 || (size_t)(pLocal.instId + 1) >= sf.insts.size()) continue;

        const Inst& inst = sf.insts[(size_t)pLocal.instId];
        const Inst& instNext = sf.insts[(size_t)pLocal.instId + 1];
        const uint16_t iBagStart = inst.bagNdx;
        const uint16_t iBagEnd = instNext.bagNdx;
        if (iBagEnd > sf.ibags.size() || iBagStart >= iBagEnd) continue;

        GenValues iGlobal = defaultInstGens();
        uint16_t iFirstLocal = iBagStart;
        {
            const uint16_t ig0 = sf.ibags[iBagStart].genNdx;
            const uint16_t ig1 = ((size_t)iBagStart + 1 < sf.ibags.size()) ? sf.ibags[iBagStart + 1].genNdx : (uint16_t)sf.igens.size();
            if (!zoneHasOp(sf.igens, ig0, ig1, GenSampleID)) {
                const size_t ilim = std::min<size_t>(ig1, sf.igens.size());
                for (size_t gi = ig0; gi < ilim; ++gi) applyGen(iGlobal, sf.igens[gi], false);
                iFirstLocal = iBagStart + 1;
            }
        }

        for (uint16_t ib = iFirstLocal; ib < iBagEnd && (size_t)ib + 1 < sf.ibags.size(); ++ib) {
            const uint16_t ig0 = sf.ibags[ib].genNdx;
            const uint16_t ig1 = sf.ibags[ib + 1].genNdx;
            if (!zoneHasOp(sf.igens, ig0, ig1, GenSampleID)) continue;
            GenValues iLocal = iGlobal;
            const size_t ilim = std::min<size_t>(ig1, sf.igens.size());
            for (size_t gi = ig0; gi < ilim; ++gi) {
                applyGen(iLocal, sf.igens[gi], false);
                if (sf.igens[gi].oper == GenSampleID) break;
            }
            if (iLocal.sampleId < 0 || (size_t)iLocal.sampleId + 1 >= sf.shdrs.size()) continue;

            GenValues combined = combinePresetAndInst(pLocal, iLocal);
            if (combined.keyLo > combined.keyHi || combined.velLo > combined.velHi) continue;
            zones.push_back({combined, iLocal.sampleId, false});
        }
    }

    // Cache decoded mono PCM by shdr index.
    std::unordered_map<int, DecodedMono> monoCache;
    auto getMono = [&](int shdrIdx) -> const DecodedMono* {
        auto it = monoCache.find(shdrIdx);
        if (it != monoCache.end()) return it->second.ok ? &it->second : nullptr;
        DecodedMono dm = decodeShdrMono(sf, (size_t)shdrIdx, oggDecode, warnings);
        auto [ins, _] = monoCache.emplace(shdrIdx, std::move(dm));
        return ins->second.ok ? &ins->second : nullptr;
    };

    // Cache Sample objects by stereo key (leftIdx, rightIdx) or mono key (idx, -1).
    std::map<std::pair<int, int>, Sample*> sampleCache;
    auto getOrCreateSample = [&](int leftIdx, int rightIdx) -> Sample* {
        const std::pair<int, int> key{leftIdx, rightIdx};
        auto it = sampleCache.find(key);
        if (it != sampleCache.end()) return it->second;

        const DecodedMono* dmL = getMono(leftIdx);
        if (!dmL) return nullptr;

        auto s = std::make_unique<Sample>();
        const Shdr& shL = sf.shdrs[(size_t)leftIdx];
        s->path = path + "#" + shL.name;
        s->sampleRate = dmL->sampleRate;
        s->unityNote = shL.originalKey <= 127 ? (int)shL.originalKey : 60;

        if (rightIdx >= 0) {
            const DecodedMono* dmR = getMono(rightIdx);
            if (!dmR || dmR->pcm.size() != dmL->pcm.size()) {
                return nullptr;
            }
            s->channels = 2;
            s->frames = (int64_t)dmL->pcm.size();
            s->data[0] = dmL->pcm;
            s->data[1] = dmR->pcm;
            s->data[0].resize((size_t)(s->frames + Sample::kPadFrames), 0.0f);
            s->data[1].resize((size_t)(s->frames + Sample::kPadFrames), 0.0f);
        } else {
            s->channels = 1;
            s->frames = (int64_t)dmL->pcm.size();
            s->data[0] = dmL->pcm;
            s->data[0].resize((size_t)(s->frames + Sample::kPadFrames), 0.0f);
        }

        const int64_t baseL = dmL->baseStart;
        const int64_t ls = (int64_t)shL.startLoop - baseL;
        const int64_t le = (int64_t)shL.endLoop - baseL;
        if (ls >= 0 && le > ls + 1 && le <= s->frames) {
            s->loopStart = ls;
            s->loopEnd = le;
        }
        Sample* ptr = s.get();
        out.samples.push_back(std::move(s));
        sampleCache[key] = ptr;
        return ptr;
    };

    // Match leftSample (bit 2: 0x04) + rightSample (bit 1: 0x02) pairs into stereo Samples.
    for (size_t i = 0; i < zones.size(); ++i) {
        if (zones[i].pairedRightConsumed) continue;
        const int idxL = zones[i].shdrIdx;
        const Shdr& shL = sf.shdrs[(size_t)idxL];
        const uint16_t baseTypeL = (uint16_t)(shL.sampleType & 0x07u);

        int pairedZoneIdx = -1;
        if (baseTypeL == 4 && shL.sampleLink < sf.shdrs.size() - 1) {
            const int idxR = (int)shL.sampleLink;
            const Shdr& shR = sf.shdrs[(size_t)idxR];
            if ((shR.sampleType & 0x07u) == 2 && shR.sampleLink == (uint16_t)idxL) {
                for (size_t j = 0; j < zones.size(); ++j) {
                    if (j == i || zones[j].pairedRightConsumed) continue;
                    if (zones[j].shdrIdx != idxR) continue;
                    const GenValues& gL = zones[i].g;
                    const GenValues& gR = zones[j].g;
                    if (gL.keyLo == gR.keyLo && gL.keyHi == gR.keyHi &&
                        gL.velLo == gR.velLo && gL.velHi == gR.velHi &&
                        gL.coarseTune == gR.coarseTune && gL.fineTune == gR.fineTune) {
                        pairedZoneIdx = (int)j;
                        break;
                    }
                }
            }
        }

        Sample* sample = nullptr;
        float pan = 0.0f;
        if (pairedZoneIdx >= 0) {
            sample = getOrCreateSample(idxL, zones[(size_t)pairedZoneIdx].shdrIdx);
            if (sample) {
                zones[(size_t)pairedZoneIdx].pairedRightConsumed = true;
                const float pL = clampf((float)zones[i].g.pan / 500.0f, -1.0f, 1.0f);
                const float pR = clampf((float)zones[(size_t)pairedZoneIdx].g.pan / 500.0f, -1.0f, 1.0f);
                pan = clampf(0.5f * (pL + pR), -1.0f, 1.0f);
            }
        }
        if (!sample) {
            sample = getOrCreateSample(idxL, -1);
            if (!sample) continue;
            if (zones[i].g.panExplicit) {
                pan = clampf((float)zones[i].g.pan / 500.0f, -1.0f, 1.0f);
            } else if (baseTypeL == 4) {
                pan = -1.0f;
            } else if (baseTypeL == 2) {
                pan = 1.0f;
            }
        }

        const GenValues& g = zones[i].g;
        const Shdr& sh = sf.shdrs[(size_t)idxL];
        const DecodedMono* dm = getMono(idxL);
        const int64_t baseStart = dm ? dm->baseStart : (int64_t)sh.start;

        Region r;
        r.sample = sample;
        r.lokey = std::max(0, std::min(127, g.keyLo));
        r.hikey = std::max(0, std::min(127, g.keyHi));
        r.lovel = std::max(1, std::min(127, g.velLo));
        r.hivel = std::max(1, std::min(127, g.velHi));
        if (r.lovel > r.hivel) r.lovel = r.hivel;

        if (g.overridingRootKey >= 0 && g.overridingRootKey <= 127) r.keycenter = g.overridingRootKey;
        else if (sh.originalKey <= 127) r.keycenter = (int)sh.originalKey;
        else r.keycenter = 60;

        r.pitchKeytrack = (float)std::max(0, g.scaleTuning);
        r.transpose = g.coarseTune;
        r.tune = (float)g.fineTune + (float)sh.correction;
        r.volumeDb = -(float)std::max(0, g.initialAttenuation) * 0.1f;
        r.pan = pan;

        // Sample start / end offsets
        const int64_t off = std::max<int64_t>(0, (int64_t)g.startOffset);
        const int64_t end = sample->frames + (int64_t)g.endOffset;
        r.offset = std::min(off, std::max<int64_t>(0, sample->frames - 1));
        r.end = (end > r.offset && end < sample->frames) ? end : -1;

        // Loop mode & offsets
        if (g.sampleModes == 1) r.loopMode = LoopMode::Continuous;
        else if (g.sampleModes == 3) r.loopMode = LoopMode::Sustain;
        else r.loopMode = LoopMode::NoLoop;

        if (r.loopMode == LoopMode::Continuous || r.loopMode == LoopMode::Sustain) {
            const int64_t ls = (int64_t)sh.startLoop - baseStart + (int64_t)g.startloopOffset;
            const int64_t le = (int64_t)sh.endLoop - baseStart + (int64_t)g.endloopOffset;
            if (ls >= 0 && le > ls + 1 && le <= sample->frames) {
                r.loopStart = ls;
                r.loopEnd = le;
            } else {
                r.loopMode = LoopMode::NoLoop;
            }
        }

        // Volume envelope (SF2 timecents & centibel sustain attenuation)
        r.ampeg.enabled = true;
        r.ampeg.delay = timecentsToSec(g.delayVolEnv);
        r.ampeg.attack = timecentsToSec(g.attackVolEnv);
        r.ampeg.hold = timecentsToSec(g.holdVolEnv);
        r.ampeg.decay = timecentsToSec(g.decayVolEnv);
        r.ampeg.sustain = g.sustainVolEnv <= 0
                              ? 1.0f
                              : (g.sustainVolEnv >= 1000 ? 0.0f : clampf(dbToGain(-(float)g.sustainVolEnv * 0.1f), 0.0f, 1.0f));
        r.ampeg.release = timecentsToSec(g.releaseVolEnv);

        // Low-pass filter (when below wide-open 13500 cents)
        if (g.initialFilterFc < 13450) {
            r.filter = RegionFilter::Lp2;
            r.cutoff = clampf(8.176f * std::exp2((float)g.initialFilterFc / 1200.0f), 20.0f, 20000.0f);
            r.resonance = std::max(0.0f, (float)g.initialFilterQ * 0.1f);
        }

        // Exclusive class
        if (g.exclusiveClass > 0) {
            r.group = g.exclusiveClass;
            r.offBy = g.exclusiveClass;
            r.offFast = true;
        }

        out.regions.push_back(r);
    }

    out.warnings = warnings;
    if (out.regions.empty()) {
        error = "no playable regions in preset '" + out.name + "' of " + path;
        return false;
    }

    out.prepareMips();
    return true;
}

}  // namespace hsynth
