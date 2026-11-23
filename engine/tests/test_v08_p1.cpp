// Phase 1 (v0.8): SST (Seamless Sound Switching), multi split crossfade,
// MasterStage EQ & Compressor + preset isolation, phrase quantize / SMF
// export, and Scala (.scl) microtonal tuning + Remote ops.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "analysis.h"
#include "hsynth/dsp/master_stage.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/preset.h"
#include "hsynth/remote.h"
#include "hsynth/smf.h"
#include "hsynth/tuning.h"
#include "hsynth/wav.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;
namespace fs = std::filesystem;

namespace {

constexpr float kSr = 48000.0f;

std::string tempDirP1(const char* name) {
    fs::path p = fs::temp_directory_path() / "hsynth_tests" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p.generic_string();
}

void writeSineLoopSfz(const std::string& dir, const std::string& stem, float freqHz, int keycenter = 60) {
    std::vector<float> wav((size_t)kSr);
    for (size_t i = 0; i < wav.size(); ++i) {
        wav[i] = 0.4f * std::sin(2.0f * kPi * freqHz * (float)i / kSr);
    }
    std::string err;
    CHECK(writeWav(dir + "/" + stem + ".wav", wav, nullptr, (int)kSr, false, err));
    std::ofstream sfz(dir + "/" + stem + ".sfz");
    sfz << "<region> sample=" << stem << ".wav pitch_keycenter=" << keycenter
        << " lokey=0 hikey=127 loop_mode=loop_continuous loop_start=0 loop_end=" << (wav.size() - 1) << "\n";
}

Preset makeCleanSinePreset(const std::string& name, float coarseSemis, float vaLevel = 0.7f) {
    Preset p;
    p.name = name;
    p.params[(size_t)P::FilterType] = (float)FilterType::Off;
    p.params[(size_t)P::AmpAttack] = 0.002f;
    p.params[(size_t)P::AmpSustain] = 1.0f;
    p.params[(size_t)P::AmpRelease] = 0.02f;
    p.params[(size_t)P::Limiter] = 0.0f;
    p.params[(size_t)P::fxp(fx::Reverb, fx::reverb::On)] = 0.0f;
    p.params[(size_t)P::mod(0, P::ModAmt)] = 0.0f;
    p.params[(size_t)P::mod(0, P::ModVia)] = 0.0f;
    p.params[(size_t)P::layer(0, P::LType)] = (float)LayerType::VA;
    p.params[(size_t)P::layer(0, P::VaWave1)] = (float)VaWave::Sine;
    p.params[(size_t)P::layer(0, P::VaMix)] = 0.0f;
    p.params[(size_t)P::layer(0, P::LCoarse)] = coarseSemis;
    p.params[(size_t)P::layer(0, P::LLevel)] = vaLevel;
    return p;
}

void renderAppend(Engine& e, int frames, std::vector<float>& outL, std::vector<float>& outR, int block = 128) {
    std::vector<float> bl((size_t)block), br((size_t)block);
    for (int done = 0; done < frames; done += block) {
        const int n = std::min(block, frames - done);
        e.process(bl.data(), br.data(), n);
        outL.insert(outL.end(), bl.begin(), bl.begin() + n);
        outR.insert(outR.end(), br.begin(), br.begin() + n);
        e.collectGarbage();
    }
}

Json parseJson(const std::string& s) {
    Json j;
    std::string err;
    CHECK(Json::parse(s, j, err));
    return j;
}

}  // namespace

