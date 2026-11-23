// Phase 5 (v0.8): NPU Morph, 4-Stem Backing Sync/Mix/Solo/Mute, STFT & Fast Spectral
// Separation, and New Format Parsers (SF2, DLS2, GIG v2, Decent Sampler, DX7 SysEx,
// Surge .wt / Vital .vitaltable).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "analysis.h"
#include "hsynth/backing_player.h"
#include "hsynth/dsp/wavetable.h"
#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/formats/dls_gig.h"
#include "hsynth/formats/dspreset.h"
#include "hsynth/formats/dx7_sysex.h"
#include "hsynth/formats/sf2.h"
#include "hsynth/formats/wavetable_ext.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/npu_morph.h"
#include "hsynth/params.h"
#include "hsynth/preset.h"
#include "hsynth/remote.h"
#include "hsynth/stft.h"
#include "hsynth/wav.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;
namespace fs = std::filesystem;

namespace {

constexpr float kSr = 48000.0f;
constexpr int kSrInt = 48000;

std::string tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / "hsynth_tests" / "v08_p5" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p.generic_string();
}

std::string writeStereoSineWav(const std::string& path, double sec, double freqL, double freqR,
                               float amp = 0.5f, int sr = kSrInt) {
    const size_t n = (size_t)(sec * sr);
    std::vector<float> l(n, 0.0f), r(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / sr;
        l[i] = amp * float(std::sin(2.0 * kPi * freqL * t));
        r[i] = amp * float(std::sin(2.0 * kPi * freqR * t));
    }
    std::string err;
    CHECK(writeWav(path, l, &r, sr, false, err));
    return path;
}

void renderAppend(Engine& e, int frames, std::vector<float>& outL, std::vector<float>& outR, int block = 128) {
    std::vector<float> bl((size_t)block, 0.0f), br((size_t)block, 0.0f);
    for (int done = 0; done < frames; done += block) {
        const int n = std::min(block, frames - done);
        e.process(bl.data(), br.data(), n);
        outL.insert(outL.end(), bl.begin(), bl.begin() + n);
        outR.insert(outR.end(), br.begin(), br.begin() + n);
        e.collectGarbage();
    }
}

void renderBacking(Engine& e, int frames, std::vector<float>& outL, std::vector<float>& outR) {
    const int block = 256;
    std::array<float, block> bl{}, br{};
    const auto start = std::chrono::steady_clock::now();
    for (int done = 0; done < frames;) {
        const int n = std::min(block, frames - done);
        e.process(bl.data(), br.data(), n);
        outL.insert(outL.end(), bl.begin(), bl.begin() + n);
        outR.insert(outR.end(), br.begin(), br.begin() + n);
        done += n;
        if (done < frames && done % 1024 == 0) {
            const auto elapsed = std::chrono::microseconds(int64_t(done) * 1'000'000 / kSrInt);
            std::this_thread::sleep_until(start + elapsed);
        }
    }
}

void waitBackingReady() {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
}

Json parseJson(const std::string& s) {
    Json j;
    std::string err;
    CHECK(Json::parse(s, j, err));
    return j;
}

float normParam(int id, float value) {
    const auto& p = paramTable()[(size_t)id];
    value = sanitizeParam(id, value);
    if (p.curve == ParamCurve::Exp && p.min > 0.0f)
        return float(std::log(double(value) / p.min) / std::log(double(p.max) / p.min));
    return (value - p.min) / (p.max - p.min);
}

// Binary writers for RIFF/SF2/DLS2/GIG2/WT/SYX fixtures
void pushU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void pushU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
}
void pushI16(std::vector<uint8_t>& b, int16_t v) { pushU16(b, (uint16_t)v); }
void pushU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
    b.push_back((uint8_t)((v >> 16) & 0xFF));
    b.push_back((uint8_t)((v >> 24) & 0xFF));
}
void pushI32(std::vector<uint8_t>& b, int32_t v) { pushU32(b, (uint32_t)v); }
void pushTag(std::vector<uint8_t>& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }
void pushFixedStr(std::vector<uint8_t>& b, const std::string& s, size_t width) {
    for (size_t i = 0; i < width; ++i) b.push_back(i < s.size() ? (uint8_t)s[i] : 0);
}
std::vector<uint8_t> makeChunk(const char* id, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.reserve(8 + body.size() + 1);
    pushTag(out, id);
    pushU32(out, (uint32_t)body.size());
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() & 1u) out.push_back(0);
    return out;
}
std::vector<uint8_t> makeList(const char* outer, const char* listType, const std::vector<std::vector<uint8_t>>& children) {
    std::vector<uint8_t> body;
    pushTag(body, listType);
    for (const auto& c : children) body.insert(body.end(), c.begin(), c.end());
    return makeChunk(outer, body);
}
bool writeBytesFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "wb"), &std::fclose);
    return f && std::fwrite(bytes.data(), 1, bytes.size(), f.get()) == bytes.size();
}
bool writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream ofs(utf8Path(path), std::ios::binary);
    if (!ofs) return false;
    ofs.write(text.data(), (std::streamsize)text.size());
    return ofs.good();
}

std::string encodeBase64(const uint8_t* data, size_t len) {
    static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kTbl[(triple >> 18) & 0x3F]);
        out.push_back(kTbl[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kTbl[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kTbl[triple & 0x3F] : '=');
    }
    return out;
}

