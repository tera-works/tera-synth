// Phase 4 (v0.8): Backing Audio Player, 16-Pad Sampler, Live Sampling Wizard,
// and External MIDI Zone Master routing + Scene Bank/PC + Mix::Count == 16.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "analysis.h"
#include "hsynth/backing_player.h"
#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/live_sampler.h"
#include "hsynth/midi_zone.h"
#include "hsynth/pad_sampler.h"
#include "hsynth/preset.h"
#include "hsynth/remote.h"
#include "hsynth/wav.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;
namespace fs = std::filesystem;

namespace {

constexpr float kSr = 48000.0f;
constexpr int kSrInt = 48000;

std::string tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / "hsynth_tests" / "v08_p4" / name;
    fs::create_directories(p);
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

// Feed the callback at the playback rate so the streaming worker can refill it.
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
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

Json parseJson(const std::string& s) {
    Json j;
    std::string err;
    CHECK(Json::parse(s, j, err));
    return j;
}

Preset makeCleanVaPreset(const std::string& name, VaWave wave = VaWave::Sine, float level = 0.5f) {
    Preset p;
    p.name = name;
    p.params[(size_t)P::MasterVolume] = 0.8f;
    p.params[(size_t)P::Limiter] = 0.0f;
    p.params[(size_t)P::FilterType] = (float)FilterType::Off;
    p.params[(size_t)P::AmpAttack] = 0.001f;
    p.params[(size_t)P::AmpDecay] = 0.1f;
    p.params[(size_t)P::AmpSustain] = 1.0f;
    p.params[(size_t)P::AmpRelease] = 0.02f;
    p.params[(size_t)P::AmpVelSens] = 0.0f;
    p.params[(size_t)P::fxp(fx::Reverb, fx::reverb::On)] = 0.0f;
    p.params[(size_t)P::fxp(fx::Delay, fx::delay::On)] = 0.0f;
    p.params[(size_t)P::fxp(fx::Chorus, fx::chorus::On)] = 0.0f;
    p.params[(size_t)P::mod(0, P::ModAmt)] = 0.0f;
    p.params[(size_t)P::mod(1, P::ModAmt)] = 0.0f;
    p.params[(size_t)P::mod(2, P::ModAmt)] = 0.0f;
    p.params[(size_t)P::layer(0, P::LType)] = (float)LayerType::VA;
    p.params[(size_t)P::layer(0, P::VaWave1)] = (float)wave;
    p.params[(size_t)P::layer(0, P::VaMix)] = 0.0f;
    p.params[(size_t)P::layer(0, P::LLevel)] = level;
    return p;
}

struct WireMsg {
    uint8_t slot = 0;
    std::vector<uint8_t> data;
};

std::vector<WireMsg> decodeWire(const std::vector<uint8_t>& bytes) {
    std::vector<WireMsg> out;
    size_t i = 0;
    while (i + 2 <= bytes.size()) {
        const uint8_t slot = bytes[i];
        const uint8_t sz = bytes[i + 1];
        i += 2;
        if (i + sz > bytes.size()) break;
        out.push_back({slot, std::vector<uint8_t>(bytes.begin() + ptrdiff_t(i), bytes.begin() + ptrdiff_t(i + sz))});
        i += sz;
    }
    return out;
}

bool hasWire3(const std::vector<WireMsg>& msgs, uint8_t slot, uint8_t b0, uint8_t b1, uint8_t b2) {
    for (const auto& m : msgs) {
        if (m.slot == slot && m.data.size() == 3 && m.data[0] == b0 && m.data[1] == b1 && m.data[2] == b2)
            return true;
    }
    return false;
}

bool hasWire2(const std::vector<WireMsg>& msgs, uint8_t slot, uint8_t b0, uint8_t b1) {
    for (const auto& m : msgs) {
        if (m.slot == slot && m.data.size() == 2 && m.data[0] == b0 && m.data[1] == b1)
            return true;
    }
    return false;
}

bool hasWireNotePrefix(const std::vector<WireMsg>& msgs, uint8_t slot, uint8_t status, uint8_t key) {
    for (const auto& m : msgs) {
        if (m.slot == slot && m.data.size() == 3 && m.data[0] == status && m.data[1] == key)
            return true;
    }
    return false;
}

}  // namespace