TEST(sst_live_preset_and_multi_ghost_tail) {
    const std::string dir = tempDirP1("v08_sst");
    // s1: 350 Hz at key 60; s2: 910 Hz at key 60 (well separated from VA notes)
    writeSineLoopSfz(dir, "s1", 350.0f, 60);
    writeSineLoopSfz(dir, "s2", 910.0f, 60);

    // Preset A: VA Sine (coarse=0 -> key 60 = 261.63 Hz, key 67 = 392.00 Hz) + PCM s1 (350 Hz @ key 60)
    // + Delay + Reverb + Send A + Send B active.
    Preset presetA = makeCleanSinePreset("Patch A", 0.0f, 0.5f);
    presetA.params[(size_t)P::layer(1, P::LType)] = (float)LayerType::PCM;
    presetA.params[(size_t)P::layer(1, P::LLevel)] = 0.5f;
    presetA.samples[1] = "s1.sfz";
    presetA.params[(size_t)P::fxp(fx::Delay, fx::delay::On)] = 1.0f;
    presetA.params[(size_t)P::fxp(fx::Delay, fx::delay::Sync)] = 0.0f;
    presetA.params[(size_t)P::fxp(fx::Delay, fx::delay::Time)] = 0.12f;
    presetA.params[(size_t)P::fxp(fx::Delay, fx::delay::Feedback)] = 0.45f;
    presetA.params[(size_t)P::fxp(fx::Delay, fx::delay::Mix)] = 0.35f;
    presetA.params[(size_t)P::fxp(fx::Reverb, fx::reverb::On)] = 1.0f;
    presetA.params[(size_t)P::fxp(fx::Reverb, fx::reverb::Decay)] = 1.8f;
    presetA.params[(size_t)P::fxp(fx::Reverb, fx::reverb::Mix)] = 0.35f;
    presetA.params[(size_t)P::SendA] = 0.5f;
    presetA.params[(size_t)P::SendB] = 0.5f;
    presetA.params[(size_t)P::fxChain(P::SendALevel)] = 0.6f;
    presetA.params[(size_t)P::fxChain(P::SendADecay)] = 1.8f;
    presetA.params[(size_t)P::fxChain(P::SendBLevel)] = 0.6f;
    presetA.params[(size_t)P::fxChain(P::SendBSync)] = 0.0f;
    presetA.params[(size_t)P::fxChain(P::SendBTime)] = 0.14f;
    presetA.params[(size_t)P::fxChain(P::SendBFeedback)] = 0.45f;

    // Preset B: VA Sine (+15 st -> key 60 = 622.25 Hz) + PCM s2 (910 Hz @ key 60), all FX/sends off.
    Preset presetB = makeCleanSinePreset("Patch B", 15.0f, 0.5f);
    presetB.params[(size_t)P::layer(1, P::LType)] = (float)LayerType::PCM;
    presetB.params[(size_t)P::layer(1, P::LLevel)] = 0.5f;
    presetB.samples[1] = "s2.sfz";
    presetB.params[(size_t)P::SendA] = 0.0f;
    presetB.params[(size_t)P::SendB] = 0.0f;
    presetB.params[(size_t)P::fxp(fx::Delay, fx::delay::On)] = 0.0f;
    presetB.params[(size_t)P::fxp(fx::Reverb, fx::reverb::On)] = 0.0f;

    {
        Engine e;
        e.prepare(kSr);
        e.setSstEnabled(true);
        e.setSstTimeMs(150.0f);
        CHECK(e.sstEnabled());
        CHECK_NEAR(e.sstTimeMs(), 150.0f, 1e-3f);

        std::vector<std::string> warnings;
        CHECK(applyPreset(e, presetA, dir, warnings));

        // Hold sustained chord on Preset A (keys 60 and 64: VA 261.63/329.63 Hz, PCM s1 350.00/440.97 Hz)
        e.sendEvent(Event::noteOn(1, 60, 0.85f));
        e.sendEvent(Event::noteOn(2, 64, 0.85f));
        std::vector<float> L, R;
        renderAppend(e, (int)(0.25f * kSr), L, R);
        const size_t switchSample = L.size();

        // Trigger SST switch to Preset B while chord is still held
        uint32_t serial = 0;
        CHECK(applyPresetSst(e, presetB, dir, warnings, &serial, false));
        renderAppend(e, 128, L, R, 128);
        CHECK(e.liveChangeApplied(serial));
        CHECK(e.sstProgress() >= 0.0f && e.sstProgress() < 1.0f);

        // Hold old chord across the transition + beyond SST window
        renderAppend(e, (int)(0.35f * kSr), L, R);
        CHECK(e.sstProgress() < 0.0f);  // transition finished

        // Continuity check across switchSample: no click or cut
        double steadyDiff = 0.0, boundaryDiff = 0.0;
        for (size_t i = switchSample - 2000; i < switchSample - 10; ++i)
            steadyDiff = std::max(steadyDiff, (double)std::fabs(L[i] - 2.0f * L[i - 1] + L[i - 2]));
        for (size_t i = switchSample; i < switchSample + 640; ++i)
            boundaryDiff = std::max(boundaryDiff, (double)std::fabs(L[i] - 2.0f * L[i - 1] + L[i - 2]));
        INFO("SST switch boundary 2nd diff: steady %.5f, switch %.5f\n", steadyDiff, boundaryDiff);
        CHECK(boundaryDiff < 0.05);

        // Old held chord must still sound on old VA (261.63 Hz) + old PCM (350 Hz) and NOT new patch (622.25 / 910 Hz)
        const size_t postStart = switchSample + (size_t)(0.16f * kSr);
        const size_t postLen = (size_t)(0.18f * kSr);
        const double oldVa = goertzel(L, kSr, 261.63, postStart, postLen);
        const double oldPcm = goertzel(L, kSr, 350.0, postStart, postLen);
        const double newVa = goertzel(L, kSr, 622.25, postStart, postLen);
        const double newPcm = goertzel(L, kSr, 910.0, postStart, postLen);
        INFO("during held ghost: oldVa=%.4f oldPcm=%.4f newVa=%.4f newPcm=%.4f\n", oldVa, oldPcm, newVa, newPcm);
        CHECK(oldVa > 0.03);
        CHECK(oldPcm > 0.03);
        CHECK(newPcm < 0.005);
        CHECK(newVa < 0.005);

        // Play a NEW note (key 60) while old chord is still held -> triggers Preset B (622.25 Hz + 910 Hz)!
        e.sendEvent(Event::noteOn(3, 60, 0.85f));
        const size_t newNoteStart = L.size();
        renderAppend(e, (int)(0.25f * kSr), L, R);
        const double triggeredNewVa = goertzel(L, kSr, 622.25, newNoteStart + 2400, 8192);
        const double triggeredNewPcm = goertzel(L, kSr, 910.0, newNoteStart + 2400, 8192);
        INFO("new note after SST: newVa=%.4f newPcm=%.4f\n", triggeredNewVa, triggeredNewPcm);
        CHECK(triggeredNewVa > 0.03);
        CHECK(triggeredNewPcm > 0.03);

        // Release ONLY the new note first -> old held notes STILL sound!
        e.sendEvent(Event::noteOff(3, 60));
        renderAppend(e, (int)(0.10f * kSr), L, R);
        CHECK(goertzel(L, kSr, 350.0, L.size() - 4096, 4096) > 0.02);

        // Now release old held chord -> Delay/Reverb/Send tails from oldFX must continue smoothly!
        e.sendEvent(Event::noteOff(1, 60));
        e.sendEvent(Event::noteOff(2, 64));
        const size_t releaseSample = L.size();
        renderAppend(e, (int)(0.60f * kSr), L, R);
        const double tailRms = rms(L, releaseSample + (size_t)(0.15f * kSr), releaseSample + (size_t)(0.45f * kSr));
        INFO("outgoing FX/send tail rms after chord release: %.5f\n", tailRms);
        CHECK(tailRms > 0.002);

        // Let voices & tails finish completely and reclaim ghosts/held sets
        renderAppend(e, (int)(3.5f * kSr), L, R);
        e.collectGarbage();
        CHECK(e.stats().activeVoices == 0);
        CHECK(allFinite(L) && allFinite(R));
    }

    // Multi SST + single->multi->single + rapid consecutive SST switches
    {
        Engine e;
        e.prepare(kSr);
        e.setSstEnabled(true);
        e.setSstTimeMs(80.0f);

        MultiPreset m1;
        m1.name = "Multi 1";
        m1.parts[0] = makeCleanSinePreset("M1 P1", 0.0f, 0.5f);
        m1.parts[0].params[(size_t)P::layer(1, P::LType)] = (float)LayerType::PCM;
        m1.parts[0].params[(size_t)P::layer(1, P::LLevel)] = 0.5f;
        m1.parts[0].samples[1] = "s1.sfz";
        m1.parts[1] = makeCleanSinePreset("M1 P2", 0.0f, 0.5f);
        m1.mix[0][Mix::Channel] = 1.0f;
        m1.mix[1][Mix::Channel] = 2.0f;
        m1.global.params[(size_t)P::Limiter] = 0.0f;
        m1.global.params[(size_t)P::fxp(fx::Delay, fx::delay::On)] = 1.0f;
        m1.global.params[(size_t)P::fxp(fx::Delay, fx::delay::Mix)] = 0.3f;
        m1.global.params[(size_t)P::fxp(fx::Reverb, fx::reverb::On)] = 0.0f;

        MultiPreset m2 = m1;
        m2.name = "Multi 2";
        m2.parts[0] = makeCleanSinePreset("M2 P1", 12.0f, 0.5f);
        m2.parts[0].params[(size_t)P::layer(1, P::LType)] = (float)LayerType::PCM;
        m2.parts[0].params[(size_t)P::layer(1, P::LLevel)] = 0.5f;
        m2.parts[0].samples[1] = "s2.sfz";
        m2.parts[1] = makeCleanSinePreset("M2 P2", 7.0f, 0.5f);

        std::vector<std::string> warnings;
        uint32_t serial = 0;
        CHECK(applyMultiSst(e, m1, dir, warnings, &serial));
        std::vector<float> L, R;
        renderAppend(e, 256, L, R);
        CHECK(e.liveChangeApplied(serial));
        CHECK(e.multiTimbral());

        Event p0On = Event::noteOn(10, 60, 0.8f);
        p0On.channel = 0;
        Event p1On = Event::noteOn(11, 69, 0.8f);
        p1On.channel = 1;
        e.sendEvent(p0On);
        e.sendEvent(p1On);
        renderAppend(e, (int)(0.15f * kSr), L, R);

        // Switch Multi 1 -> Multi 2 via SST while notes on both parts are held
        CHECK(applyMultiSst(e, m2, dir, warnings, &serial));
        renderAppend(e, 256, L, R);
        CHECK(e.liveChangeApplied(serial));
        renderAppend(e, (int)(0.15f * kSr), L, R);
        // Old Multi 1 notes still sound (350 Hz on part 0 PCM, 440 Hz on part 1 VA)
        CHECK(goertzel(L, kSr, 350.0, L.size() - 4096, 4096) > 0.02);
        CHECK(goertzel(L, kSr, 440.0, L.size() - 4096, 4096) > 0.02);

        // Play new note on part 0 ch 1 -> hears M2 P1 (910 Hz PCM)
        Event p0New = Event::noteOn(12, 60, 0.8f);
        p0New.channel = 0;
        e.sendEvent(p0New);
        renderAppend(e, (int)(0.15f * kSr), L, R);
        CHECK(goertzel(L, kSr, 910.0, L.size() - 4096, 4096) > 0.02);

        // Rapid consecutive SST switches creating multiple ghost cohorts
        for (int s = 0; s < 5; ++s) {
            Event on = Event::noteOn((uint32_t)(20 + s), 55 + s, 0.6f);
            on.channel = 0;
            e.sendEvent(on);
            Preset step = makeCleanSinePreset("Step", (float)(s + 1), 0.4f);
            uint32_t sSerial = 0;
            CHECK(applyPartPresetSst(e, 0, step, dir, warnings, &sSerial));
            renderAppend(e, 256, L, R);
            CHECK(e.liveChangeApplied(sSerial));
        }
        // Release all notes and switch back to single mode via SST
        e.sendEvent(Event::control(EventType::AllNotesOff, 0));
        for (int ch = 0; ch < 2; ++ch) {
            Event off = Event::control(EventType::AllNotesOff, 0);
            off.channel = (uint8_t)ch;
            e.sendEvent(off);
        }
        CHECK(applyPresetSst(e, presetB, dir, warnings, &serial, true));
        renderAppend(e, (int)(0.8f * kSr), L, R);
        CHECK(!e.multiTimbral());
        CHECK(e.stats().activeVoices == 0);
        CHECK(allFinite(L) && allFinite(R) && peak(L) < 4.0 && peak(R) < 4.0);
    }

    fs::remove_all(dir);
}