std::vector<uint8_t> makeSine16Pcm(int frames, double freq, int sr = 48000, float amp = 0.6f) {
    std::vector<uint8_t> pcm;
    pcm.reserve((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        const double v = amp * std::sin(2.0 * kPi * freq * double(i) / sr);
        pushI16(pcm, (int16_t)std::lround(v * 32767.0));
    }
    return pcm;
}

std::vector<uint8_t> buildSyntheticSf2(int frames = 4800, int sr = 48000) {
    // INFO
    std::vector<uint8_t> ifil;
    pushU16(ifil, 2);
    pushU16(ifil, 1);
    std::vector<uint8_t> inam{'T','e','s','t','S','F','2',0};
    auto infoList = makeList("LIST", "INFO", {makeChunk("ifil", ifil), makeChunk("INAM", inam)});

    // sdta -> smpl
    std::vector<uint8_t> smpl = makeSine16Pcm(frames, 440.0, sr, 0.5f);
    smpl.resize((size_t)(frames + 46) * 2, 0);
    auto sdtaList = makeList("LIST", "sdta", {makeChunk("smpl", smpl)});

    // pdta
    auto addPhdr = [](std::vector<uint8_t>& b, const std::string& name, uint16_t preset, uint16_t bank, uint16_t bag) {
        pushFixedStr(b, name, 20);
        pushU16(b, preset);
        pushU16(b, bank);
        pushU16(b, bag);
        pushU32(b, 0);
        pushU32(b, 0);
        pushU32(b, 0);
    };
    std::vector<uint8_t> phdr;
    addPhdr(phdr, "TestPiano", 0, 0, 0);
    addPhdr(phdr, "EOP", 255, 255, 1);

    std::vector<uint8_t> pbag;
    pushU16(pbag, 0); pushU16(pbag, 0);
    pushU16(pbag, 2); pushU16(pbag, 0);

    std::vector<uint8_t> pgen;
    pushU16(pgen, 43); pushU8(pgen, 36); pushU8(pgen, 84);  // KeyRange 36..84
    pushU16(pgen, 41); pushU16(pgen, 0);                     // Instrument 0
    pushU16(pgen, 0);  pushU16(pgen, 0);                     // Terminal

    std::vector<uint8_t> inst;
    pushFixedStr(inst, "TestInst", 20); pushU16(inst, 0);
    pushFixedStr(inst, "EOI", 20);      pushU16(inst, 1);

    std::vector<uint8_t> ibag;
    pushU16(ibag, 0); pushU16(ibag, 0);
    pushU16(ibag, 4); pushU16(ibag, 0);

    std::vector<uint8_t> igen;
    pushU16(igen, 43); pushU8(igen, 36); pushU8(igen, 84);   // KeyRange 36..84
    pushU16(igen, 58); pushU16(igen, 69);                    // OverridingRootKey 69
    pushU16(igen, 54); pushU16(igen, 1);                     // SampleModes = 1 (Continuous loop)
    pushU16(igen, 53); pushU16(igen, 0);                     // SampleID = 0
    pushU16(igen, 0);  pushU16(igen, 0);                     // Terminal

    std::vector<uint8_t> shdr;
    pushFixedStr(shdr, "Sine440", 20);
    pushU32(shdr, 0);
    pushU32(shdr, (uint32_t)frames);
    pushU32(shdr, 100);
    pushU32(shdr, (uint32_t)(frames - 100));
    pushU32(shdr, (uint32_t)sr);
    pushU8(shdr, 69);
    pushU8(shdr, 0);
    pushU16(shdr, 0);
    pushU16(shdr, 1);  // monoSample
    // EOS record (46 bytes)
    pushFixedStr(shdr, "EOS", 20);
    for (int i = 0; i < 26; ++i) pushU8(shdr, 0);

    auto pdtaList = makeList("LIST", "pdta", {
        makeChunk("phdr", phdr), makeChunk("pbag", pbag), makeChunk("pgen", pgen),
        makeChunk("inst", inst), makeChunk("ibag", ibag), makeChunk("igen", igen),
        makeChunk("shdr", shdr)
    });

    return makeList("RIFF", "sfbk", {infoList, sdtaList, pdtaList});
}

std::vector<uint8_t> makeWsmpChunk(uint16_t unity, uint32_t loopStart, uint32_t loopLen) {
    std::vector<uint8_t> wsmp;
    pushU32(wsmp, 20);
    pushU16(wsmp, unity);
    pushI16(wsmp, 0);
    pushI32(wsmp, 0);
    pushU32(wsmp, 0);
    pushU32(wsmp, 1);  // 1 loop
    pushU32(wsmp, 16);
    pushU32(wsmp, 0);  // Continuous
    pushU32(wsmp, loopStart);
    pushU32(wsmp, loopLen);
    return makeChunk("wsmp", wsmp);
}

std::vector<uint8_t> makeWaveList(int frames, double freq, int sr) {
    std::vector<uint8_t> fmt;
    pushU16(fmt, 1);               // PCM
    pushU16(fmt, 1);               // mono
    pushU32(fmt, (uint32_t)sr);
    pushU32(fmt, (uint32_t)(sr * 2));
    pushU16(fmt, 2);               // blockAlign
    pushU16(fmt, 16);              // bits
    auto data = makeSine16Pcm(frames, freq, sr, 0.5f);
    return makeList("LIST", "wave", {makeChunk("fmt ", fmt), makeChunk("data", data)});
}

std::vector<uint8_t> buildSyntheticDls2(int frames = 4800, int sr = 48000) {
    std::vector<uint8_t> vers;
    pushU16(vers, 0); pushU16(vers, 2); pushU32(vers, 0);  // major=2

    std::vector<uint8_t> ptbl;
    pushU32(ptbl, 8);  // header size
    pushU32(ptbl, 1);  // 1 cue
    pushU32(ptbl, 0);  // offset 0 into wvpl

    std::vector<uint8_t> insh;
    pushU32(insh, 1);  // cRegions
    pushU32(insh, 0);  // ulBank
    pushU32(insh, 0);  // ulInstrument

    std::vector<uint8_t> inam{'T','e','s','t','D','L','S','2',0};
    auto insInfo = makeList("LIST", "INFO", {makeChunk("INAM", inam)});

    std::vector<uint8_t> rgnh;
    pushU16(rgnh, 36); pushU16(rgnh, 84);  // key 36..84
    pushU16(rgnh, 1);  pushU16(rgnh, 127); // vel 1..127
    pushU16(rgnh, 0);  pushU16(rgnh, 0);

    std::vector<uint8_t> wlnk;
    pushU16(wlnk, 0); pushU16(wlnk, 0); pushU32(wlnk, 1); pushU32(wlnk, 0);

    // Level 2 articulator (lar2 -> art2) setting Attack = ~10ms
    std::vector<uint8_t> art2;
    pushU32(art2, 8);  // header
    pushU32(art2, 1);  // 1 connection
    pushU16(art2, 0); pushU16(art2, 0); pushU16(art2, 0x0206); pushU16(art2, 0);
    pushI32(art2, int32_t(-7973.0 * 65536.0));
    auto lar2 = makeList("LIST", "lar2", {makeChunk("art2", art2)});

    auto rgn = makeList("LIST", "rgn ", {
        makeChunk("rgnh", rgnh),
        makeWsmpChunk(69, 100, (uint32_t)(frames - 200)),
        makeChunk("wlnk", wlnk),
        lar2
    });
    auto lrgn = makeList("LIST", "lrgn", {rgn});
    auto ins = makeList("LIST", "ins ", {makeChunk("insh", insh), insInfo, lrgn});
    auto lins = makeList("LIST", "lins", {ins});
    auto wvpl = makeList("LIST", "wvpl", {makeWaveList(frames, 440.0, sr)});

    return makeList("RIFF", "DLS ", {makeChunk("vers", vers), makeChunk("ptbl", ptbl), lins, wvpl});
}

std::vector<uint8_t> buildSyntheticGig2(int frames = 4800, int sr = 48000) {
    std::vector<uint8_t> vers;
    pushU16(vers, 0); pushU16(vers, 2); pushU32(vers, 0);  // major=2

    std::vector<uint8_t> ptbl;
    pushU32(ptbl, 8);
    pushU32(ptbl, 1);
    pushU32(ptbl, 0);

    std::vector<uint8_t> insh;
    pushU32(insh, 1);
    pushU32(insh, 0);
    pushU32(insh, 1);  // program 1

    std::vector<uint8_t> inam{'T','e','s','t','G','I','G','2',0};
    auto insInfo = makeList("LIST", "INFO", {makeChunk("INAM", inam)});

    std::vector<uint8_t> ewg(12, 0);  // 3ewg (gain=0, tune=0)

    std::vector<uint8_t> rgnh;
    pushU16(rgnh, 48); pushU16(rgnh, 72);
    pushU16(rgnh, 0);  pushU16(rgnh, 127);
    pushU16(rgnh, 0);  pushU16(rgnh, 0);

    std::vector<uint8_t> wlnk;
    pushU16(wlnk, 0); pushU16(wlnk, 0); pushU32(wlnk, 1); pushU32(wlnk, 0);

    // 3lnk (GIG v2: 5 slots of 8 bytes + 2 cues)
    std::vector<uint8_t> lnk;
    pushU32(lnk, 2);  // count = 2 dimension regions
    // Slot 0: Velocity (0x82), 1 bit (2 zones)
    pushU8(lnk, 0x82); pushU8(lnk, 1); pushU16(lnk, 0); pushU32(lnk, 0);
    for (int s = 1; s < 5; ++s) { pushU32(lnk, 0); pushU32(lnk, 0); }
    pushU32(lnk, 0);  // cue 0
    pushU32(lnk, 0);  // cue 1

    auto makeEwa = [&](uint8_t velLimit) {
        std::vector<uint8_t> b(140, 0);
        // sustain at offset 38 = 1000 (1.0)
        b[38] = uint8_t(1000 & 0xFF);
        b[39] = uint8_t((1000 >> 8) & 0xFF);
        // b[109] = 0 (center pan)
        b[124] = velLimit;
        return makeChunk("3ewa", b);
    };

    auto ewl0 = makeList("LIST", "3ewl", {makeWsmpChunk(69, 100, (uint32_t)(frames - 200)), makeEwa(63)});
    auto ewl1 = makeList("LIST", "3ewl", {makeWsmpChunk(69, 100, (uint32_t)(frames - 200)), makeEwa(127)});
    auto prg = makeList("LIST", "3prg", {ewl0, ewl1});

    auto rgn = makeList("LIST", "rgn ", {
        makeChunk("rgnh", rgnh), makeChunk("wlnk", wlnk), makeChunk("3lnk", lnk), prg
    });
    auto lrgn = makeList("LIST", "lrgn", {rgn});
    auto ins = makeList("LIST", "ins ", {makeChunk("insh", insh), insInfo, makeChunk("3ewg", ewg), lrgn});
    auto lins = makeList("LIST", "lins", {ins});
    auto wvpl = makeList("LIST", "wvpl", {makeWaveList(frames, 440.0, sr)});

    return makeList("RIFF", "DLS ", {makeChunk("vers", vers), makeChunk("ptbl", ptbl), lins, wvpl});
}

// Build a single 128-byte VMEM voice (OP6..OP1 packed 17 bytes each + 26 global bytes)
std::vector<uint8_t> makeDx7VmemVoice(const std::string& name, int algo0to31, int carrierLevel = 99) {
    std::vector<uint8_t> v(128, 0);
    for (int k = 0; k < 6; ++k) {
        uint8_t* p = v.data() + k * 17;
        p[0] = 99; p[1] = 85; p[2] = 75; p[3] = 65;  // R1..R4
        p[4] = 99; p[5] = 90; p[6] = 85; p[7] = 0;   // L1..L4
        p[8] = 39;                                   // BP
        p[12] = uint8_t(7 << 3);                     // detune = 7 (center)
        // Make k==5 (DX7 OP1, always a carrier) and k==4 (OP2) audible
        p[14] = (k == 5) ? (uint8_t)carrierLevel : (k == 4 ? 75 : 0);
        p[15] = uint8_t(1 << 1);                     // ratio = 1
    }
    for (int i = 0; i < 4; ++i) {
        v[102 + i] = 99;
        v[106 + i] = 50;
    }
    v[110] = (uint8_t)(algo0to31 & 0x1F);
    v[111] = 3;   // feedback 3
    v[112] = 35;  // LFO speed
    v[116] = 1;   // keySync
    v[117] = 24;  // transpose = C3
    for (size_t i = 0; i < 10; ++i) v[118 + i] = (uint8_t)(i < name.size() ? name[i] : ' ');
    return v;
}

std::vector<uint8_t> buildDx7VmemSyx() {
    std::vector<uint8_t> out;
    out.reserve(4104);
    out.insert(out.end(), {0xF0, 0x43, 0x00, 0x09, 0x20, 0x00});
    uint32_t sum = 0;
    for (int i = 0; i < 32; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "DX7P%02d", i + 1);
        auto v = makeDx7VmemVoice(buf, i % 32, 99);
        for (uint8_t b : v) sum += b;
        out.insert(out.end(), v.begin(), v.end());
    }
    out.push_back((uint8_t)((0u - sum) & 0x7Fu));
    out.push_back(0xF7);
    return out;
}