TEST(backing_audio_stream_ab_loop_speed_and_peaks) {
    const std::string dir = tempDir("backing");
    const std::string wavPath = writeStereoSineWav(dir + "/backing_2s.wav", 2.0, 440.0, 880.0, 0.5f);

    Engine e;
    e.prepare(kSr);
    e.setParam(P::MasterVolume, 1.0f);
    e.setParam(P::Limiter, 0.0f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);

    // Let the synth rack finish its startup quiet period before testing backing wake and idle.
    std::array<float, 256> quietL{}, quietR{};
    const int quietBlock = int(quietL.size());
    for (int done = 0; done < 5 * kSrInt && !e.isSilentIdle(); done += quietBlock)
        e.process(quietL.data(), quietR.data(), quietBlock);
    CHECK(e.isSilentIdle());

    auto& bp = e.backingPlayer();
    const Json loaded = bp.load(wavPath);
    CHECK(loaded.find("ok") && loaded.find("ok")->boolean());
    CHECK_NEAR(loaded.find("dur")->number(), 2.0, 1e-3);
    CHECK((int)loaded.find("sr")->number() == kSrInt);
    CHECK((int)loaded.find("ch")->number() == 2);

    // Peaks(64) returns 64 min/max pairs (128 floats) with non-zero stereo amplitude
    const std::vector<float> pk = bp.peaks(64);
    CHECK(pk.size() == 128);
    float pkMin = 1.0f, pkMax = -1.0f;
    for (size_t i = 0; i < 64; ++i) {
        pkMin = std::min(pkMin, pk[2 * i]);
        pkMax = std::max(pkMax, pk[2 * i + 1]);
    }
    CHECK(pkMin < -0.4f && pkMax > 0.4f);

    // 1. play(true) produces finite stereo audio matching 440 Hz L / 880 Hz R
    bp.play(true);
    waitBackingReady();
    std::vector<float> L, R;
    renderBacking(e, 12000, L, R);  // 0.25 s
    CHECK(!e.isSilentIdle());
    CHECK(allFinite(L) && allFinite(R));
    const double fL = zeroCrossFreq(L, kSr, 2400, 11000);
    const double fR = zeroCrossFreq(R, kSr, 2400, 11000);
    INFO("backing playback freq L=%.2f Hz, R=%.2f Hz\n", fL, fR);
    CHECK_NEAR(fL, 440.0, 3.0);
    CHECK_NEAR(fR, 880.0, 4.0);

    // 2. A-B loop [0.25, 0.75] wraps seamlessly past 0.75 s without clicks/drops
    bp.setLoop(0.25, 0.75, true);
    bp.seek(0.25);
    waitBackingReady();
    std::vector<float> loopL, loopR;
    renderBacking(e, int(0.85f * kSr), loopL, loopR);  // 0.85 s > 0.50 s loop span (wraps at 0.50 s)
    CHECK(allFinite(loopL) && allFinite(loopR));
    const auto stLoop = bp.state();
    CHECK_NEAR(stLoop[2], 1.0, 1e-6);  // playing
    CHECK_NEAR(stLoop[3], 0.25, 1e-4);
    CHECK_NEAR(stLoop[4], 0.75, 1e-4);
    CHECK_NEAR(stLoop[5], 1.0, 1e-6);  // loop on
    CHECK(stLoop[0] >= 0.25 - 1e-3 && stLoop[0] <= 0.75 + 1e-3);

    // Check continuity across the entire 0.85 s looped render (including wrap around frame 24000)
    double maxStepL = 0.0, maxStepR = 0.0;
    for (size_t i = 2000; i < loopL.size(); ++i) {
        maxStepL = std::max(maxStepL, double(std::fabs(loopL[i] - loopL[i - 1])));
        maxStepR = std::max(maxStepR, double(std::fabs(loopR[i] - loopR[i - 1])));
    }
    double minWinRms = 1.0;
    for (size_t off = 2000; off + 1024 <= loopL.size(); off += 512) {
        minWinRms = std::min(minWinRms, rms(loopL, off, off + 1024));
    }
    INFO("backing loop maxStepL=%.4f, maxStepR=%.4f, minWinRms=%.4f\n", maxStepL, maxStepR, minWinRms);
    CHECK(maxStepL < 0.12 && maxStepR < 0.15);
    CHECK(minWinRms > 0.10);

    // 3. Speed changes source time; pitch shifts both channels independently.
    bp.setLoop(0.0, 2.0, false);
    bp.setSpeed(1.5f);
    bp.setPitch(0.0f);
    bp.seek(0.0);
    waitBackingReady();
    std::vector<float> spL, spR;
    renderBacking(e, int(0.40f * kSr), spL, spR);  // 0.40 s output -> ~0.60 s source position
    CHECK(allFinite(spL) && allFinite(spR));
    const double posAfterSpeed = bp.state()[0];
    const double fSpeedL = zeroCrossFreq(spL, kSr, 2400, 16000);
    const double fSpeedR = zeroCrossFreq(spR, kSr, 2400, 16000);
    INFO("backing speed=1.5 pos=%.3f s, fL=%.2f Hz, fR=%.2f Hz\n", posAfterSpeed, fSpeedL, fSpeedR);
    CHECK_NEAR(posAfterSpeed, 0.60, 0.001);
    CHECK_NEAR(fSpeedL, 440.0, 3.0);
    CHECK_NEAR(fSpeedR, 880.0, 4.0);

    bp.setPitch(2.0f);  // Keep the current source position at 1.5x speed.
    waitBackingReady();
    std::vector<float> ptL, ptR;
    renderBacking(e, int(0.40f * kSr), ptL, ptR);
    CHECK(allFinite(ptL) && allFinite(ptR));
    const double posAfterPitch = bp.state()[0];
    const double fPitchL = zeroCrossFreq(ptL, kSr, 2400, 16000);
    const double fPitchR = zeroCrossFreq(ptR, kSr, 2400, 16000);
    INFO("backing speed=1.5 pitch=+2 pos=%.3f s, fL=%.2f Hz, fR=%.2f Hz\n", posAfterPitch, fPitchL, fPitchR);
    CHECK_NEAR(posAfterPitch, posAfterSpeed + 0.60, 0.001);
    CHECK(rms(ptL, 2400, ptL.size()) > 0.10 && rms(ptR, 2400, ptR.size()) > 0.10);
    const double pitchRatio = std::exp2(2.0 / 12.0);
    CHECK_NEAR(fPitchL, 440.0 * pitchRatio, 3.0);
    CHECK_NEAR(fPitchR, 880.0 * pitchRatio, 4.0);

    // 4. Stop completes its 10 ms fade, then the engine's 0.5 s idle timer runs.
    bp.play(false);
    std::vector<float> stopL, stopR;
    renderAppend(e, 128, stopL, stopR);
    CHECK(bp.active());
    renderAppend(e, kSrInt / 100 - 128, stopL, stopR);
    CHECK(!bp.active());
    CHECK(stopL.back() == 0.0f && stopR.back() == 0.0f);
    stopL.clear();
    stopR.clear();
    renderAppend(e, int(0.52f * kSr), stopL, stopR);
    CHECK(peak(stopL) == 0.0 && peak(stopR) == 0.0);
    CHECK(e.isSilentIdle());
    std::vector<float> idleL, idleR;
    renderAppend(e, 512, idleL, idleR, 128);
    CHECK(peak(idleL) == 0.0 && peak(idleR) == 0.0);

    // 5. Remote "backing" op roundtrip
    RemoteControl rc(e, RemoteControl::Config{});
    Json r = rc.handle(parseJson(std::string(R"({"op":"backing","cmd":"load","path":")") + wavPath + "\"}"));
    CHECK(r.find("ok")->boolean());
    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"loop","a":0.2,"b":0.8,"on":true})")).find("ok")->boolean());
    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"speed","r":1.25})")).find("ok")->boolean());
    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"pitch","semis":2})")).find("ok")->boolean());
    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"volume","v":0.8})")).find("ok")->boolean());
    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"seek","sec":0.3})")).find("ok")->boolean());
    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"route","direct":false})")).find("ok")->boolean());
    CHECK(!bp.direct());
    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"route","direct":true})")).find("ok")->boolean());
    CHECK(bp.direct());
    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"play","on":true})")).find("ok")->boolean());

    r = rc.handle(parseJson(R"({"op":"backing","cmd":"peaks","bins":64})"));
    CHECK(r.find("ok")->boolean());
    CHECK(r.find("peaks") && r.find("peaks")->isArray() && r.find("peaks")->size() == 128);

    r = rc.handle(parseJson(R"({"op":"backing","cmd":"state"})"));
    CHECK(r.find("ok")->boolean());
    const Json* st = r.find("state");
    CHECK(st && st->isArray() && st->size() == 6);
    CHECK_NEAR(st->at(1).number(), 2.0, 1e-3);
    CHECK_NEAR(st->at(2).number(), 1.0, 1e-6);
    CHECK_NEAR(st->at(3).number(), 0.2, 1e-4);
    CHECK_NEAR(st->at(4).number(), 0.8, 1e-4);
    CHECK_NEAR(st->at(5).number(), 1.0, 1e-6);

    CHECK(rc.handle(parseJson(R"({"op":"backing","cmd":"play","on":false})")).find("ok")->boolean());
}