TEST(multi_split_crossfade_equal_power) {
    // 1. Equal-power crossfade across boundary with matched fade width (12 semitones)
    auto measurePair = [](float fadeWidth, int key, double& g1, double& g2) {
        Engine e;
        e.prepare(kSr);
        e.setMultiTimbral(true);
        e.setParam(P::Limiter, 0.0f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        for (int k = 0; k < 2; ++k) {
            e.setPartParam(k, P::layer(0, P::LType), (float)LayerType::VA);
            e.setPartParam(k, P::layer(0, P::VaWave1), (float)VaWave::Sine);
            e.setPartParam(k, P::layer(0, P::VaMix), 0.0f);
            e.setPartParam(k, P::FilterType, (float)FilterType::Off);
            e.setPartParam(k, P::AmpAttack, 0.001f);
            e.setPartParam(k, P::AmpSustain, 1.0f);
            e.setPartParam(k, P::mod(0, P::ModAmt), 0.0f);
            e.setPartMix(k, Mix::Channel, 1.0f);
        }
        // Zone 1: C1 (24) .. B3 (59), pan hard left so L measures g1
        e.setPartMix(0, Mix::KeyLow, 24.0f);
        e.setPartMix(0, Mix::KeyHigh, 59.0f);
        e.setPartMix(0, Mix::KeyFadeLow, 0.0f);
        e.setPartMix(0, Mix::KeyFadeHigh, fadeWidth);
        e.setPartMix(0, Mix::Pan, -1.0f);
        // Zone 2: C4 (60) .. C7 (96), pan hard right so R measures g2
        e.setPartMix(1, Mix::KeyLow, 60.0f);
        e.setPartMix(1, Mix::KeyHigh, 96.0f);
        e.setPartMix(1, Mix::KeyFadeLow, fadeWidth);
        e.setPartMix(1, Mix::KeyFadeHigh, 0.0f);
        e.setPartMix(1, Mix::Pan, 1.0f);

        Event on = Event::noteOn(1, key, 1.0f);
        on.channel = 0;
        e.sendEvent(on);
        std::vector<float> L, R;
        renderAppend(e, (int)(0.15f * kSr), L, R);
        g1 = rms(L, 2400, L.size());
        g2 = rms(R, 2400, R.size());
        return std::pair<int, int>{e.partActiveVoices(0), e.partActiveVoices(1)};
    };

    double ref1 = 0.0, zeroR = 0.0;
    measurePair(12.0f, 48, ref1, zeroR);  // deep inside Zone 1 -> full unity g1, g2 == 0
    CHECK(ref1 > 0.05);
    CHECK(zeroR < 1e-7);

    double prevG1 = 1.01, prevG2 = -0.01;
    for (int key : {53, 55, 57, 59, 60, 62, 64, 66}) {
        double g1 = 0.0, g2 = 0.0;
        measurePair(12.0f, key, g1, g2);
        const double n1 = g1 / ref1, n2 = g2 / ref1;
        const double pwrSum = n1 * n1 + n2 * n2;
        INFO("key %d: g1=%.4f g2=%.4f (g1^2+g2^2=%.4f)\n", key, n1, n2, pwrSum);
        CHECK_NEAR(pwrSum, 1.0, 0.02);
        CHECK(n1 <= prevG1 + 0.005);
        CHECK(n2 >= prevG2 - 0.005);
        prevG1 = n1;
        prevG2 = n2;
    }

    // 2. fade == 0 exact hard cut
    double l59 = 0.0, r59 = 0.0, l60 = 0.0, r60 = 0.0;
    const auto v59 = measurePair(0.0f, 59, l59, r59);
    const auto v60 = measurePair(0.0f, 60, l60, r60);
    CHECK(v59.first == 1 && v59.second == 0);
    CHECK_NEAR(l59 / ref1, 1.0, 0.01);
    CHECK(r59 < 1e-7);
    CHECK(v60.first == 0 && v60.second == 1);
    CHECK(l60 < 1e-7);
    CHECK_NEAR(r60 / ref1, 1.0, 0.01);

    // 3. Multi JSON roundtrip for KeyFadeLow = 8 and KeyFadeHigh = 9
    MultiPreset mp;
    mp.name = "Split Crossfade";
    mp.mix[0][Mix::KeyLow] = 24.0f;
    mp.mix[0][Mix::KeyHigh] = 59.0f;
    mp.mix[0][Mix::KeyFadeLow] = 8.0f;
    mp.mix[0][Mix::KeyFadeHigh] = 9.0f;
    mp.mix[1][Mix::KeyLow] = 60.0f;
    mp.mix[1][Mix::KeyHigh] = 96.0f;
    mp.mix[1][Mix::KeyFadeLow] = 12.0f;
    mp.mix[1][Mix::KeyFadeHigh] = 6.0f;
    const std::string json = multiToJson(mp);
    CHECK(json.find("\"key.fade.low\"") != std::string::npos);
    CHECK(json.find("\"key.fade.high\"") != std::string::npos);
    MultiPreset back;
    std::string err;
    CHECK(multiFromJson(json, back, err));
    CHECK(back.warnings.empty());
    CHECK_NEAR(back.mix[0][Mix::KeyFadeLow], 8.0f, 1e-5f);
    CHECK_NEAR(back.mix[0][Mix::KeyFadeHigh], 9.0f, 1e-5f);
    CHECK_NEAR(back.mix[1][Mix::KeyFadeLow], 12.0f, 1e-5f);
    CHECK_NEAR(back.mix[1][Mix::KeyFadeHigh], 6.0f, 1e-5f);
}

TEST(master_stage_eq_comp_and_preset_isolation) {
    const std::string dir = tempDirP1("v08_master");
    // Helper to render a steady 987.77 Hz (key 83) sine tone on a clean engine
    auto setupClean = [](Engine& e) {
        e.setParam(P::FilterType, (float)FilterType::Off);
        e.setParam(P::layer(0, P::LType), (float)LayerType::VA);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        e.setParam(P::AmpAttack, 0.001f);
        e.setParam(P::AmpSustain, 1.0f);
        e.setParam(P::Limiter, 0.0f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        e.setParam(P::mod(0, P::ModAmt), 0.0f);
        e.setParam(P::MasterVolume, 0.8f);
    };
    auto renderTone = [](Engine& e, int key = 83, float vel = 0.5f) {
        e.sendEvent(Event::noteOn(1, key, vel));
        std::vector<float> L, R;
        renderAppend(e, (int)(0.25f * kSr), L, R);
        e.sendEvent(Event::noteOff(1, key));
        renderAppend(e, (int)(0.05f * kSr), L, R);
        return L;
    };

    // 1. Bypass == true is bit-identical no-op even with EQ/Comp configured
    std::vector<float> baseTone, bypassTone;
    {
        Engine e;
        e.prepare(kSr);
        setupClean(e);
        baseTone = renderTone(e);
    }
    {
        Engine e;
        e.prepare(kSr);
        setupClean(e);
        MasterStageConfig cfg;
        cfg.bypass = true;
        cfg.eqEnabled = true;
        cfg.compEnabled = true;
        cfg.bands[2].freq = 1000.0f;
        cfg.bands[2].gainDb = 12.0f;
        cfg.bands[2].q = 1.0f;
        cfg.bands[2].enabled = true;
        cfg.comp.thresholdDb = -36.0f;
        cfg.comp.ratio = 8.0f;
        e.setMasterStage(cfg);
        bypassTone = renderTone(e);
    }
    double maxDiff = 0.0;
    for (size_t i = 0; i < baseTone.size(); ++i)
        maxDiff = std::max(maxDiff, (double)std::fabs(baseTone[i] - bypassTone[i]));
    INFO("MasterStage bypass=true max diff vs default: %g\n", maxDiff);
    CHECK(maxDiff == 0.0);

    // 2. 5-band Master EQ (+6 dB @ 1 kHz) & Master Comp -> Goertzel delta + non-positive GR
    Engine e;
    e.prepare(kSr);
    setupClean(e);
    MasterStageConfig eqCfg;
    eqCfg.bypass = false;
    eqCfg.eqEnabled = true;
    eqCfg.compEnabled = false;
    eqCfg.bands[2].freq = 1000.0f;
    eqCfg.bands[2].gainDb = 6.0f;
    eqCfg.bands[2].q = 1.0f;
    eqCfg.bands[2].enabled = true;
    e.setMasterStage(eqCfg);

    const auto eqTone = renderTone(e, 83, 0.4f);  // key 83 = 987.77 Hz (~1 kHz)
    const double baseAmp = goertzel(baseTone, kSr, 987.77, 4800, 4800);
    // Note baseTone used vel=0.5f, eqTone used vel=0.4f -> re-render base at 0.4f for exact delta
    std::vector<float> base04;
    {
        Engine eb;
        eb.prepare(kSr);
        setupClean(eb);
        base04 = renderTone(eb, 83, 0.4f);
    }
    const double refAmp = goertzel(base04, kSr, 987.77, 4800, 4800);
    const double eqAmp = goertzel(eqTone, kSr, 987.77, 4800, 4800);
    const double deltaDb = 20.0 * std::log10(eqAmp / std::max(1e-12, refAmp));
    INFO("Master EQ +6 dB @ 1 kHz: ref=%.4f eq=%.4f delta=%.2f dB\n", refAmp, eqAmp, deltaDb);
    CHECK(deltaDb > 5.0 && deltaDb < 6.5);
    CHECK(e.masterCompGrDb() <= 0.0f);

    // Enable Compressor too
    MasterStageConfig fullCfg = eqCfg;
    fullCfg.compEnabled = true;
    fullCfg.comp.thresholdDb = -24.0f;
    fullCfg.comp.ratio = 4.0f;
    fullCfg.comp.attackMs = 1.0f;
    fullCfg.comp.releaseMs = 100.0f;
    fullCfg.comp.kneeDb = 0.0f;
    fullCfg.comp.makeupDb = 0.0f;
    e.setMasterStage(fullCfg);
    {
        e.sendEvent(Event::noteOn(2, 83, 1.0f));
        std::vector<float> cl, cr;
        renderAppend(e, (int)(0.15f * kSr), cl, cr);
        INFO("Master Comp GR while active: %.2f dB\n", e.masterCompGrDb());
        CHECK(e.masterCompGrDb() < -0.5f);
        e.sendEvent(Event::noteOff(2, 83));
        renderAppend(e, (int)(0.3f * kSr), cl, cr);
    }

    // 3. Preset / Multi / SST isolation: MasterStage config & response stay 100% unchanged!
    auto checkMasterCfgEq = [&](const MasterStageConfig& got) {
        CHECK(!got.bypass);
        CHECK(got.eqEnabled);
        CHECK(got.compEnabled);
        CHECK_NEAR(got.bands[2].freq, 1000.0f, 1e-4f);
        CHECK_NEAR(got.bands[2].gainDb, 6.0f, 1e-4f);
        CHECK(got.bands[2].enabled);
        CHECK_NEAR(got.comp.thresholdDb, -24.0f, 1e-4f);
        CHECK_NEAR(got.comp.ratio, 4.0f, 1e-4f);
    };
    Preset pOther = makeCleanSinePreset("Other", 5.0f, 0.5f);
    Preset pClean = makeCleanSinePreset("Clean", 0.0f, 0.8f);
    pClean.params[(size_t)P::MasterVolume] = 0.8f;
    MultiPreset mp;
    mp.parts[0] = pClean;
    mp.global.params[(size_t)P::Limiter] = 0.0f;
    mp.global.params[(size_t)P::fxp(fx::Reverb, fx::reverb::On)] = 0.0f;
    mp.global.params[(size_t)P::MasterVolume] = 0.8f;

    std::vector<std::string> warnings;
    uint32_t serial = 0;
    std::vector<float> tmpL, tmpR;
    CHECK(applyPreset(e, pOther, dir, warnings));
    checkMasterCfgEq(e.getMasterStage());
    CHECK(applyPartPreset(e, 1, pOther, dir, warnings));
    checkMasterCfgEq(e.getMasterStage());
    CHECK(applyMulti(e, mp, dir, warnings));
    checkMasterCfgEq(e.getMasterStage());
    CHECK(applyMultiSst(e, mp, dir, warnings, &serial));
    renderAppend(e, 512, tmpL, tmpR);
    CHECK(e.liveChangeApplied(serial));
    checkMasterCfgEq(e.getMasterStage());
    CHECK(applyPresetSst(e, pClean, dir, warnings, &serial, true));
    renderAppend(e, (int)(0.1f * kSr), tmpL, tmpR);
    CHECK(e.liveChangeApplied(serial));
    checkMasterCfgEq(e.getMasterStage());

    // Disable comp (leaving EQ +6 dB) to verify output EQ response remained identical after all preset/multi/SST ops
    e.setMasterStage(eqCfg);
    const auto postEqTone = renderTone(e, 83, 0.4f);
    const double postEqAmp = goertzel(postEqTone, kSr, 987.77, 4800, 4800);
    const double postDeltaDb = 20.0 * std::log10(postEqAmp / std::max(1e-12, refAmp));
    INFO("Master EQ delta after all preset/multi/SST switches: %.2f dB (before: %.2f dB)\n", postDeltaDb, deltaDb);
    CHECK_NEAR(postDeltaDb, deltaDb, 0.05);

    // 4. Remote master_stage_get / master_stage_set roundtrip
    RemoteControl rc(e, RemoteControl::Config{});
    Json setReq = parseJson(R"({
        "op":"master_stage_set",
        "config":{
            "bypass":false,
            "eqEnabled":true,
            "compEnabled":true,
            "bands":[
                {"freq":80,"gainDb":2.5,"q":0.7,"enabled":true},
                {"freq":250,"gainDb":0.0,"q":0.7,"enabled":false},
                {"freq":1200,"gainDb":-3.5,"q":1.4,"enabled":true},
                {"freq":4000,"gainDb":1.5,"q":0.9,"enabled":true},
                {"freq":12000,"gainDb":0.0,"q":0.7,"enabled":false}
            ],
            "comp":{"thresholdDb":-15.0,"ratio":3.5,"attackMs":5.0,"releaseMs":120.0,"kneeDb":4.0,"makeupDb":2.0}
        }
    })");
    Json setReply = rc.handle(setReq);
    CHECK(setReply.find("ok") && setReply.find("ok")->boolean());
    Json getReply = rc.handle(parseJson(R"({"op":"master_stage_get"})"));
    CHECK(getReply.find("ok") && getReply.find("ok")->boolean());
    const Json* gotCfg = getReply.find("config");
    CHECK(gotCfg != nullptr && !gotCfg->find("bypass")->boolean());
    CHECK(gotCfg->find("eqEnabled")->boolean() && gotCfg->find("compEnabled")->boolean());
    CHECK_NEAR(gotCfg->find("bands")->at(2).find("freq")->number(), 1200.0, 1e-3);
    CHECK_NEAR(gotCfg->find("bands")->at(2).find("gainDb")->number(), -3.5, 1e-3);
    CHECK_NEAR(gotCfg->find("comp")->find("thresholdDb")->number(), -15.0, 1e-3);
    CHECK_NEAR(gotCfg->find("comp")->find("ratio")->number(), 3.5, 1e-3);
    CHECK(getReply.find("grDb") && getReply.find("grDb")->number() <= 0.0);

    const auto engineCfg = e.getMasterStage();
    CHECK_NEAR(engineCfg.bands[0].gainDb, 2.5f, 1e-4f);
    CHECK_NEAR(engineCfg.bands[2].freq, 1200.0f, 1e-4f);
    CHECK_NEAR(engineCfg.bands[2].gainDb, -3.5f, 1e-4f);
    CHECK_NEAR(engineCfg.comp.makeupDb, 2.0f, 1e-4f);

    fs::remove_all(dir);
}

TEST(phrase_quantize_smf_and_scala_tuning) {
    const std::string dir = tempDirP1("v08_phrase_scala");

    // 1. Phrase record / play / quantize / SMF export roundtrip
    {
        Engine e;
        e.prepare(kSr);
        e.setParam(P::fxChain(P::FxTempo), 120.0f);  // 2 beats/sec -> 1 beat = 0.5 s, 1 bar = 2.0 s
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        e.setPhraseBars(1);

        std::vector<float> L, R;
        e.phraseRecord();
        renderAppend(e, (int)(0.115f * kSr), L, R, 480);  // ~0.23 beats (near 0.25)
        e.sendEvent(Event::noteOn(1, 60, 0.8f));
        renderAppend(e, (int)(0.34f * kSr), L, R, 480);   // duration ~0.68 beats
        e.sendEvent(Event::noteOff(1, 60));

        renderAppend(e, (int)(0.18f * kSr), L, R, 480);   // ~1.27 beats (near 1.25)
        e.sendEvent(Event::noteOn(2, 64, 0.7f));
        renderAppend(e, (int)(0.30f * kSr), L, R, 480);   // duration ~0.60 beats
        e.sendEvent(Event::noteOff(2, 64));

        // Finish the 1-bar recording (auto-closes at 4.0 beats = 2.0 s)
        renderAppend(e, (int)(1.2f * kSr), L, R, 480);
        CHECK(e.phraseState() == Engine::PhraseState::Playing);
        CHECK_NEAR(e.phraseLength(), 4.0, 1e-6);
        CHECK(e.phraseEventCount() == 4);

        // Quantize recorded phrase via host_utils helper (1/16 = 0.25 beat grid, strength 1.0)
        // Snapshot request runs on audio thread on next block, so drive audio on a short helper thread
        // or directly test requestPhraseSnapshot + quantizePhrase + replacePhrase AND quantizeRecordedPhrase!
        std::atomic<bool> stopAudio{false};
        std::thread audio([&] {
            std::vector<float> al(480), ar(480);
            while (!stopAudio.load(std::memory_order_relaxed)) {
                e.process(al.data(), ar.data(), 480);
                e.collectGarbage();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        std::string err;
        CHECK(quantizeRecordedPhrase(e, 0.25, 1.0, err));
        const std::string smfPath = dir + "/phrase_rt.mid";
        CHECK(exportRecordedPhrase(e, smfPath, 2, err));
        stopAudio.store(true);
        audio.join();
        // Ensure audio thread swap settled
        renderAppend(e, 480, L, R, 480);

        // Verify quantized event timing via snapshot
        e.requestPhraseSnapshot();
        renderAppend(e, 480, L, R, 480);
        std::vector<Engine::PhraseEvent> ev;
        double len = 0.0;
        CHECK(e.takePhraseSnapshot(ev, len));
        CHECK(ev.size() == 4);
        if (ev.size() == 4) {
            CHECK_NEAR(ev[0].beat, 0.25, 1e-6);
            CHECK_NEAR(ev[2].beat, 1.25, 1e-6);
            CHECK_NEAR(ev[1].beat - ev[0].beat, 0.68, 0.03);
            CHECK_NEAR(ev[3].beat - ev[2].beat, 0.60, 0.03);
        }

        // Verify SMF export roundtrip
        Song song;
        CHECK(loadSmf(smfPath, song, err));
        INFO("exported SMF: %d notes, %.3f s\n", song.noteCount, song.length);
        CHECK(song.noteCount == 4);  // 2 notes * 2 repeats
    }

    // 2. Scala (.scl) tuning: parseScala, scalaToOffsets, applyScalaTuning + error handling
    const std::string validScl =
        "! 5-limit Just Intonation\n"
        "5-limit JI (C root)\n"
        " 12\n"
        "!\n"
        " 16/15\n"
        " 9/8\n"
        " 6/5\n"
        " 5/4\n"
        " 4/3\n"
        " 45/32\n"
        " 3/2\n"
        " 8/5\n"
        " 5/3\n"
        " 9/5\n"
        " 15/8\n"
        " 2/1\n";
    const std::string sclFile = dir + "/ji12.scl";
    {
        std::ofstream f(sclFile);
        f << validScl;
    }

    ScalaScale sc;
    std::string err;
    CHECK(parseScala(validScl, sc, err));
    CHECK(sc.description == "5-limit JI (C root)");
    CHECK(sc.cents.size() == 12);
    CHECK_NEAR(sc.cents[3], 1200.0 * std::log2(5.0 / 4.0), 1e-6);  // major 3rd ~386.31 cents
    CHECK_NEAR(sc.cents[11], 1200.0, 1e-6);

    float offsets[12] = {};
    CHECK(scalaToOffsets(sc, 0, offsets, err));
    CHECK_NEAR(offsets[0], 0.0f, 1e-5f);
    CHECK_NEAR(offsets[4], (float)(1200.0 * std::log2(5.0 / 4.0) - 400.0), 1e-3f);  // -13.686 cents

    // Apply to Engine and measure rendered pitch of E4 (MIDI 64 -> 261.6256 * 5/4 = 327.032 Hz)
    {
        Engine e;
        e.prepare(kSr);
        e.setParam(P::FilterType, (float)FilterType::Off);
        e.setParam(P::layer(0, P::LType), (float)LayerType::VA);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        e.setParam(P::AmpSustain, 1.0f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        e.setParam(P::mod(0, P::ModAmt), 0.0f);

        CHECK(applyScalaTuning(e, 0, sclFile, 0, err));
        CHECK_NEAR(e.getPartParam(0, P::TuneC + 4), offsets[4], 1e-3f);

        e.sendEvent(Event::noteOn(1, 64, 0.8f));
        std::vector<float> L, R;
        renderAppend(e, (int)(0.4f * kSr), L, R);
        const double fE4 = zeroCrossFreq(L, kSr, 4800, L.size() - 2400);
        const double expectedJiE4 = 440.0 * std::exp2((60 - 69) / 12.0) * (5.0 / 4.0);
        INFO("JI E4 measured=%.2f Hz expected=%.2f Hz (12-TET=329.63 Hz)\n", fE4, expectedJiE4);
        CHECK_NEAR(fE4, expectedJiE4, 0.5);
    }

    // Invalid .scl handling
    {
        ScalaScale bad;
        CHECK(!parseScala("Bad count\n0\n2/1\n", bad, err) && !err.empty());
        CHECK(!parseScala("Bad ratio\n1\n3/0\n", bad, err) && !err.empty());
        CHECK(!parseScala("Truncated\n12\n100.0\n200.0\n", bad, err) && !err.empty());

        const std::string sevenScl = dir + "/pent7.scl";
        {
            std::ofstream f(sevenScl);
            f << "7-note scale\n7\n200.0\n400.0\n500.0\n700.0\n900.0\n1100.0\n1200.0\n";
        }
        ScalaScale s7;
        CHECK(parseScala("7-note scale\n7\n200.0\n400.0\n500.0\n700.0\n900.0\n1100.0\n1200.0\n", s7, err));
        float off7[12] = {};
        CHECK(!scalaToOffsets(s7, 0, off7, err) && !err.empty());

        Engine e;
        e.prepare(kSr);
        CHECK(!applyScalaTuning(e, 0, sevenScl, 0, err) && !err.empty());
        CHECK(!applyScalaTuning(e, 0, dir + "/does_not_exist.scl", 0, err) && !err.empty());
    }

    // 3. RemoteControl tuning & sst ops
    {
        Engine e;
        e.prepare(kSr);
        RemoteControl::Config cfg;
        cfg.libraryRoot = dir;
        RemoteControl rc(e, cfg);

        Json info = rc.handle(parseJson(R"({"op":"tuning","cmd":"info","file":"ji12.scl"})"));
        CHECK(info.find("ok") && info.find("ok")->boolean());
        CHECK((int)info.find("notes")->number() == 12);
        CHECK_NEAR(info.find("period")->number(), 1200.0, 0.1);

        Json applyR = rc.handle(parseJson(R"({"op":"tuning","cmd":"scala","part":0,"root":0,"file":"ji12.scl"})"));
        CHECK(applyR.find("ok") && applyR.find("ok")->boolean());
        CHECK_NEAR(e.getPartParam(0, P::TuneC + 4), offsets[4], 1e-3f);

        Json resetR = rc.handle(parseJson(R"({"op":"tuning","cmd":"reset","part":0})"));
        CHECK(resetR.find("ok") && resetR.find("ok")->boolean());
        for (int i = 0; i < 12; ++i) CHECK_NEAR(e.getPartParam(0, P::TuneC + i), 0.0f, 1e-6f);

        Json badCmd = rc.handle(parseJson(R"({"op":"tuning","cmd":"bogus","file":"ji12.scl"})"));
        CHECK(badCmd.find("ok") && !badCmd.find("ok")->boolean());

        Json sstR = rc.handle(parseJson(R"({"op":"sst","enabled":true,"time_ms":450})"));
        CHECK(sstR.find("ok") && sstR.find("ok")->boolean());
        CHECK(sstR.find("enabled")->boolean());
        CHECK_NEAR(sstR.find("time_ms")->number(), 450.0, 1e-3);
        CHECK(e.sstEnabled());
        CHECK_NEAR(e.sstTimeMs(), 450.0f, 1e-3f);

        Json sstBad = rc.handle(parseJson(R"({"op":"sst","enabled":"notabool"})"));
        CHECK(sstBad.find("ok") && !sstBad.find("ok")->boolean());
    }

    fs::remove_all(dir);
}