std::vector<uint8_t> buildDx7VcedSyx(const std::string& name, int algo0to31) {
    std::vector<uint8_t> payload(155, 0);
    for (int k = 0; k < 6; ++k) {
        uint8_t* p = payload.data() + k * 21;
        p[0] = 99; p[1] = 85; p[2] = 75; p[3] = 60;
        p[4] = 99; p[5] = 90; p[6] = 80; p[7] = 0;
        p[8] = 39;
        p[16] = (k == 5) ? 99 : (k == 4 ? 70 : 0);
        p[18] = 1;   // coarse = 1
        p[20] = 7;   // detune = 7
    }
    for (int i = 0; i < 4; ++i) {
        payload[126 + i] = 99;
        payload[130 + i] = 50;
    }
    payload[134] = (uint8_t)(algo0to31 & 0x1F);
    payload[135] = 4;
    payload[136] = 1;
    payload[137] = 35;
    payload[144] = 24;
    for (size_t i = 0; i < 10; ++i) payload[145 + i] = (uint8_t)(i < name.size() ? name[i] : ' ');

    std::vector<uint8_t> out{0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B};
    uint32_t sum = 0;
    for (uint8_t b : payload) sum += b;
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back((uint8_t)((0u - sum) & 0x7Fu));
    out.push_back(0xF7);
    return out;
}

std::vector<uint8_t> buildSurgeWt(int frameSize, int numFrames, bool int16Fmt) {
    std::vector<uint8_t> out;
    pushTag(out, "vawt");
    pushU32(out, (uint32_t)frameSize);
    pushU16(out, (uint16_t)numFrames);
    pushU16(out, int16Fmt ? 0x0004u : 0x0000u);
    for (int f = 0; f < numFrames; ++f) {
        const float mixSaw = float(f) / float(std::max(1, numFrames - 1));
        for (int i = 0; i < frameSize; ++i) {
            const float ph = float(i) / float(frameSize);
            const float sine = std::sin(2.0f * kPi * ph);
            const float harm = 0.65f * sine + 0.35f * std::sin(4.0f * kPi * ph) + 0.2f * std::sin(6.0f * kPi * ph);
            const float v = (1.0f - mixSaw) * sine + mixSaw * harm;
            if (int16Fmt) {
                pushI16(out, (int16_t)std::lround(clampf(v, -1.0f, 1.0f) * 32767.0f));
            } else {
                uint8_t raw[4];
                std::memcpy(raw, &v, 4);
                out.insert(out.end(), raw, raw + 4);
            }
        }
    }
    return out;
}

}  // namespace