TEST(pad_sampler_poly_choke_modes_and_midi) {
    const std::string dir = tempDir("pads");
    const std::string p0Path = writeStereoSineWav(dir + "/pad0.wav", 0.12, 220.0, 220.0, 0.5f);
    const std::string p1Path = writeStereoSineWav(dir + "/pad1.wav", 0.12, 440.0, 440.0, 0.5f);
    const std::string p2Path = writeStereoSineWav(dir + "/pad2.wav", 0.20, 660.0, 660.0, 0.5f);
    const std::string p3Path = writeStereoSineWav(dir + "/pad3.wav", 0.05, 880.0, 880.0, 0.5f);

    Engine e;
    e.prepare(kSr);
    e.setParam(P::MasterVolume, 1.0f);
    e.setParam(P::Limiter, 0.0f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);

    auto& pads = e.padSampler();
    CHECK(pads.load(0, p0Path).find("ok")->boolean());
    CHECK(pads.load(1, p1Path).find("ok")->boolean());
    CHECK(pads.load(2, p2Path).find("ok")->boolean());
    CHECK(pads.load(3, p3Path).find("ok")->boolean());

    // Pad 0 & 1: OneShot (mode=0), choke=1
    pads.setup(0, 1.0f, 0.0f, 0, 1, 1, 0.0f, 1.0f);
    pads.setup(1, 1.0f, 0.0f, 0, 1, 1, 0.0f, 1.0f);
    // Pad 2: Gate (mode=2), choke=0
    pads.setup(2, 1.0f, 0.0f, 2, 0, 1, 0.0f, 1.0f);
    // Pad 3: Loop (mode=1), choke=0
    pads.setup(3, 1.0f, 0.0f, 1, 0, 1, 0.0f, 1.0f);

    // 1. OneShot plays to completion
    {
        pads.trigger(0, 1.0f);
        std::vector<float> L, R;
        renderAppend(e, int(0.05f * kSr), L, R);
        CHECK(allFinite(L) && allFinite(R));
        CHECK(goertzel(L, kSr, 220.0, 512, 1500) > 0.15);
        CHECK(pads.levels()[0] > 0.1f);

        // Render past 0.12 s duration -> finishes cleanly
        renderAppend(e, int(0.12f * kSr), L, R);
        CHECK(rms(L, L.size() - 1024, L.size()) == 0.0);
        CHECK(pads.levels()[0] == 0.0f);
    }

    // 2. Gate stops on release (and via MIDI ch 10 / 0-indexed ch 9, key 36+2=38)
    {
        pads.trigger(2, 1.0f);
        std::vector<float> L, R;
        renderAppend(e, int(0.04f * kSr), L, R);
        CHECK(goertzel(L, kSr, 660.0, 256, 1200) > 0.15);
        CHECK(pads.levels()[2] > 0.1f);

        pads.release(2);
        renderAppend(e, int(0.02f * kSr), L, R);  // > 4 ms release fade
        CHECK(rms(L, L.size() - 256, L.size()) == 0.0);
        CHECK(pads.levels()[2] == 0.0f);

        // MIDI channel 9 (MIDI ch 10), note 38 (pad 2)
        Event mOn = Event::noteOn(10, 38, 0.9f);
        mOn.channel = 9;
        e.sendEvent(mOn);
        L.clear(); R.clear();
        renderAppend(e, int(0.03f * kSr), L, R);
        CHECK(goertzel(L, kSr, 660.0, 256, 1000) > 0.10);
        CHECK(pads.levels()[2] > 0.05f);

        Event mOff = Event::noteOff(10, 38);
        mOff.channel = 9;
        e.sendEvent(mOff);
        renderAppend(e, int(0.02f * kSr), L, R);
        CHECK(rms(L, L.size() - 256, L.size()) == 0.0);
        CHECK(pads.levels()[2] == 0.0f);
    }

    // 3. Loop sustains continuously past clip length (0.05 s -> render 0.20 s)
    {
        pads.trigger(3, 1.0f);
        std::vector<float> L, R;
        renderAppend(e, int(0.20f * kSr), L, R);
        CHECK(allFinite(L) && allFinite(R));
        CHECK(goertzel(L, kSr, 880.0, L.size() - 2048, 1500) > 0.15);
        CHECK(pads.levels()[3] > 0.1f);
    }

    // 4. Poly + Choke group 1 on pads 0 & 1 cuts pad 0 smoothly when pad 1 triggers (while Loop pad 3 keeps playing)
    {
        pads.trigger(0, 1.0f);
        std::vector<float> L, R;
        renderAppend(e, int(0.03f * kSr), L, R);
        CHECK(goertzel(L, kSr, 220.0, 256, 1000) > 0.15);
        CHECK(goertzel(L, kSr, 880.0, 256, 1000) > 0.15);  // polyphonic with pad 3

        pads.trigger(1, 1.0f);
        L.clear(); R.clear();
        renderAppend(e, int(0.04f * kSr), L, R);  // > 4 ms choke fade
        CHECK(allFinite(L) && allFinite(R));
        const double tail220 = goertzel(L, kSr, 220.0, L.size() - 1024, 1024);
        const double tail440 = goertzel(L, kSr, 440.0, L.size() - 1024, 1024);
        const double tail880 = goertzel(L, kSr, 880.0, L.size() - 1024, 1024);
        INFO("choke tail: 220Hz=%.4f, 440Hz=%.4f, 880Hz=%.4f\n", tail220, tail440, tail880);
        CHECK(tail220 < 0.01);
        CHECK(tail440 > 0.15);
        CHECK(tail880 > 0.15);
        CHECK(pads.levels()[1] > 0.1f);
        CHECK(pads.levels()[3] > 0.1f);
    }

    // 5. stopAll silences all pads
    {
        pads.stopAll();
        std::vector<float> L, R;
        renderAppend(e, int(0.02f * kSr), L, R);
        CHECK(rms(L, L.size() - 256, L.size()) == 0.0);
        const auto lv = pads.levels();
        for (int i = 0; i < PadSampler::kPads; ++i) CHECK(lv[size_t(i)] == 0.0f);
        CHECK(!pads.active());
    }

    // 6. Remote "pads" op roundtrip
    RemoteControl rc(e, RemoteControl::Config{});
    CHECK(rc.handle(parseJson(std::string(R"({"op":"pads","cmd":"load","pad":4,"path":")") + p0Path + "\"}")).find("ok")->boolean());
    CHECK(rc.handle(parseJson(R"({"op":"pads","cmd":"setup","pad":4,"level":0.9,"pitchSemis":12,"mode":1,"choke":2,"velSens":1,"startFrac":0.0,"endFrac":1.0})")).find("ok")->boolean());
    Json pkReply = rc.handle(parseJson(R"({"op":"pads","cmd":"peaks","pad":4,"bins":32})"));
    CHECK(pkReply.find("ok")->boolean());
    CHECK(pkReply.find("peaks")->isArray() && pkReply.find("peaks")->size() == 64);

    CHECK(rc.handle(parseJson(R"({"op":"pads","cmd":"trigger","pad":4,"vel":0.85})")).find("ok")->boolean());
    std::vector<float> rL, rR;
    renderAppend(e, 512, rL, rR);
    Json lvReply = rc.handle(parseJson(R"({"op":"pads","cmd":"levels"})"));
    CHECK(lvReply.find("ok")->boolean());
    CHECK(lvReply.find("levels")->at(4).number() > 0.05);

    CHECK(rc.handle(parseJson(R"({"op":"pads","cmd":"stop_all"})")).find("ok")->boolean());
    renderAppend(e, int(0.02f * kSr), rL, rR);
    lvReply = rc.handle(parseJson(R"({"op":"pads","cmd":"levels"})"));
    CHECK(lvReply.find("levels")->at(4).number() == 0.0);
    CHECK(rc.handle(parseJson(R"({"op":"pads","cmd":"clear","pad":4})")).find("ok")->boolean());
}