TEST(npu_morph_anchor_fit_eval_and_remote) {
    Engine e;
    e.prepare(kSr);
    auto& morph = e.npuMorph();

    const int part = 0;
    struct Corner {
        int slot;
        float x, y;
        const char* name;
        float cutoff, reso, drive, envAmt, vaPw, macro1;
    };
    const Corner corners[4] = {
        {0, 0.0f, 0.0f, "Warm",   300.0f,  0.10f, 0.05f,  0.20f, 0.15f, 0.10f},
        {1, 1.0f, 0.0f, "Bright", 4200.0f, 0.45f, 0.25f, -0.40f, 0.55f, 0.40f},
        {2, 0.0f, 1.0f, "Acid",   1100.0f, 0.85f, 0.70f,  0.75f, 0.80f, 0.75f},
        {3, 1.0f, 1.0f, "Lead",   9600.0f, 0.30f, 0.90f,  0.10f, 0.35f, 0.95f},
    };
    const int trackedIds[] = {
        P::FilterCutoff, P::FilterReso, P::FilterDrive,
        P::FilterEnvAmt, P::layer(0, P::VaPw), P::Macro1
    };

    for (const auto& c : corners) {
        e.setPartParam(part, P::FilterCutoff, c.cutoff);
        e.setPartParam(part, P::FilterReso, c.reso);
        e.setPartParam(part, P::FilterDrive, c.drive);
        e.setPartParam(part, P::FilterEnvAmt, c.envAmt);
        e.setPartParam(part, P::layer(0, P::VaPw), c.vaPw);
        e.setPartParam(part, P::Macro1, c.macro1);
        CHECK(morph.captureAnchor(e, part, c.slot, c.x, c.y, c.name));
    }

    const Json stBefore = morph.status();
    CHECK(stBefore.find("available")->boolean());
    CHECK(!stBefore.find("compiled")->boolean());
    CHECK((int)stBefore.find("anchors")->number() == 4);

    const Json compiled = morph.compile();
    CHECK(compiled.find("compiled")->boolean());
    CHECK((int)compiled.find("anchors")->number() == 4);
    CHECK((int)compiled.find("params")->number() >= 6);

    // Evaluate at all 4 corners -> verify parameter reconstruction accuracy
    float maxNormErr = 0.0f;
    float maxLinearErr = 0.0f;
    for (const auto& c : corners) {
        const float ms = morph.evaluate(e, part, c.x, c.y);
        CHECK(ms >= 0.0f && std::isfinite(ms));
        const float expected[6] = {c.cutoff, c.reso, c.drive, c.envAmt, c.vaPw, c.macro1};
        for (int k = 0; k < 6; ++k) {
            const float actual = e.getPartParam(part, trackedIds[k]);
            const float nErr = std::fabs(normParam(trackedIds[k], actual) - normParam(trackedIds[k], expected[k]));
            maxNormErr = std::max(maxNormErr, nErr);
            if (trackedIds[k] != P::FilterCutoff)
                maxLinearErr = std::max(maxLinearErr, std::fabs(actual - expected[k]));
        }
    }
    INFO("npu_morph corner maxNormErr=%.6f, maxLinearErr=%.6f\n", maxNormErr, maxLinearErr);
    CHECK(maxNormErr < 0.015f);
    CHECK(maxLinearErr < 0.015f);

    // Evaluate at center (0.5, 0.5) -> smooth finite interpolation
    const float msMid = morph.evaluate(e, part, 0.5f, 0.5f);
    CHECK(msMid >= 0.0f && std::isfinite(msMid));
    const float midCutoff = e.getPartParam(part, P::FilterCutoff);
    const float midReso = e.getPartParam(part, P::FilterReso);
    const float midMacro = e.getPartParam(part, P::Macro1);
    INFO("npu_morph center (0.5,0.5): cutoff=%.1f, reso=%.3f, macro1=%.3f\n", midCutoff, midReso, midMacro);
    CHECK(std::isfinite(midCutoff) && midCutoff > 300.0f && midCutoff < 9600.0f);
    CHECK(std::isfinite(midReso) && midReso >= 0.05f && midReso <= 0.90f);
    CHECK(std::isfinite(midMacro) && midMacro > 0.15f && midMacro < 0.90f);

    // RemoteControl "npu_status" and "npu_morph" ops
    RemoteControl rc(e, RemoteControl::Config{});
    Json r = rc.handle(parseJson(R"({"op":"npu_status"})"));
    CHECK(r.find("ok")->boolean());
    CHECK(r.find("compiled")->boolean());
    CHECK((int)r.find("anchors")->number() == 4);

    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"anchors"})"));
    CHECK(r.find("ok")->boolean());
    CHECK(r.find("anchors")->isArray() && r.find("anchors")->size() == 4);

    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"remove","slot":3})"));
    CHECK(r.find("ok")->boolean());
    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"status"})"));
    CHECK(r.find("ok")->boolean());
    CHECK(!r.find("compiled")->boolean());
    CHECK((int)r.find("anchors")->number() == 3);

    // Re-capture slot 3 via remote, compile, and eval
    CHECK(rc.handle(parseJson(R"({"op":"set","values":{"filter.cutoff":7200,"macro.1":0.88}})")).find("ok")->boolean());
    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"capture","part":0,"slot":3,"x":1.0,"y":1.0,"name":"Lead2"})"));
    CHECK(r.find("ok")->boolean());
    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"compile"})"));
    CHECK(r.find("ok")->boolean());
    CHECK(r.find("compiled")->boolean());
    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"eval","part":0,"x":0.25,"y":0.75})"));
    CHECK(r.find("ok")->boolean());
    CHECK(r.find("ms")->number() >= 0.0);

    // Clear and verify graceful errors when uncompiled / empty
    CHECK(rc.handle(parseJson(R"({"op":"npu_morph","cmd":"clear"})")).find("ok")->boolean());
    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"anchors"})"));
    CHECK(r.find("anchors")->size() == 0);
    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"compile"})"));
    CHECK(!r.find("ok")->boolean());
    r = rc.handle(parseJson(R"({"op":"npu_morph","cmd":"eval","x":0.5,"y":0.5})"));
    CHECK(!r.find("ok")->boolean());
}

TEST(backing_four_stem_sync_mix_solo_mute) {
    const std::string dir = tempDir("stems");
    const std::string paths[4] = {
        writeStereoSineWav(dir + "/vocals.wav", 1.8, 220.0, 220.0, 0.20f),
        writeStereoSineWav(dir + "/drums.wav",  1.8, 440.0, 440.0, 0.20f),
        writeStereoSineWav(dir + "/bass.wav",   1.8, 110.0, 110.0, 0.20f),
        writeStereoSineWav(dir + "/other.wav",  1.8, 880.0, 880.0, 0.20f),
    };

    Engine e;
    e.prepare(kSr);
    e.setParam(P::MasterVolume, 1.0f);
    e.setParam(P::Limiter, 0.0f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);

    auto& bp = e.backingPlayer();
    const Json loaded = bp.loadStems(paths);
    CHECK(loaded.find("ok") && loaded.find("ok")->boolean());
    CHECK(bp.isStemsLoaded());
    CHECK_NEAR(loaded.find("dur")->number(), 1.8, 1e-3);
    CHECK((int)loaded.find("sr")->number() == kSrInt);
    CHECK((int)loaded.find("ch")->number() == 2);

    const auto pk = bp.peaks(64);
    CHECK(pk.size() == 128);
    float pkMax = -1.0f;
    for (size_t i = 0; i < pk.size(); ++i) pkMax = std::max(pkMax, pk[i]);
    CHECK(pkMax > 0.3f);

    // 1. All 4 stems summed when unmuted
    for (int s = 0; s < 4; ++s) bp.setStemMix(s, 1.0f, false, false);
    bp.play(true);
    waitBackingReady();
    std::vector<float> L, R;
    renderBacking(e, 9600, L, R);  // 200 ms
    CHECK(allFinite(L) && allFinite(R));
    const double gVocAll = goertzel(L, kSr, 220.0, 2400, 4096);
    const double gDrmAll = goertzel(L, kSr, 440.0, 2400, 4096);
    const double gBasAll = goertzel(L, kSr, 110.0, 2400, 4096);
    const double gOthAll = goertzel(L, kSr, 880.0, 2400, 4096);
    INFO("4-stem all unmuted: 220=%.4f, 440=%.4f, 110=%.4f, 880=%.4f\n", gVocAll, gDrmAll, gBasAll, gOthAll);
    CHECK(gVocAll > 0.12 && gDrmAll > 0.12 && gBasAll > 0.12 && gOthAll > 0.12);

    // 2. Soloing stem 1 (drums = 440 Hz) isolates ONLY stem 1's energy after 10 ms ramp
    bp.setStemMix(1, 1.0f, false, true);
    L.clear(); R.clear();
    renderBacking(e, 6000, L, R);  // 125 ms (> 10 ms ramp)
    CHECK(allFinite(L) && allFinite(R));
    const double gVocSolo = goertzel(L, kSr, 220.0, 1500, 4096);
    const double gDrmSolo = goertzel(L, kSr, 440.0, 1500, 4096);
    const double gBasSolo = goertzel(L, kSr, 110.0, 1500, 4096);
    const double gOthSolo = goertzel(L, kSr, 880.0, 1500, 4096);
    INFO("4-stem solo(1): 220=%.5f, 440=%.4f, 110=%.5f, 880=%.5f\n", gVocSolo, gDrmSolo, gBasSolo, gOthSolo);
    CHECK(gDrmSolo > 0.12);
    CHECK(gVocSolo < 0.005 && gBasSolo < 0.005 && gOthSolo < 0.005);

    // 3. Unsolo stem 1, mute stem 0 (vocals = 220 Hz) -> removes ONLY stem 0
    bp.setStemMix(1, 1.0f, false, false);
    bp.setStemMix(0, 1.0f, true, false);
    L.clear(); R.clear();
    renderBacking(e, 6000, L, R);
    CHECK(allFinite(L) && allFinite(R));
    const double gVocMute = goertzel(L, kSr, 220.0, 1500, 4096);
    const double gDrmMute = goertzel(L, kSr, 440.0, 1500, 4096);
    const double gBasMute = goertzel(L, kSr, 110.0, 1500, 4096);
    const double gOthMute = goertzel(L, kSr, 880.0, 1500, 4096);
    INFO("4-stem mute(0): 220=%.5f, 440=%.4f, 110=%.4f, 880=%.4f\n", gVocMute, gDrmMute, gBasMute, gOthMute);
    CHECK(gVocMute < 0.005);
    CHECK(gDrmMute > 0.12 && gBasMute > 0.12 && gOthMute > 0.12);

    // 4. Gain ramping (stem 2 gain -> 0.5) is click-free and matches expected amplitude
    bp.setStemMix(2, 0.5f, false, false);
    L.clear(); R.clear();
    renderBacking(e, 4800, L, R);
    CHECK(allFinite(L) && allFinite(R));
    double maxStep = 0.0;
    for (size_t i = 1; i < L.size(); ++i) maxStep = std::max(maxStep, double(std::fabs(L[i] - L[i - 1])));
    const double gBasHalf = goertzel(L, kSr, 110.0, 1200, 3072);
    INFO("4-stem gain(2)=0.5: maxStep=%.4f, 110Hz=%.4f\n", maxStep, gBasHalf);
    CHECK(maxStep < 0.08);
    CHECK_NEAR(gBasHalf, gBasMute * 0.5, 0.025);

    const Json stemJson = bp.stemStateJson();
    CHECK(stemJson.find("active")->boolean());
    CHECK(stemJson.find("stems")->size() == 4);
    CHECK(stemJson.find("stems")->at(0).find("mute")->boolean());
    CHECK_NEAR(stemJson.find("stems")->at(2).find("gain")->number(), 0.5, 1e-4);

    // 5. Clean stop fade
    bp.play(false);
    std::vector<float> stopL, stopR;
    renderAppend(e, kSrInt / 100 + 64, stopL, stopR);
    CHECK(allFinite(stopL) && allFinite(stopR));
    CHECK(!bp.active());
    CHECK(stopL.back() == 0.0f && stopR.back() == 0.0f);

    // 6. Mismatched stem length fails gracefully
    const std::string shortStem = writeStereoSineWav(dir + "/short.wav", 0.5, 220.0, 220.0, 0.2f);
    const std::string badPaths[4] = {shortStem, paths[1], paths[2], paths[3]};
    CHECK(!bp.loadStems(badPaths).find("ok")->boolean());
}