TEST(live_sampling_capture_analyze_export_roundtrip) {
    const std::string dir = tempDir("live_sample");

    // 1. Source 0: AudioInput 220 Hz sine -> start -> stop -> analyze -> exportWav -> reload in Layer A PCM
    {
        Engine e;
        e.prepare(kSr);
        e.setParam(P::MasterVolume, 1.0f);
        e.setParam(P::Limiter, 0.0f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);

        auto& ls = e.liveSampler();
        CHECK(ls.start(0, 2.0f, -120.0f));

        constexpr int kBlock = 256;
        constexpr int kBlocks = 80;  // 20480 frames = ~0.427 s
        std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
        double phase = 0.0;
        const double inc = 2.0 * double(kPi) * 220.0 / kSr;
        for (int b = 0; b < kBlocks; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                const float v = (b < 2) ? 0.0f : 0.4f * float(std::sin(phase));
                inL[size_t(i)] = v;
                inR[size_t(i)] = v;
                phase += inc;
            }
            e.setInputBuffer(inL.data(), inR.data(), kBlock);
            e.process(outL.data(), outR.data(), kBlock);
        }
        e.setInputBuffer(nullptr, nullptr, 0);

        const double sec = ls.stop();
        CHECK(sec > 0.35);
        const auto pk = ls.peaks(64);
        CHECK(pk.size() == 128);

        const auto a = ls.analyze();
        const int startFrame = int(a[0]);
        const int endFrame = int(a[1]);
        const float gainDb = a[2];
        const int loopStart = int(a[3]);
        const int loopEnd = int(a[4]);
        const int rootMidi = int(a[5]);
        INFO("live_sample src=0 analyze: start=%d, end=%d, gainDb=%.2f, loop=[%d,%d], root=%d, sr=%.0f\n",
             startFrame, endFrame, gainDb, loopStart, loopEnd, rootMidi, a[6]);
        CHECK(startFrame >= 0 && startFrame < endFrame);
        CHECK(std::isfinite(gainDb));
        CHECK(rootMidi >= 56 && rootMidi <= 58);  // A3 = 57 (220 Hz)
        CHECK(loopStart >= startFrame && loopEnd > loopStart && loopEnd <= endFrame);

        // Auditioning outputs audio
        ls.audition(true, false, startFrame, endFrame);
        CHECK(ls.auditioning());
        std::vector<float> audL, audR;
        renderAppend(e, 4096, audL, audR);
        CHECK(allFinite(audL) && rms(audL, 512, 3500) > 0.05);
        ls.audition(false, false, 0, -1);
        CHECK(!ls.auditioning());

        // Export WAV with smpl loop points & unityNote
        const std::string wavPath = dir + "/captured_src0.wav";
        const std::string errStr = ls.exportWav(wavPath, startFrame, endFrame, gainDb, loopStart, loopEnd, 5.0f, rootMidi);
        CHECK(errStr.empty());

        WavData wd;
        std::string werr;
        CHECK(readWav(wavPath, wd, werr));
        CHECK(wd.unityNote == rootMidi);
        CHECK(wd.loopStart >= 0 && wd.loopEnd > wd.loopStart);

        // Reload exported WAV into Layer A PCM and verify playback!
        Engine ePlay;
        ePlay.prepare(kSr);
        ePlay.setParam(P::MasterVolume, 1.0f);
        ePlay.setParam(P::Limiter, 0.0f);
        ePlay.setParam(P::FilterType, (float)FilterType::Off);
        ePlay.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        ePlay.setParam(P::AmpAttack, 0.001f);
        ePlay.setParam(P::AmpSustain, 1.0f);
        ePlay.setParam(P::AmpRelease, 0.02f);
        ePlay.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
        ePlay.setParam(P::layer(0, P::LLevel), 0.8f);
        std::vector<std::string> warnings;
        std::string lerr;
        CHECK(loadLayerSamples(ePlay, 0, wavPath, lerr, warnings));
        ePlay.sendEvent(Event::noteOn(1, rootMidi, 1.0f));
        std::vector<float> pL, pR;
        renderAppend(ePlay, 16000, pL, pR);
        CHECK(allFinite(pL) && allFinite(pR));
        const double fPlay = zeroCrossFreq(pL, kSr, 2000, 12000);
        INFO("reloaded Layer A PCM freq=%.2f Hz (expected ~220 Hz)\n", fPlay);
        CHECK(rms(pL, 2000, 12000) > 0.10);
        CHECK_NEAR(fPlay, 220.0, 15.0);

        ls.discard();
        CHECK(ls.state()[0] == 0.0f);  // Idle
    }

    // 2. Source 1: InternalMaster synth note (A3 = 220 Hz)
    {
        Engine e;
        e.prepare(kSr);
        std::vector<std::string> warnings;
        Preset p = makeCleanVaPreset("ResampleSine", VaWave::Sine, 0.6f);
        CHECK(applyPreset(e, p, "", warnings));

        auto& ls = e.liveSampler();
        CHECK(ls.start(1, 2.0f, -120.0f));
        e.sendEvent(Event::noteOn(1, 57, 1.0f));  // A3 = 220 Hz
        std::vector<float> outL, outR;
        renderAppend(e, 20480, outL, outR);
        e.sendEvent(Event::noteOff(1, 57));

        const double sec = ls.stop();
        CHECK(sec > 0.35);
        const auto a = ls.analyze();
        INFO("live_sample src=1 analyze: start=%.0f, end=%.0f, gainDb=%.2f, root=%.0f\n",
             a[0], a[1], a[2], a[5]);
        CHECK(a[0] < a[1]);
        CHECK(std::isfinite(a[2]));
        CHECK(a[5] >= 56.0f && a[5] <= 58.0f);
        ls.discard();
    }

    // 3. Remote "live_sample" op roundtrip
    {
        Engine e;
        e.prepare(kSr);
        std::vector<std::string> warnings;
        CHECK(applyPreset(e, makeCleanVaPreset("RemoteResample", VaWave::Sine, 0.6f), "", warnings));
        RemoteControl rc(e, RemoteControl::Config{});

        CHECK(rc.handle(parseJson(R"({"op":"live_sample","cmd":"start","src":1,"maxSec":2.0,"armDb":-120})")).find("ok")->boolean());
        e.sendEvent(Event::noteOn(2, 57, 1.0f));
        std::vector<float> outL, outR;
        renderAppend(e, 16384, outL, outR);

        Json st = rc.handle(parseJson(R"({"op":"live_sample","cmd":"state"})"));
        CHECK(st.find("ok")->boolean());
        CHECK(st.find("state")->at(0).number() >= 1.0);  // Recording

        Json stopR = rc.handle(parseJson(R"({"op":"live_sample","cmd":"stop"})"));
        CHECK(stopR.find("ok")->boolean());
        CHECK(stopR.find("sec")->number() > 0.25);

        Json pkR = rc.handle(parseJson(R"({"op":"live_sample","cmd":"peaks","bins":32})"));
        CHECK(pkR.find("ok")->boolean() && pkR.find("peaks")->size() == 64);

        Json anR = rc.handle(parseJson(R"({"op":"live_sample","cmd":"analyze"})"));
        CHECK(anR.find("ok")->boolean() && anR.find("analysis")->size() == 7);
        const int sF = (int)anR.find("analysis")->at(0).number();
        const int eF = (int)anR.find("analysis")->at(1).number();
        CHECK(sF < eF);

        const std::string rWav = dir + "/remote_export.wav";
        Json exR = rc.handle(parseJson(
            std::string(R"({"op":"live_sample","cmd":"export","path":")") + rWav +
            R"(","startFrame":)" + std::to_string(sF) + R"(,"endFrame":)" + std::to_string(eF) +
            R"(,"gainDb":0.0,"loopStart":-1,"loopEnd":-1,"xfadeMs":0.0,"rootMidi":57})"));
        CHECK(exR.find("ok")->boolean());
        CHECK(fs::exists(utf8Path(rWav)));

        CHECK(rc.handle(parseJson(R"({"op":"live_sample","cmd":"discard"})")).find("ok")->boolean());
        st = rc.handle(parseJson(R"({"op":"live_sample","cmd":"state"})"));
        CHECK(st.find("state")->at(0).number() == 0.0);
    }
}