TEST(stft_extract_reconstruct_and_fast_spectral) {
    const std::string dir = tempDir("stft");
    constexpr int kTestSr = 44100;
    const size_t nSamples = size_t(1.5 * kTestSr);
    std::vector<float> srcL(nSamples, 0.0f), srcR(nSamples, 0.0f);

    // 75 Hz bass (center), 600 Hz vocal (center), 7500 Hz high tone (stereo side-shifted) + short percussive clicks
    for (size_t i = 0; i < nSamples; ++i) {
        const double t = double(i) / kTestSr;
        const float bass = 0.30f * float(std::sin(2.0 * kPi * 75.0 * t));
        const float vocal = 0.25f * float(std::sin(2.0 * kPi * 600.0 * t));
        const float high = 0.18f * float(std::sin(2.0 * kPi * 7500.0 * t));
        srcL[i] = bass + vocal + high;
        srcR[i] = bass + vocal - high;
    }
    const std::string wavPath = dir + "/mix.wav";
    std::string err;
    CHECK(writeWav(wavPath, srcL, &srcR, kTestSr, false, err));

    // 1. stftExtractMag -> [frame][1024][2] float32
    const std::string magPath = dir + "/mag.f32";
    const Json magInfo = stftExtractMag(wavPath, magPath, 0);
    CHECK(magInfo.find("ok") && magInfo.find("ok")->boolean());
    const int64_t frames = (int64_t)magInfo.find("frames")->number();
    CHECK(frames > 0);
    CHECK((int)magInfo.find("bins")->number() == 1024);
    CHECK((int)magInfo.find("channels")->number() == 2);
    CHECK((int)magInfo.find("sr")->number() == kTestSr);

    const uint64_t expectedBytes = uint64_t(frames) * 1024u * 2u * sizeof(float);
    CHECK(fs::file_size(utf8Path(magPath)) == expectedBytes);
    {
        std::ifstream ifs(utf8Path(magPath), std::ios::binary);
        std::vector<float> mags(size_t(frames) * 1024u * 2u);
        CHECK(ifs.read(reinterpret_cast<char*>(mags.data()), (std::streamsize)expectedBytes).good());
        CHECK(allFinite(mags));
        double maxMag = 0.0;
        for (float v : mags) {
            CHECK(v >= 0.0f);
            maxMag = std::max(maxMag, double(v));
        }
        CHECK(maxMag > 10.0);
    }

    // 2. stftReconstructStems with 4 uniform 0.25 masks -> reconstructs equal quarters matching source energy
    std::string maskPaths[4];
    const std::vector<float> uniformMask(size_t(frames) * 1024u * 2u, 0.25f);
    for (int s = 0; s < 4; ++s) {
        maskPaths[s] = dir + "/mask_" + std::to_string(s) + ".f32";
        std::ofstream ofs(utf8Path(maskPaths[s]), std::ios::binary);
        CHECK(ofs.write(reinterpret_cast<const char*>(uniformMask.data()), (std::streamsize)expectedBytes).good());
    }
    const std::string reconDir = dir + "/recon";
    const Json reconReply = stftReconstructStems(wavPath, maskPaths, reconDir);
    CHECK(reconReply.find("ok") && reconReply.find("ok")->boolean());

    const char* kStemNames[4] = {"vocals", "drums", "bass", "other"};
    std::vector<float> sumL(nSamples, 0.0f), sumR(nSamples, 0.0f);
    for (int s = 0; s < 4; ++s) {
        WavData wd;
        std::string werr;
        const std::string stemPath = reconDir + "/" + kStemNames[s] + ".wav";
        CHECK(readWav(stemPath, wd, werr));
        CHECK(wd.channels == 2);
        CHECK(wd.sampleRate == kTestSr);
        CHECK(wd.frames == (int64_t)nSamples);
        CHECK(allFinite(wd.left) && allFinite(wd.right));
        CHECK(rms(wd.left, 4096, nSamples - 4096) > 0.02);
        for (size_t i = 0; i < nSamples; ++i) {
            sumL[i] += wd.left[i];
            sumR[i] += wd.right[i];
        }
    }
    const double srcRms = rms(srcL, 4096, nSamples - 4096);
    const double sumRms = rms(sumL, 4096, nSamples - 4096);
    double diffSq = 0.0;
    for (size_t i = 4096; i < nSamples - 4096; ++i) {
        const double d = double(sumL[i]) - double(srcL[i]);
        diffSq += d * d;
    }
    const double errRms = std::sqrt(diffSq / double(nSamples - 8192));
    INFO("stftReconstructStems srcRms=%.4f, sumRms=%.4f, errRms=%.5f\n", srcRms, sumRms, errRms);
    CHECK_NEAR(sumRms, srcRms, 0.01);
    CHECK(errRms < 0.01);

    // 3. separateStemsFastSpectral -> outputs 4 valid stereo WAVs with expected frequency separation
    const std::string fastDir = dir + "/fast";
    const Json fastReply = separateStemsFastSpectral(wavPath, fastDir);
    CHECK(fastReply.find("ok") && fastReply.find("ok")->boolean());

    WavData fastStems[4];
    for (int s = 0; s < 4; ++s) {
        std::string werr;
        CHECK(readWav(fastDir + "/" + kStemNames[s] + ".wav", fastStems[s], werr));
        CHECK(fastStems[s].channels == 2);
        CHECK(fastStems[s].sampleRate == kTestSr);
        CHECK(fastStems[s].frames == (int64_t)nSamples);
        CHECK(allFinite(fastStems[s].left) && allFinite(fastStems[s].right));
    }

    const double bass75 = goertzel(fastStems[2].left, kTestSr, 75.0, 8192, 16384);
    const double other75 = goertzel(fastStems[3].left, kTestSr, 75.0, 8192, 16384);
    const double vocal600 = goertzel(fastStems[0].left, kTestSr, 600.0, 8192, 16384);
    const double bass600 = goertzel(fastStems[2].left, kTestSr, 600.0, 8192, 16384);
    const double other7500 = goertzel(fastStems[3].left, kTestSr, 7500.0, 8192, 16384);
    const double bass7500 = goertzel(fastStems[2].left, kTestSr, 7500.0, 8192, 16384);
    INFO("fastSpectral: bass75=%.4f vs other75=%.4f, vocal600=%.4f vs bass600=%.4f, other7500=%.4f vs bass7500=%.4f\n",
         bass75, other75, vocal600, bass600, other7500, bass7500);
    CHECK(bass75 > other75 * 4.0);
    CHECK(vocal600 > bass600 * 4.0);
    CHECK(other7500 > bass7500 * 4.0);
}

TEST(formats_sf2_and_dls_gig_synthetic_and_fuzz) {
    const std::string dir = tempDir("sf2_dls_gig");

    // 1. SF2 synthetic fixture
    {
        const std::string sf2Path = dir + "/test.sf2";
        CHECK(writeBytesFile(sf2Path, buildSyntheticSf2(4800, 48000)));

        std::vector<Sf2PresetInfo> list;
        std::string err;
        CHECK(listSf2Presets(sf2Path, list, err));
        CHECK(list.size() == 1);
        CHECK(list[0].bank == 0 && list[0].program == 0 && list[0].name == "TestPiano");

        SampleSet set;
        std::vector<std::string> warnings;
        CHECK(loadSf2Preset(sf2Path, 0, 0, set, err, warnings));
        CHECK(warnings.empty());
        CHECK(set.name == "TestPiano");
        CHECK(set.samples.size() == 1);
        CHECK(set.samples[0]->frames == 4800);
        CHECK(set.regions.size() == 1);
        CHECK(set.regions[0].lokey == 36 && set.regions[0].hikey == 84);
        CHECK(set.regions[0].keycenter == 69);
        CHECK(set.regions[0].loopMode == LoopMode::Continuous);
        CHECK(set.regions[0].loopStart == 100 && set.regions[0].loopEnd == 4700);
        CHECK(allFinite(set.samples[0]->data[0]));

        // Also load via Engine host_utils
        Engine e;
        e.prepare(kSr);
        CHECK(loadPartSoundBankPreset(e, 0, 0, sf2Path, 0, 0, false, err, warnings));
        CHECK(e.partSampleSetName(0, 0) == "TestPiano");
    }

    // 2. DLS2 synthetic fixture
    {
        const std::string dlsPath = dir + "/test.dls";
        CHECK(writeBytesFile(dlsPath, buildSyntheticDls2(4800, 48000)));

        std::vector<DlsPresetInfo> list;
        std::string err;
        CHECK(listDlsGigPresets(dlsPath, list, err));
        CHECK(list.size() == 1);
        CHECK(list[0].bank == 0 && list[0].program == 0 && !list[0].isDrum && list[0].name == "TestDLS2");

        SampleSet set;
        std::vector<std::string> warnings;
        CHECK(loadDlsGigPreset(dlsPath, 0, 0, false, set, err, warnings));
        CHECK(warnings.empty());
        CHECK(set.name == "TestDLS2");
        CHECK(set.samples.size() == 1);
        CHECK(set.samples[0]->frames == 4800);
        CHECK(set.regions.size() == 1);
        CHECK(set.regions[0].lokey == 36 && set.regions[0].hikey == 84);
        CHECK(set.regions[0].keycenter == 69);
        CHECK(set.regions[0].loopMode == LoopMode::Continuous);
        CHECK(set.regions[0].loopStart == 100 && set.regions[0].loopEnd == 4700);
        CHECK(set.regions[0].ampeg.enabled);
        CHECK(allFinite(set.samples[0]->data[0]));
    }

    // 3. GIG v2 synthetic fixture
    {
        const std::string gigPath = dir + "/test.gig";
        CHECK(writeBytesFile(gigPath, buildSyntheticGig2(4800, 48000)));

        std::vector<DlsPresetInfo> list;
        std::string err;
        CHECK(listDlsGigPresets(gigPath, list, err));
        CHECK(list.size() == 1);
        CHECK(list[0].bank == 0 && list[0].program == 1 && list[0].name == "TestGIG2");

        SampleSet set;
        std::vector<std::string> warnings;
        CHECK(loadDlsGigPreset(gigPath, 0, 1, false, set, err, warnings));
        CHECK(warnings.empty());
        CHECK(set.name == "TestGIG2");
        CHECK(set.samples.size() == 1);
        CHECK(set.regions.size() == 2);
        CHECK(set.regions[0].lokey == 48 && set.regions[0].hikey == 72);
        CHECK(set.regions[0].lovel == 0 && set.regions[0].hivel == 63);
        CHECK(set.regions[1].lovel == 64 && set.regions[1].hivel == 127);
        CHECK(set.regions[0].loopMode == LoopMode::Continuous);
        CHECK(set.regions[0].loopStart == 100 && set.regions[0].loopEnd == 4700);
        CHECK(allFinite(set.samples[0]->data[0]));
    }

    // 4. Fuzz / corrupted buffers -> graceful false return, zero crashes
    const std::vector<std::vector<uint8_t>> badBuffers = {
        {},
        {'B', 'A', 'D', '!', 0, 0, 0, 0, 0, 0, 0, 0},
        {'R', 'I', 'F', 'F', 0xFF, 0xFF, 0xFF, 0xFF, 's', 'f', 'b', 'k', 'L', 'I', 'S', 'T', 0xFF, 0xFF, 0xFF, 0x7F},
        {'R', 'I', 'F', 'F', 0xFF, 0xFF, 0xFF, 0xFF, 'D', 'L', 'S', ' ', 'L', 'I', 'S', 'T', 0xFF, 0xFF, 0xFF, 0x7F},
    };
    for (size_t i = 0; i < badBuffers.size(); ++i) {
        const std::string pSf2 = dir + "/bad_" + std::to_string(i) + ".sf2";
        const std::string pDls = dir + "/bad_" + std::to_string(i) + ".dls";
        CHECK(writeBytesFile(pSf2, badBuffers[i]));
        CHECK(writeBytesFile(pDls, badBuffers[i]));

        std::string err;
        std::vector<std::string> warnings;
        std::vector<Sf2PresetInfo> sfList;
        std::vector<DlsPresetInfo> dlsList;
        SampleSet set;
        CHECK(!listSf2Presets(pSf2, sfList, err));
        CHECK(!loadSf2Preset(pSf2, -1, -1, set, err, warnings));
        CHECK(!listDlsGigPresets(pDls, dlsList, err));
        CHECK(!loadDlsGigPreset(pDls, -1, -1, false, set, err, warnings));
    }

    // Truncated mid-way copies of valid SF2, DLS2, and GIG2
    const auto validSf2 = buildSyntheticSf2(2048, 48000);
    const auto validDls = buildSyntheticDls2(2048, 48000);
    const auto validGig = buildSyntheticGig2(2048, 48000);
    for (size_t frac : {16u, 48u, 128u}) {
        std::string err;
        std::vector<std::string> warnings;
        SampleSet set;
        const std::string tSf2 = dir + "/trunc_" + std::to_string(frac) + ".sf2";
        const std::string tDls = dir + "/trunc_" + std::to_string(frac) + ".dls";
        const std::string tGig = dir + "/trunc_" + std::to_string(frac) + ".gig";
        CHECK(writeBytesFile(tSf2, std::vector<uint8_t>(validSf2.begin(), validSf2.begin() + std::min(frac, validSf2.size()))));
        CHECK(writeBytesFile(tDls, std::vector<uint8_t>(validDls.begin(), validDls.begin() + std::min(frac, validDls.size()))));
        CHECK(writeBytesFile(tGig, std::vector<uint8_t>(validGig.begin(), validGig.begin() + std::min(frac, validGig.size()))));
        CHECK(!loadSf2Preset(tSf2, -1, -1, set, err, warnings));
        CHECK(!loadDlsGigPreset(tDls, -1, -1, false, set, err, warnings));
        CHECK(!loadDlsGigPreset(tGig, -1, -1, false, set, err, warnings));
    }
}