TEST(ext_midi_zone_master_routing_and_scene_pc) {
    static_assert(Mix::Count == 16, "Mix::Count must equal 16");

    Engine e;
    e.prepare(kSr);
    std::vector<std::string> warnings;

    MultiPreset m;
    m.name = "ZoneMaster";
    m.global = makeCleanVaPreset("ZoneGlobal");
    m.parts[0] = makeCleanVaPreset("Part0Ext", VaWave::Sine, 0.5f);
    m.parts[1] = makeCleanVaPreset("Part1Both", VaWave::Sine, 0.5f);

    // Part 0: EXT (ZoneMode=1), port 1, ch 2, bank MSB 10, LSB 20, PC 5
    m.mix[0][Mix::Channel] = 1.0f;
    m.mix[0][Mix::ZoneMode] = 1.0f;
    m.mix[0][Mix::ZoneOutPort] = 1.0f;
    m.mix[0][Mix::ZoneOutChannel] = 2.0f;
    m.mix[0][Mix::ZoneBankMsb] = 10.0f;
    m.mix[0][Mix::ZoneBankLsb] = 20.0f;
    m.mix[0][Mix::ZoneProgram] = 5.0f;

    // Part 1: BOTH (ZoneMode=2), port 1, ch 3
    m.mix[1][Mix::Channel] = 2.0f;
    m.mix[1][Mix::ZoneMode] = 2.0f;
    m.mix[1][Mix::ZoneOutPort] = 1.0f;
    m.mix[1][Mix::ZoneOutChannel] = 3.0f;

    CHECK(applyMulti(e, m, "", warnings));

    std::vector<float> L, R;
    renderAppend(e, 256, L, R);

    // 1. applyMulti emits CC0=10, CC32=20, PC=5 on slot 1 ch 2 (status nibble 1)
    auto msgs = decodeWire(e.externalMidi().next(0));
    CHECK(hasWire3(msgs, 1, 0xB1, 0, 10));
    CHECK(hasWire3(msgs, 1, 0xB1, 32, 20));
    CHECK(hasWire2(msgs, 1, 0xC1, 5));

    // 2. NoteOn on Part 0 (EXT) -> 0 internal voices, external NoteOn on port 1 ch 2 (0x91)
    Event n0 = Event::noteOn(101, 60, 0.8f);
    n0.channel = 0;
    e.sendEvent(n0);
    renderAppend(e, 256, L, R);
    CHECK(e.partActiveVoices(0) == 0);
    CHECK(e.stats().activeVoices == 0);
    msgs = decodeWire(e.externalMidi().next(0));
    CHECK(hasWireNotePrefix(msgs, 1, 0x91, 60));

    // 3. NoteOn on Part 1 (BOTH) -> 1 internal voice AND external NoteOn on port 1 ch 3 (0x92)
    Event n1 = Event::noteOn(102, 64, 0.75f);
    n1.channel = 1;
    e.sendEvent(n1);
    renderAppend(e, 256, L, R);
    CHECK(e.partActiveVoices(1) == 1);
    CHECK(e.stats().activeVoices == 1);
    msgs = decodeWire(e.externalMidi().next(0));
    CHECK(hasWireNotePrefix(msgs, 1, 0x92, 64));

    // 4. Hold a note across a split/scene change -> NoteOff STILL routes to original port 1 / ch 2 / key 60!
    MultiPreset m2 = m;
    m2.mix[0][Mix::KeyLow] = 80.0f;         // key 60 is now outside Part 0's split
    m2.mix[0][Mix::ZoneMode] = 0.0f;        // and Part 0 switched to INT
    m2.mix[0][Mix::ZoneOutPort] = 2.0f;     // and moved port/channel/transpose
    m2.mix[0][Mix::ZoneOutChannel] = 9.0f;
    m2.mix[0][Mix::Transpose] = 12.0f;
    CHECK(applyMulti(e, m2, "", warnings));
    renderAppend(e, 256, L, R);
    (void)e.externalMidi().next(0);  // drain any scene change messages

    Event n0Off = Event::noteOff(101, 60);
    n0Off.channel = 0;
    e.sendEvent(n0Off);
    renderAppend(e, 256, L, R);
    msgs = decodeWire(e.externalMidi().next(0));
    CHECK(hasWireNotePrefix(msgs, 1, 0x81, 60));  // exact original slot 1, ch 2 (0x81), key 60!

    // 5. extMidiAllOff (requestPanic on slot 1) clears all held external notes
    // Hold fresh notes on Part 0 (EXT) and Part 1 (BOTH)
    CHECK(applyMulti(e, m, "", warnings));
    renderAppend(e, 256, L, R);
    (void)e.externalMidi().next(0);

    Event hold0 = Event::noteOn(201, 62, 0.8f);
    hold0.channel = 0;
    Event hold1 = Event::noteOn(202, 67, 0.8f);
    hold1.channel = 1;
    e.sendEvent(hold0);
    e.sendEvent(hold1);
    renderAppend(e, 256, L, R);
    msgs = decodeWire(e.externalMidi().next(0));
    CHECK(hasWireNotePrefix(msgs, 1, 0x91, 62));
    CHECK(hasWireNotePrefix(msgs, 1, 0x92, 67));

    e.externalMidi().requestPanic(1);
    renderAppend(e, 256, L, R);
    msgs = decodeWire(e.externalMidi().next(0));
    CHECK(hasWireNotePrefix(msgs, 1, 0x81, 62));
    CHECK(hasWireNotePrefix(msgs, 1, 0x82, 67));
    CHECK(hasWire3(msgs, 1, 0xB1, 123, 0));
    CHECK(hasWire3(msgs, 1, 0xB2, 123, 0));

    // Subsequent NoteOffs must not emit duplicate NoteOffs after panic cleared held notes
    Event hold0Off = Event::noteOff(201, 62);
    hold0Off.channel = 0;
    e.sendEvent(hold0Off);
    renderAppend(e, 256, L, R);
    CHECK(e.externalMidi().next(0).empty());

    // 6. multiToJson / multiFromJson roundtrip with Mix::Count == 16 & legacy 8-key multi compatibility
    const std::string jsonStr = multiToJson(m);
    MultiPreset parsed;
    std::string err;
    CHECK(multiFromJson(jsonStr, parsed, err));
    CHECK(parsed.warnings.empty());
    CHECK_NEAR(parsed.mix[0][Mix::ZoneMode], 1.0f, 1e-5f);
    CHECK_NEAR(parsed.mix[0][Mix::ZoneOutPort], 1.0f, 1e-5f);
    CHECK_NEAR(parsed.mix[0][Mix::ZoneOutChannel], 2.0f, 1e-5f);
    CHECK_NEAR(parsed.mix[0][Mix::ZoneBankMsb], 10.0f, 1e-5f);
    CHECK_NEAR(parsed.mix[0][Mix::ZoneBankLsb], 20.0f, 1e-5f);
    CHECK_NEAR(parsed.mix[0][Mix::ZoneProgram], 5.0f, 1e-5f);
    CHECK_NEAR(parsed.mix[1][Mix::ZoneMode], 2.0f, 1e-5f);
    CHECK_NEAR(parsed.mix[1][Mix::ZoneOutChannel], 3.0f, 1e-5f);

    const char* legacyMulti = R"({
        "format": "hsynth-multi",
        "version": 1,
        "name": "Legacy8Mix",
        "selected": 1,
        "global": { "params": { "master.volume": 0.7 } },
        "parts": [
            {
                "mix": {
                    "volume": 0.8, "pan": -0.25, "mute": false, "channel": 1,
                    "key.low": 24, "key.high": 96, "transpose": 0, "fx.send": 0.5
                },
                "preset": { "format": "hsynth-preset", "version": 1, "name": "P1", "params": { "a.type": "va" } }
            }
        ]
    })";
    MultiPreset legacyOut;
    CHECK(multiFromJson(legacyMulti, legacyOut, err));
    CHECK(legacyOut.warnings.empty());
    CHECK_NEAR(legacyOut.mix[0][Mix::Volume], 0.8f, 1e-5f);
    CHECK_NEAR(legacyOut.mix[0][Mix::ZoneMode], 0.0f, 1e-5f);
    CHECK_NEAR(legacyOut.mix[0][Mix::ZoneOutPort], 0.0f, 1e-5f);
    CHECK_NEAR(legacyOut.mix[0][Mix::ZoneBankMsb], -1.0f, 1e-5f);
    CHECK_NEAR(legacyOut.mix[0][Mix::ZoneBankLsb], -1.0f, 1e-5f);
    CHECK_NEAR(legacyOut.mix[0][Mix::ZoneProgram], -1.0f, 1e-5f);
}