TEST(formats_decent_sampler_and_dx7_sysex) {
    const std::string dir = tempDir("ds_dx7");

    // 1. Decent Sampler .dspreset
    {
        const std::string dsDir = dir + "/ds";
        fs::create_directories(utf8Path(dsDir));
        writeStereoSineWav(dsDir + "/sine60.wav", 0.25, 261.63, 261.63, 0.5f);

        const std::string xml = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<DecentSampler minVersion="1.0.0">
  <ui width="812" height="375">
    <tab name="main"><labeled-knob x="10" y="10" label="Cutoff"/></tab>
  </ui>
  <groups attack="0.01" decay="0.15" sustain="0.85" release="0.25">
    <group name="Main" volume="-3dB" tuning="0.0">
      <sample path="sine60.wav" rootNote="C4" loNote="36" hiNote="84" loVel="1" hiVel="127"
              loopEnabled="true" loopStart="100" loopEnd="11000" loopCrossfade="240" tags="grpA"/>
    </group>
  </groups>
  <effects>
    <effect type="reverb" wetLevel="0.25"/>
    <effect type="lowpass" frequency="4200"/>
  </effects>
</DecentSampler>
)xml";
        const std::string dsPath = dsDir + "/test.dspreset";
        CHECK(writeTextFile(dsPath, xml));

        SampleSet set;
        std::string err;
        std::vector<std::string> warnings;
        CHECK(loadDecentSamplerPreset(dsPath, set, err, warnings));
        CHECK(warnings.empty());
        CHECK(set.samples.size() == 1);
        CHECK(set.samples[0]->frames == 12000);
        CHECK(set.regions.size() == 1);
        CHECK(set.regions[0].keycenter == 60);
        CHECK(set.regions[0].lokey == 36 && set.regions[0].hikey == 84);
        CHECK(set.regions[0].loopMode == LoopMode::Continuous);
        CHECK(set.regions[0].loopStart == 100 && set.regions[0].loopEnd == 11000);
        CHECK_NEAR(set.regions[0].volumeDb, -3.0f, 0.05f);
        CHECK(set.regions[0].ampeg.enabled);

        // Corrupted / empty .dspreset -> graceful false
        const std::string badXml = dsDir + "/bad.dspreset";
        CHECK(writeTextFile(badXml, "<DecentSampler><groups><group><sample path=\"missing.wav\""));
        SampleSet badSet;
        CHECK(!loadDecentSamplerPreset(badXml, badSet, err, warnings));
        const std::string emptyXml = dsDir + "/empty.dspreset";
        CHECK(writeTextFile(emptyXml, ""));
        CHECK(!loadDecentSamplerPreset(emptyXml, badSet, err, warnings));
    }

    // 2. DX7 SysEx (.syx): 32-voice VMEM (4104 bytes) & 1-voice VCED (163 bytes)
    {
        const auto vmemBytes = buildDx7VmemSyx();
        CHECK(vmemBytes.size() == 4104);
        const std::string vmemPath = dir + "/bank32.syx";
        CHECK(writeBytesFile(vmemPath, vmemBytes));

        std::vector<Preset> presets;
        std::vector<Dx7VoiceSummary> summaries;
        std::string err;
        CHECK(loadDx7SysexFile(vmemPath, presets, err, &summaries));
        CHECK(presets.size() == 32);
        CHECK(summaries.size() == 32);
        CHECK(summaries[0].name == "DX7P01" && summaries[0].algorithm == 1);
        CHECK(summaries[31].name == "DX7P32" && summaries[31].algorithm == 32);

        // Apply voice 0 to Engine FM and verify audible output
        Engine e;
        e.prepare(kSr);
        std::vector<std::string> warnings;
        CHECK(applyPartPreset(e, 0, presets[0], "", warnings));
        CHECK((LayerType)(int)e.getPartParam(0, P::layer(0, P::LType)) == LayerType::FM);
        e.sendEvent(Event::noteOn(1, 60, 0.9f));
        std::vector<float> L, R;
        renderAppend(e, 4800, L, R);
        CHECK(allFinite(L) && allFinite(R));
        CHECK(rms(L, 512, 4096) > 0.02);

        // Import bank to flat <prefix>_01_<name>.json files and verify valid JSON + loadPresetFile
        const std::string outDir = dir + "/dx7_presets";
        fs::create_directories(utf8Path(outDir));
        for (size_t i = 0; i < presets.size(); ++i) {
            const std::string num = (i < 9 ? "0" : "") + std::to_string(i + 1);
            const std::string pPath = outDir + "/DX7_" + num + "_" + presets[i].name + ".json";
            CHECK(savePresetFile(pPath, presets[i], err));
            Preset reloaded;
            CHECK(loadPresetFile(pPath, reloaded, err));
            CHECK(reloaded.name == presets[i].name);
            CHECK(reloaded.warnings.empty());
        }
        CHECK(fs::exists(utf8Path(outDir + "/DX7_01_DX7P01.json")));
        CHECK(fs::exists(utf8Path(outDir + "/DX7_32_DX7P32.json")));

        // 1-voice VCED (163 bytes)
        const auto vcedBytes = buildDx7VcedSyx("VCEDLEAD  ", 4);
        CHECK(vcedBytes.size() == 163);
        const std::string vcedPath = dir + "/single.syx";
        CHECK(writeBytesFile(vcedPath, vcedBytes));
        std::vector<Preset> vcedPresets;
        std::vector<Dx7VoiceSummary> vcedSums;
        CHECK(loadDx7SysexFile(vcedPath, vcedPresets, err, &vcedSums));
        CHECK(vcedPresets.size() == 1);
        CHECK(vcedSums.size() == 1);
        CHECK(vcedSums[0].name == "VCEDLEAD" && vcedSums[0].algorithm == 5);

        // Corrupted / bad-length .syx buffers -> graceful false
        const std::vector<uint8_t> badSyx1 = {0xF0, 0x41, 0x00, 0x09, 0x00, 0xF7};  // wrong manufacturer
        const std::vector<uint8_t> badSyx2(100, 0xF0);                              // unterminated / wrong size
        std::vector<Preset> badOut;
        CHECK(!parseDx7Sysex(nullptr, 0, badOut, err));
        CHECK(!parseDx7Sysex(badSyx1.data(), badSyx1.size(), badOut, err));
        CHECK(!parseDx7Sysex(badSyx2.data(), badSyx2.size(), badOut, err));
    }
}

TEST(formats_wavetable_surge_vital_and_preview) {
    const std::string dir = tempDir("wavetables");

    // 1. Surge .wt (float32 and int16)
    for (bool isInt16 : {false, true}) {
        const std::string wtPath = dir + (isInt16 ? "/surge_i16.wt" : "/surge_f32.wt");
        CHECK(writeBytesFile(wtPath, buildSurgeWt(256, 4, isInt16)));

        Wavetable wt;
        std::string err;
        CHECK(loadAnyWavetableFile(wtPath, wt, err));
        CHECK(wt.frames == 4);
        CHECK(wt.size[0] == 4096);
        CHECK(allFinite(wt.data[0]));

        std::array<float, 128> p0{}, p1{};
        sampleWavetablePreview(wt, 0.0f, 128, p0.data());
        sampleWavetablePreview(wt, 1.0f, 128, p1.data());
        float max0 = 0.0f, max1 = 0.0f, diff = 0.0f;
        for (int i = 0; i < 128; ++i) {
            CHECK(std::isfinite(p0[i]) && std::isfinite(p1[i]));
            max0 = std::max(max0, std::fabs(p0[i]));
            max1 = std::max(max1, std::fabs(p1[i]));
            diff = std::max(diff, std::fabs(p0[i] - p1[i]));
        }
        CHECK(max0 > 0.5f && max1 > 0.5f);
        CHECK(diff > 0.1f);

        // Load via host_utils into Engine
        Engine e;
        e.prepare(kSr);
        Wavetable preview;
        CHECK(loadPartWavetable(e, 0, 0, wtPath, err, &preview));
        CHECK(preview.frames == 4);
    }

    // 2. Vital .vitaltable (JSON with base64 float32 keyframes)
    {
        constexpr int kPts = 256;
        std::vector<float> kf0(kPts), kf1(kPts);
        for (int i = 0; i < kPts; ++i) {
            const float ph = float(i) / float(kPts);
            kf0[i] = std::sin(2.0f * kPi * ph);
            kf1[i] = 0.6f * std::sin(2.0f * kPi * ph) + 0.4f * std::sin(6.0f * kPi * ph);
        }
        const std::string b64_0 = encodeBase64(reinterpret_cast<const uint8_t*>(kf0.data()), kf0.size() * sizeof(float));
        const std::string b64_1 = encodeBase64(reinterpret_cast<const uint8_t*>(kf1.data()), kf1.size() * sizeof(float));

        Json root = Json::object();
        root.set("name", "VitalTest");
        Json kfs = Json::array();
        Json k0 = Json::object(); k0.set("position", 0); k0.set("wave_data", b64_0); kfs.push(std::move(k0));
        Json k1 = Json::object(); k1.set("position", 3); k1.set("wave_data", b64_1); kfs.push(std::move(k1));
        Json comp = Json::object();
        comp.set("type", "Wave Source");
        comp.set("keyframes", std::move(kfs));
        Json comps = Json::array(); comps.push(std::move(comp));
        Json grp = Json::object(); grp.set("components", std::move(comps));
        Json grps = Json::array(); grps.push(std::move(grp));
        root.set("groups", std::move(grps));

        const std::string vitalPath = dir + "/test.vitaltable";
        CHECK(writeTextFile(vitalPath, root.dump(2)));

        Wavetable wt;
        std::string err;
        CHECK(loadAnyWavetableFile(vitalPath, wt, err));
        CHECK(wt.name == "VitalTest");
        CHECK(wt.frames == 4);
        CHECK(wt.size[0] == 4096);
        CHECK(allFinite(wt.data[0]));

        std::array<float, 128> prev{};
        sampleWavetablePreview(wt, 0.5f, 128, prev.data());
        float pk = 0.0f;
        for (float v : prev) {
            CHECK(std::isfinite(v));
            pk = std::max(pk, std::fabs(v));
        }
        CHECK(pk > 0.5f);
    }

    // 3. Corrupted / invalid .wt and .vitaltable files -> clean false return
    {
        const std::string badWt1 = dir + "/bad_magic.wt";
        const std::string badWt2 = dir + "/trunc.wt";
        const std::string badVital1 = dir + "/bad_json.vitaltable";
        const std::string badVital2 = dir + "/empty.vitaltable";

        CHECK(writeBytesFile(badWt1, {'b', 'a', 'd', '!', 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}));
        CHECK(writeBytesFile(badWt2, {'v', 'a', 'w', 't', 0, 1, 0, 0, 4, 0, 0, 0, 1, 2, 3, 4}));
        CHECK(writeTextFile(badVital1, "{not valid json"));
        CHECK(writeTextFile(badVital2, R"({"name":"Empty","groups":[]})"));

        Wavetable wt;
        std::string err;
        CHECK(!loadAnyWavetableFile(badWt1, wt, err));
        CHECK(!loadAnyWavetableFile(badWt2, wt, err));
        CHECK(!loadAnyWavetableFile(badVital1, wt, err));
        CHECK(!loadAnyWavetableFile(badVital2, wt, err));
    }
}
