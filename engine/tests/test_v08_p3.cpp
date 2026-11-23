// Phase 3 (v0.8): Physical modeling layer, Vintage drift & pan spread,
// Morph A<->B state interpolation & preset JSON/Remote, Piano realism
// (pedal noise, lid, dynamic comp, soft release, half-damper), and
// Pattern Arp + Strum + Rhythm grooves.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "analysis.h"
#include "hsynth/dsp/physical.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/morph.h"
#include "hsynth/performance_sequence.h"
#include "hsynth/preset.h"
#include "hsynth/remote.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

constexpr float kSr = 48000.0f;

Preset makeCleanVaPreset(const std::string& name, VaWave wave = VaWave::Saw, float level = 0.45f) {
    Preset p;
    p.name = name;
    p.params[(size_t)P::MasterVolume] = 0.7f;
    p.params[(size_t)P::Limiter] = 0.0f;
    p.params[(size_t)P::FilterType] = (float)FilterType::LP24;
    p.params[(size_t)P::FilterCutoff] = 8000.0f;
    p.params[(size_t)P::FilterReso] = 0.1f;
    p.params[(size_t)P::FilterKeytrack] = 0.0f;
    p.params[(size_t)P::FilterEnvAmt] = 0.0f;
    p.params[(size_t)P::FilterVelAmt] = 0.0f;
    p.params[(size_t)P::AmpAttack] = 0.002f;
    p.params[(size_t)P::AmpDecay] = 0.1f;
    p.params[(size_t)P::AmpSustain] = 1.0f;
    p.params[(size_t)P::AmpRelease] = 0.05f;
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
    p.params[(size_t)P::layer(0, P::VaDetune)] = 0.0f;
    p.params[(size_t)P::layer(0, P::LLevel)] = level;
    return p;
}

Preset makePhysicalPreset(const std::string& name, int model = 0, int exciter = 0) {
    Preset p = makeCleanVaPreset(name);
    p.params[(size_t)P::FilterType] = (float)FilterType::Off;
    p.params[(size_t)P::AmpAttack] = 0.001f;
    p.params[(size_t)P::AmpSustain] = 1.0f;
    p.params[(size_t)P::AmpRelease] = 0.08f;
    p.params[(size_t)P::layer(0, P::LType)] = (float)LayerType::Physical;
    p.params[(size_t)P::layer(0, P::LLevel)] = 0.8f;
    p.params[(size_t)P::layer(0, P::PhysModel)] = (float)model;
    p.params[(size_t)P::layer(0, P::PhysExciter)] = (float)exciter;
    p.params[(size_t)P::layer(0, P::PhysDecay)] = 2.0f;
    p.params[(size_t)P::layer(0, P::PhysBowPress)] = 0.45f;
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

double highBandEnergy(const std::vector<float>& x, double sr, size_t from, size_t len, double fStart, double fEnd) {
    double sum = 0.0;
    for (double f = fStart; f <= fEnd; f += 500.0) {
        const double g = goertzel(x, sr, f, from, len);
        sum += g * g;
    }
    return std::sqrt(sum);
}

}  // namespace

TEST(physical_layer_models_stability_and_modes) {
    static_assert((int)LayerType::Physical == 7, "LayerType::Physical must equal 7");

    std::vector<std::string> warnings;

    // 1. All 4 models x 4 exciters across sample rates 44.1 / 48 / 96 kHz and notes 24..108
    for (float sr : {44100.0f, 48000.0f, 96000.0f}) {
        Engine e;
        e.prepare(sr);
        for (int model = 0; model < 4; ++model) {
            for (int exciter = 0; exciter < 4; ++exciter) {
                Preset p = makePhysicalPreset("PhysTest", model, exciter);
                CHECK(applyPreset(e, p, "", warnings));

                // Snapshot physModes before rendering
                float modes[16 * 3] = {};
                const int mCount = e.physModes(0, 0, modes, 16);
                CHECK(mCount == (model == 0 ? 16 : 4));
                for (int i = 0; i < mCount; ++i) {
                    const float ratio = modes[3 * i];
                    const float gain = modes[3 * i + 1];
                    const float decay = modes[3 * i + 2];
                    CHECK(std::isfinite(ratio) && ratio >= 1.0f);
                    CHECK(std::isfinite(gain) && gain >= 0.0f);
                    CHECK(std::isfinite(decay) && decay > 0.0f);
                }

                for (int note : {24, 60, 108}) {
                    if (sr != 48000.0f && note == 60 && exciter % 2 == 1) continue;
                    std::vector<float> L, R;
                    e.sendEvent(Event::noteOn(1, note, 0.85f));
                    renderAppend(e, (int)(0.15f * sr), L, R);
                    CHECK(allFinite(L) && allFinite(R));
                    const double attackRms = rms(L, 0, L.size());
                    CHECK(attackRms > 1e-4);

                    // Release and verify clean decay
                    e.sendEvent(Event::noteOff(1, note));
                    const size_t relStart = L.size();
                    renderAppend(e, (int)(0.75f * sr), L, R);
                    CHECK(allFinite(L) && allFinite(R));
                    const double tailRms = rms(L, L.size() - (size_t)(0.10f * sr), L.size());
                    CHECK(tailRms < attackRms * 0.1);
                    CHECK(tailRms < 0.01);
                    (void)relStart;
                }
            }
        }
    }

    // 2. Extreme parameter values across all 4 models (must remain finite and bounded)
    {
        Engine e;
        e.prepare(kSr);
        for (int model = 0; model < 4; ++model) {
            for (int ext = 0; ext < 2; ++ext) {
                Preset p = makePhysicalPreset("PhysExtreme", model, ext == 0 ? 0 : 3);
                const float v = ext == 0 ? 0.0f : 1.0f;
                p.params[(size_t)P::layer(0, P::PhysDecay)] = ext == 0 ? 0.05f : 20.0f;
                p.params[(size_t)P::layer(0, P::PhysDamp)] = v;
                p.params[(size_t)P::layer(0, P::PhysBright)] = v;
                p.params[(size_t)P::layer(0, P::PhysPos)] = ext == 0 ? 0.05f : 0.95f;
                p.params[(size_t)P::layer(0, P::PhysHardness)] = v;
                p.params[(size_t)P::layer(0, P::PhysInharm)] = v;
                p.params[(size_t)P::layer(0, P::PhysDispersion)] = v;
                p.params[(size_t)P::layer(0, P::PhysPickup)] = ext == 0 ? 0.05f : 0.95f;
                p.params[(size_t)P::layer(0, P::PhysDrive)] = v;
                p.params[(size_t)P::layer(0, P::PhysBody)] = v;
                p.params[(size_t)P::layer(0, P::PhysBowPress)] = v;
                CHECK(applyPreset(e, p, "", warnings));

                std::vector<float> L, R;
                for (int note : {24, 108}) {
                    e.sendEvent(Event::noteOn((uint32_t)note, note, 1.0f));
                    renderAppend(e, (int)(0.12f * kSr), L, R);
                    e.sendEvent(Event::noteOff((uint32_t)note, note));
                    renderAppend(e, (int)(0.20f * kSr), L, R);
                }
                CHECK(allFinite(L) && allFinite(R));
                CHECK(peak(L) < 8.0 && peak(R) < 8.0);
            }
        }
    }

    // 3. String delay pool saturation (> 256 active string layers falls back cleanly to modal)
    {
        Engine e;
        e.prepare(kSr);
        e.setMaxVoices(300);
        Preset p = makePhysicalPreset("PhysSaturate", 0, 0);
        // Use all 3 layers as Physical String -> 100 voices = 300 string layers (> 256 pool slots)
        for (int l = 0; l < kNumLayers; ++l) {
            p.params[(size_t)P::layer(l, P::LType)] = (float)LayerType::Physical;
            p.params[(size_t)P::layer(l, P::PhysModel)] = 0.0f;
            p.params[(size_t)P::layer(l, P::PhysExciter)] = 0.0f;
            p.params[(size_t)P::layer(l, P::LLevel)] = 0.35f;
        }
        p.params[(size_t)P::Limiter] = 1.0f;
        CHECK(applyPreset(e, p, "", warnings));

        std::vector<float> L, R;
        for (int i = 0; i < 100; ++i) {
            e.sendEvent(Event::noteOn((uint32_t)(1000 + i), 36 + (i % 48), 0.7f));
        }
        renderAppend(e, (int)(0.15f * kSr), L, R);
        INFO("physical saturation activeVoices=%d, peak=%.4f\n", e.stats().activeVoices, peak(L));
        CHECK(e.stats().activeVoices == 100);
        CHECK(allFinite(L) && allFinite(R));
        CHECK(rms(L, 0, L.size()) > 1e-3);

        // Release all and verify voices deactivate cleanly and delay slots are freed for subsequent notes
        e.sendEvent(Event::control(EventType::AllNotesOff, 0.0f));
        renderAppend(e, (int)(1.0f * kSr), L, R);
        CHECK(e.stats().activeVoices == 0);

        L.clear();
        R.clear();
        e.sendEvent(Event::noteOn(9999, 60, 0.8f));
        renderAppend(e, (int)(0.1f * kSr), L, R);
        CHECK(allFinite(L) && allFinite(R));
        CHECK(rms(L, 0, L.size()) > 1e-3);
    }
}

TEST(vintage_drift_determinism_and_spread) {
    std::vector<std::string> warnings;

    // 1. voice.vintage == 0 && voice.panspread == 0 is bit-identical to baseline
    auto renderZeroVintage = [&](std::vector<float>& L, std::vector<float>& R) {
        Engine e;
        e.prepare(kSr);
        Preset p = makeCleanVaPreset("VintageBase", VaWave::Saw, 0.5f);
        p.params[(size_t)P::FilterCutoff] = 1800.0f;
        p.params[(size_t)P::VoiceVintage] = 0.0f;
        p.params[(size_t)P::VoicePanSpread] = 0.0f;
        CHECK(applyPreset(e, p, "", warnings));
        for (int i = 0; i < 4; ++i) e.sendEvent(Event::noteOn((uint32_t)(i + 1), 48 + i * 4, 0.8f));
        renderAppend(e, (int)(0.25f * kSr), L, R);
    };
    std::vector<float> baseL1, baseR1, baseL2, baseR2;
    renderZeroVintage(baseL1, baseR1);
    renderZeroVintage(baseL2, baseR2);
    CHECK(baseL1.size() == baseL2.size());
    bool bitExactZero = true;
    for (size_t i = 0; i < baseL1.size(); ++i) {
        if (baseL1[i] != baseL2[i] || baseR1[i] != baseR2[i]) {
            bitExactZero = false;
            break;
        }
    }
    CHECK(bitExactZero);

    // 2. Two fresh Engine instances with voice.vintage > 0 & panspread > 0 produce deterministic bit-identical output
    auto renderWithVintage = [&](std::vector<float>& L, std::vector<float>& R) {
        Engine e;
        e.prepare(kSr);
        Preset p = makeCleanVaPreset("VintageDet", VaWave::Saw, 0.4f);
        p.params[(size_t)P::FilterCutoff] = 1500.0f;
        p.params[(size_t)P::VoiceVintage] = 0.85f;
        p.params[(size_t)P::VoiceVintageRate] = 0.6f;
        p.params[(size_t)P::VoicePanSpread] = 0.8f;
        CHECK(applyPreset(e, p, "", warnings));
        for (int i = 0; i < 5; ++i) e.sendEvent(Event::noteOn((uint32_t)(10 + i), 52 + i * 3, 0.8f));
        renderAppend(e, (int)(0.30f * kSr), L, R);
    };
    std::vector<float> vL1, vR1, vL2, vR2;
    renderWithVintage(vL1, vR1);
    renderWithVintage(vL2, vR2);
    CHECK(vL1.size() == vL2.size());
    bool bitExactVintage = true;
    for (size_t i = 0; i < vL1.size(); ++i) {
        if (vL1[i] != vL2[i] || vR1[i] != vR2[i]) {
            bitExactVintage = false;
            break;
        }
    }
    CHECK(bitExactVintage);

    // 3. Measure voice-to-voice pitch, cutoff, and pan dispersion across distinct voice slots
    // Use multi-timbral silent dummy voices on part 1 (Mix::Volume=0) to force part 0's note into slot N
    auto probeSlot = [&](int targetSlot, float vintage, float panSpread, VaWave wave, double& freqHz, double& hfRms,
                         double& lrRatio) {
        Engine e;
        e.prepare(kSr);
        MultiPreset m;
        m.name = "SlotProbe";
        // Multi mode takes the rack configuration from the global preset.
        m.global = makeCleanVaPreset("Dry probe");
        m.mix[0][Mix::FxSend] = 0.0f;
        m.parts[0] = makeCleanVaPreset("Probe0", wave, 0.5f);
        m.parts[0].params[(size_t)P::FilterType] = wave == VaWave::Sine ? (float)FilterType::Off : (float)FilterType::LP24;
        m.parts[0].params[(size_t)P::FilterCutoff] = 1200.0f;
        m.parts[0].params[(size_t)P::VoiceVintage] = vintage;
        m.parts[0].params[(size_t)P::VoiceVintageRate] = 0.2f;
        m.parts[0].params[(size_t)P::VoicePanSpread] = panSpread;
        m.mix[0][Mix::Channel] = 1.0f;
        m.parts[1] = makeCleanVaPreset("Dummy1", VaWave::Sine, 0.0f);
        m.mix[1][Mix::Channel] = 2.0f;
        m.mix[1][Mix::Volume] = 0.0f;
        CHECK(applyMulti(e, m, "", warnings));

        for (int s = 0; s < targetSlot; ++s) {
            Event d = Event::noteOn((uint32_t)(100 + s), 36 + s, 0.5f);
            d.channel = 1;
            e.sendEvent(d);
        }
        Event target = Event::noteOn(999, 69, 0.8f);  // A4 = 440 Hz
        target.channel = 0;
        e.sendEvent(target);

        std::vector<float> L, R;
        renderAppend(e, (int)(0.20f * kSr), L, R);
        freqHz = zeroCrossFreq(L, kSr, 1024, L.size());
        hfRms = highBandEnergy(L, kSr, 2048, 4096, 1500.0, 5000.0);
        const double rL = rms(L, 1024, L.size());
        const double rR = rms(R, 1024, R.size());
        lrRatio = rR > 1e-9 ? rL / rR : 1.0;
    };

    double f0 = 0, hf0 = 0, lr0 = 0;
    double f1 = 0, hf1 = 0, lr1 = 0;
    std::vector<double> pitches, hfs, lrs;
    for (int slot : {0, 1, 2, 3, 5, 7}) {
        double fp = 0, hfp = 0, lrp = 0;
        probeSlot(slot, 1.0f, 0.0f, VaWave::Sine, fp, hfp, lrp);
        CHECK_NEAR(lrp, 1.0, 1e-5);
        pitches.push_back(fp);
        double fs = 0, hfs_v = 0, lrs_v = 0;
        probeSlot(slot, 1.0f, 1.0f, VaWave::Saw, fs, hfs_v, lrs_v);
        hfs.push_back(hfs_v);
        lrs.push_back(lrs_v);
    }
    probeSlot(0, 0.0f, 0.0f, VaWave::Sine, f0, hf0, lr0);
    probeSlot(3, 0.0f, 0.0f, VaWave::Sine, f1, hf1, lr1);
    CHECK_NEAR(f0, f1, 1e-6);
    CHECK_NEAR(lr0, 1.0, 1e-5);
    CHECK_NEAR(lr1, 1.0, 1e-5);

    const auto [minP, maxP] = std::minmax_element(pitches.begin(), pitches.end());
    const auto [minHf, maxHf] = std::minmax_element(hfs.begin(), hfs.end());
    const auto [minLr, maxLr] = std::minmax_element(lrs.begin(), lrs.end());
    INFO("vintage pitch spread=[%.3f, %.3f] Hz, hf spread=[%.5f, %.5f], L/R ratio=[%.3f, %.3f]\n",
         *minP, *maxP, *minHf, *maxHf, *minLr, *maxLr);
    CHECK(*maxP - *minP > 0.5);
    CHECK(*maxHf / std::max(1e-9, *minHf) > 1.03);
    CHECK(*maxLr - *minLr > 0.15);
}

TEST(morph_state_ab_interpolation_and_preset_json) {
    // 1. Engine morphSetA -> modify FilterCutoff / FilterReso / FilterMorph -> morphCaptureB -> sweep perf.morph
    Engine e;
    e.prepare(kSr);
    std::vector<std::string> warnings;
    Preset base = makeCleanVaPreset("MorphBase", VaWave::Saw, 0.45f);
    base.params[(size_t)P::FilterType] = (float)FilterType::LP24;
    base.params[(size_t)P::FilterCutoff] = 400.0f;
    base.params[(size_t)P::FilterReso] = 0.10f;
    base.params[(size_t)P::FilterMorph] = 0.15f;
    CHECK(applyPreset(e, base, "", warnings));

    e.morphSetA(0);
    e.setPartParam(0, P::FilterCutoff, 6400.0f);
    e.setPartParam(0, P::FilterReso, 0.70f);
    e.setPartParam(0, P::FilterMorph, 0.85f);
    // Non-morphable Morph (mix.morph) if touched should not break capture
    e.setPartParam(0, P::Morph, 0.5f);
    const int captured = e.morphCaptureB(0);
    CHECK(captured == 3);
    CHECK_NEAR(e.getPartParam(0, P::PerfMorph), 1.0f, 1e-5f);

    e.sendEvent(Event::noteOn(1, 48, 0.8f));
    auto sweepTo = [&](float pos, std::vector<float>& L, std::vector<float>& R) {
        e.setPartParam(0, P::PerfMorph, pos);
        // Render > 15 ms morph ramp (2400 frames = 50 ms)
        renderAppend(e, 2400, L, R);
    };

    std::vector<float> L0, R0, Lmid, Rmid, L1, R1;
    sweepTo(0.0f, L0, R0);
    CHECK_NEAR(e.morphNow(0, P::FilterCutoff), 400.0f, 1.0f);
    CHECK_NEAR(e.morphNow(0, P::FilterReso), 0.10f, 1e-3f);
    CHECK_NEAR(e.morphNow(0, P::FilterMorph), 0.15f, 1e-3f);
    const double hf0 = highBandEnergy(L0, kSr, 1200, 1024, 1000.0, 5000.0);

    sweepTo(0.5f, Lmid, Rmid);
    // FilterCutoff uses ParamCurve::Exp -> geometric mean sqrt(400 * 6400) = 1600 Hz
    CHECK_NEAR(e.morphNow(0, P::FilterCutoff), 1600.0f, 5.0f);
    CHECK_NEAR(e.morphNow(0, P::FilterReso), 0.40f, 1e-3f);
    CHECK_NEAR(e.morphNow(0, P::FilterMorph), 0.50f, 1e-3f);
    const double hfMid = highBandEnergy(Lmid, kSr, 1200, 1024, 1000.0, 5000.0);

    sweepTo(1.0f, L1, R1);
    CHECK_NEAR(e.morphNow(0, P::FilterCutoff), 6400.0f, 5.0f);
    CHECK_NEAR(e.morphNow(0, P::FilterReso), 0.70f, 1e-3f);
    CHECK_NEAR(e.morphNow(0, P::FilterMorph), 0.85f, 1e-3f);
    const double hf1 = highBandEnergy(L1, kSr, 1200, 1024, 1000.0, 5000.0);

    INFO("morph HF energy at 0=%.5f, 0.5=%.5f, 1.0=%.5f\n", hf0, hfMid, hf1);
    CHECK(hf0 < hfMid && hfMid < hf1);
    CHECK(allFinite(L0) && allFinite(Lmid) && allFinite(L1));
    e.sendEvent(Event::noteOff(1, 48));
    renderAppend(e, 2400, L1, R1);

    // 2. presetToJson / presetFromJson preserves "morphB", legacy v1 without "morphB" loads warning-free
    Preset capturedPreset = capturePreset(e, "MorphRoundtrip", "");
    CHECK(capturedPreset.morph.count == 3);
    const std::string jsonStr = presetToJson(capturedPreset);
    CHECK(jsonStr.find("\"morphB\"") != std::string::npos);

    Preset parsed;
    std::string err;
    CHECK(presetFromJson(jsonStr, parsed, err));
    CHECK(parsed.warnings.empty());
    CHECK(parsed.morph.count == 3);

    const char* legacyV1 = R"({
        "format": "hsynth-preset",
        "version": 1,
        "name": "LegacyV1",
        "params": {
            "master.volume": 0.7,
            "a.type": "va",
            "filter.cutoff": 1200.0
        }
    })";
    Preset legacyOut;
    CHECK(presetFromJson(legacyV1, legacyOut, err));
    CHECK(legacyOut.warnings.empty());
    CHECK(legacyOut.morph.count == 0);

    // 3. Remote "morph" op
    RemoteControl rc(e, RemoteControl::Config{});
    Json r = rc.handle(parseJson(R"({"op":"morph","cmd":"clear_all"})"));
    CHECK(r.find("ok")->boolean());
    CHECK((int)r.find("count")->number() == 0);

    r = rc.handle(parseJson(R"({"op":"morph","cmd":"set_a"})"));
    CHECK(r.find("ok")->boolean());
    CHECK(rc.handle(parseJson(R"({"op":"set","values":{"filter.cutoff":3200,"filter.reso":0.55}})")).find("ok")->boolean());
    r = rc.handle(parseJson(R"({"op":"morph","cmd":"capture_b"})"));
    CHECK(r.find("ok")->boolean());
    CHECK((int)r.find("count")->number() == 2);

    r = rc.handle(parseJson(R"({"op":"morph","cmd":"assign","param":"a.level","a":0.2,"b":0.9})"));
    CHECK(r.find("ok")->boolean());
    CHECK((int)r.find("count")->number() == 3);

    r = rc.handle(parseJson(R"({"op":"morph","cmd":"set_amount","amount":0.5,"param":"a.level"})"));
    CHECK(r.find("ok")->boolean());
    std::vector<float> tmpL, tmpR;
    renderAppend(e, 1024, tmpL, tmpR);
    r = rc.handle(parseJson(R"({"op":"morph","cmd":"get","param":"a.level"})"));
    CHECK(r.find("ok")->boolean());
    CHECK_NEAR(r.find("amount")->number(), 0.5, 1e-3);
    CHECK_NEAR(r.find("now")->number(), 0.55, 0.02);

    r = rc.handle(parseJson(R"({"op":"morph","cmd":"clear","param":"a.level"})"));
    CHECK(r.find("ok")->boolean());
    CHECK((int)r.find("count")->number() == 2);
}

TEST(piano_realism_pedal_noise_lid_and_soft_release) {
    std::vector<std::string> warnings;

    // 1. piano.pedalnoise: audible transient on CC64 down/up edges, silent when 0
    {
        auto sendCc64 = [](Engine& eng, float val) {
            Event sus = Event::control(EventType::Sustain, val >= 0.5f ? 1.0f : 0.0f);
            sus.channel = 0;
            sus.rawCc = 64;
            sus.rawSustainValue = val;
            eng.sendEvent(sus);
        };

        Engine eSilent;
        eSilent.prepare(kSr);
        Preset p0 = makeCleanVaPreset("PedalSilent");
        p0.params[(size_t)P::PianoPedalNoise] = 0.0f;
        CHECK(applyPreset(eSilent, p0, "", warnings));

        std::vector<float> Ls, Rs;
        sendCc64(eSilent, 1.0f);
        renderAppend(eSilent, (int)(0.10f * kSr), Ls, Rs);
        sendCc64(eSilent, 0.0f);
        renderAppend(eSilent, (int)(0.10f * kSr), Ls, Rs);
        CHECK(peak(Ls) == 0.0 && peak(Rs) == 0.0);

        Engine eNoise;
        eNoise.prepare(kSr);
        Preset p1 = p0;
        p1.params[(size_t)P::PianoPedalNoise] = 0.85f;
        CHECK(applyPreset(eNoise, p1, "", warnings));

        std::vector<float> Ln, Rn;
        sendCc64(eNoise, 1.0f);
        renderAppend(eNoise, (int)(0.10f * kSr), Ln, Rn);
        const double downRms = rms(Ln, 0, Ln.size());

        const size_t upStart = Ln.size();
        sendCc64(eNoise, 0.0f);
        renderAppend(eNoise, (int)(0.10f * kSr), Ln, Rn);
        const double upRms = rms(Ln, upStart, Ln.size());

        INFO("pedal noise downRms=%.5f, upRms=%.5f\n", downRms, upRms);
        CHECK(allFinite(Ln) && allFinite(Rn));
        CHECK(downRms > 0.003);
        CHECK(upRms > 0.002);
    }

    // 2. piano.lid: exact bypass at 1.0, darkens HF spectrum when < 1.0
    {
        auto renderSawNote = [&](float lid, std::vector<float>& L, std::vector<float>& R) {
            Engine e;
            e.prepare(kSr);
            Preset p = makeCleanVaPreset("LidTest", VaWave::Saw, 0.5f);
            p.params[(size_t)P::FilterType] = (float)FilterType::Off;
            p.params[(size_t)P::PianoLid] = lid;
            CHECK(applyPreset(e, p, "", warnings));
            e.sendEvent(Event::noteOn(1, 48, 0.9f));
            renderAppend(e, (int)(0.20f * kSr), L, R);
        };

        std::vector<float> Lopen, Ropen, Lopen2, Ropen2, Lclosed, Rclosed;
        renderSawNote(1.0f, Lopen, Ropen);
        renderSawNote(1.0f, Lopen2, Ropen2);
        renderSawNote(0.0f, Lclosed, Rclosed);

        // Exact bypass determinism at 1.0
        bool exact = (Lopen == Lopen2) && (Ropen == Ropen2);
        CHECK(exact);

        const double hfOpen = highBandEnergy(Lopen, kSr, 2048, 4096, 3000.0, 10000.0);
        const double hfClosed = highBandEnergy(Lclosed, kSr, 2048, 4096, 3000.0, 10000.0);
        INFO("piano.lid HF energy open=%.5f, closed=%.5f\n", hfOpen, hfClosed);
        CHECK(hfClosed < hfOpen * 0.65);
    }

    // 3. piano.dyncomp: reduces dynamic peak range between loud (vel=1.0) and soft (vel=0.25) notes
    {
        auto measureRatio = [&](float dynComp) {
            auto playVel = [&](float vel) {
                Engine e;
                e.prepare(kSr);
                Preset p = makeCleanVaPreset("DynTest", VaWave::Sine, 0.9f);
                p.params[(size_t)P::FilterType] = (float)FilterType::Off;
                p.params[(size_t)P::AmpVelSens] = 1.0f;
                p.params[(size_t)P::PianoDynComp] = dynComp;
                CHECK(applyPreset(e, p, "", warnings));
                e.sendEvent(Event::noteOn(1, 60, vel));
                std::vector<float> L, R;
                renderAppend(e, (int)(0.10f * kSr), L, R);
                return peak(L);
            };
            const double loud = playVel(1.0f);
            const double soft = playVel(0.25f);
            return loud / std::max(1e-9, soft);
        };
        const double ratioOff = measureRatio(0.0f);
        const double ratioOn = measureRatio(1.0f);
        INFO("piano.dyncomp peak ratio off=%.3f, on=%.3f\n", ratioOff, ratioOn);
        CHECK(ratioOn < ratioOff * 0.85);
    }

    // 4. piano.softrelease: lengthens release tail
    {
        auto measureTail = [&](float softRel) {
            Engine e;
            e.prepare(kSr);
            Preset p = makeCleanVaPreset("SoftRel", VaWave::Sine, 0.6f);
            p.params[(size_t)P::FilterType] = (float)FilterType::Off;
            p.params[(size_t)P::AmpRelease] = 0.08f;
            p.params[(size_t)P::PianoSoftRelease] = softRel;
            CHECK(applyPreset(e, p, "", warnings));
            e.sendEvent(Event::noteOn(1, 60, 0.8f));
            std::vector<float> L, R;
            renderAppend(e, (int)(0.05f * kSr), L, R);
            const size_t offAt = L.size();
            e.sendEvent(Event::noteOff(1, 60));
            renderAppend(e, (int)(0.20f * kSr), L, R);
            return rms(L, offAt + (size_t)(0.06f * kSr), offAt + (size_t)(0.15f * kSr));
        };
        const double tail0 = measureTail(0.0f);
        const double tail1 = measureTail(1.0f);
        INFO("piano.softrelease tail rms 0=%.5f, 1=%.5f\n", tail0, tail1);
        CHECK(tail1 > tail0 * 2.0);
    }
}

TEST(pattern_arp_strum_and_rhythm_grooves) {
    std::vector<std::string> warnings;

    // 1. Pattern Arp: rests (gate=0), ties (gate=2), accents, octave shifts, chord steps, and JSON roundtrip
    {
        Engine e;
        e.prepare(kSr);
        Preset p = makeCleanVaPreset("PatArp", VaWave::Sine, 0.5f);
        p.params[(size_t)P::FilterType] = (float)FilterType::Off;
        p.params[(size_t)P::AmpAttack] = 0.001f;
        p.params[(size_t)P::AmpRelease] = 0.008f;
        p.params[(size_t)P::AmpVelSens] = 0.8f;
        p.params[(size_t)P::ArpOn] = 1.0f;
        p.params[(size_t)P::ArpMode] = (float)ArpMode::Pattern;
        // 1/4 beat step at 120 BPM = 0.5 s/beat * 1.0 = 0.5 s? Let's check ArpRate:
        // fx::syncBeats(idx + 1): let's set FxTempo = 240 BPM -> 4 beats/s, ArpRate = 1/16 (idx 10 -> 0.25 beat = 62.5 ms = 3000 frames)
        p.params[(size_t)P::fxChain(P::FxTempo)] = 240.0f;
        p.params[(size_t)P::ArpRate] = 10.0f;  // 1/16 = 0.25 beats = 3000 frames at 240 BPM / 48 kHz
        p.params[(size_t)P::ArpGate] = 0.6f;

        // Pattern (length 6):
        // 0: normal C4 (gate=1, accent=100, oct=0)
        // 1: tie (gate=2) -> holds step 0 continuously!
        // 2: rest (gate=0) -> silent!
        // 3: loud +1 oct (gate=1, accent=180, oct=1) -> 2x freq, higher amp
        // 4: soft -1 oct (gate=1, accent=40, oct=-1) -> 0.5x freq, lower amp
        // 5: chord step (gate=1, accent=100, oct=0, chord=true) -> all held notes simultaneously
        ArpPattern pat;
        pat.length = 6;
        pat.steps[0] = {1, 100, 0, false};
        pat.steps[1] = {2, 100, 0, false};
        pat.steps[2] = {0, 100, 0, false};
        pat.steps[3] = {1, 180, 1, false};
        pat.steps[4] = {1, 40, -1, false};
        pat.steps[5] = {1, 100, 0, true};
        p.arpPattern = pat;
        CHECK(applyPreset(e, p, "", warnings));

        // Hold two notes: C4 (60) and G4 (67)
        e.sendEvent(Event::noteOn(1, 60, 0.55f));
        e.sendEvent(Event::noteOn(2, 67, 0.55f));

        // Render 6 steps (each step = 3000 frames)
        std::vector<float> L, R;
        renderAppend(e, 6 * 3000, L, R, 128);
        CHECK(e.arpStep(0) >= 0);

        // Step 0 (0..3000) & Step 1 (3000..6000): tied C4 (~261.6 Hz) continuously audible across boundary
        const double fStep0 = zeroCrossFreq(L, kSr, 400, 2600);
        const double rmsTieBoundary = rms(L, 2800, 3400);
        const double fStep1 = zeroCrossFreq(L, kSr, 3400, 5200);
        CHECK_NEAR(fStep0, 261.63, 4.0);
        CHECK_NEAR(fStep1, 261.63, 4.0);
        CHECK(rmsTieBoundary > 0.05);

        // Step 2 (6000..9000): rest -> decayed to silence
        const double rmsRest = rms(L, 7000, 8800);
        CHECK(rmsRest < 1e-3);

        // Step 3 (9000..12000): index advances (note G4=67, oct=+1 => G5 ~ 783.99 Hz, accent=180 -> louder)
        // Step 4 (12000..15000): note C4=60, oct=-1 => C3 ~ 130.81 Hz, accent=40 -> softer
        const double fStep3 = zeroCrossFreq(L, kSr, 9300, 10600);
        const double rmsStep3 = rms(L, 9300, 10600);
        const double fStep4 = zeroCrossFreq(L, kSr, 12300, 13600);
        const double rmsStep4 = rms(L, 12300, 13600);
        INFO("pattern arp: s0=%.1f Hz, tieRms=%.4f, restRms=%.5f, s3=%.1f Hz (rms=%.4f), s4=%.1f Hz (rms=%.4f)\n",
             fStep0, rmsTieBoundary, rmsRest, fStep3, rmsStep3, fStep4, rmsStep4);
        CHECK_NEAR(fStep3, 783.99, 10.0);
        CHECK_NEAR(fStep4, 130.81, 3.0);
        CHECK(rmsStep3 > rmsStep4 * 1.5);

        // Step 5 (15000..18000): chord=true -> both C4 (261.63 Hz) and G4 (392.00 Hz) present!
        const double gC4 = goertzel(L, kSr, 261.63, 15200, 1500);
        const double gG4 = goertzel(L, kSr, 392.00, 15200, 1500);
        CHECK(gC4 > 0.05 && gG4 > 0.05);

        e.sendEvent(Event::noteOff(1, 60));
        e.sendEvent(Event::noteOff(2, 67));
        renderAppend(e, 2400, L, R);

        // Preset JSON "arpPattern" roundtrip
        const Preset cap = capturePreset(e, "PatRoundtrip", "");
        CHECK(!cap.arpPattern.isDefault());
        const std::string json = presetToJson(cap);
        CHECK(json.find("\"arpPattern\"") != std::string::npos);
        Preset back;
        std::string err;
        CHECK(presetFromJson(json, back, err));
        CHECK(back.warnings.empty());
        CHECK(back.arpPattern.length == 6);
        CHECK(back.arpPattern.steps[1].gate == 2);
        CHECK(back.arpPattern.steps[2].gate == 0);
        CHECK(back.arpPattern.steps[3].accent == 180 && back.arpPattern.steps[3].oct == 1);
        CHECK(back.arpPattern.steps[4].oct == -1);
        CHECK(back.arpPattern.steps[5].chord);
    }

    // 2. Strum: staggers chord note onsets chronologically and cancels cleanly on key-up
    {
        Engine e;
        e.prepare(kSr);
        Preset p = makeCleanVaPreset("StrumTest", VaWave::Sine, 0.35f);
        p.params[(size_t)P::FilterType] = (float)FilterType::Off;
        p.params[(size_t)P::AmpAttack] = 0.001f;
        p.params[(size_t)P::AmpRelease] = 0.02f;
        p.params[(size_t)P::StrumOn] = 1.0f;
        p.params[(size_t)P::StrumOrder] = 0.0f;    // up (low -> high)
        p.params[(size_t)P::StrumWindow] = 10.0f;  // 10 ms = 480 frames
        p.params[(size_t)P::StrumSpacing] = 40.0f; // 40 ms = 1920 frames between notes
        p.params[(size_t)P::StrumJitter] = 0.0f;
        CHECK(applyPreset(e, p, "", warnings));

        // Send 3 simultaneous notes C4 (60), E4 (64), G4 (67)
        e.sendEvent(Event::noteOn(10, 67, 0.8f));
        e.sendEvent(Event::noteOn(11, 60, 0.8f));
        e.sendEvent(Event::noteOn(12, 64, 0.8f));

        // Window 10 ms = 480 frames -> first note (60) flushes around frame 480,
        // second note (64) around 480 + 1920 = 2400, third (67) around 480 + 3840 = 4320
        std::vector<float> L, R;
        renderAppend(e, 1440, L, R, 128);  // 30 ms: only C4 should be sounding!
        CHECK(e.stats().activeVoices == 1);
        CHECK_NEAR(zeroCrossFreq(L, kSr, 600, 1400), 261.63, 4.0);

        renderAppend(e, 1920, L, R, 128);  // up to 70 ms: C4 + E4 sounding (2 voices)
        CHECK(e.stats().activeVoices == 2);

        renderAppend(e, 1920, L, R, 128);  // up to 110 ms: all 3 voices sounding
        CHECK(e.stats().activeVoices == 3);

        e.sendEvent(Event::noteOff(10, 67));
        e.sendEvent(Event::noteOff(11, 60));
        e.sendEvent(Event::noteOff(12, 64));
        renderAppend(e, 2400, L, R, 128);
        CHECK(e.stats().activeVoices == 0);

        // Key-up before strum finishes cancels pending notes cleanly
        e.sendEvent(Event::noteOn(20, 60, 0.8f));
        e.sendEvent(Event::noteOn(21, 64, 0.8f));
        e.sendEvent(Event::noteOn(22, 67, 0.8f));
        renderAppend(e, 1024, L, R, 128);  // 21 ms: first note active, notes 21 & 22 still pending
        e.sendEvent(Event::noteOff(20, 60));
        e.sendEvent(Event::noteOff(21, 64));
        e.sendEvent(Event::noteOff(22, 67));
        renderAppend(e, 4800, L, R, 128);  // past 80 ms spacing: cancelled notes must NOT fire
        CHECK(e.stats().activeVoices == 0);
    }

    // 3. Rhythm 16 patterns + fill + Remote arp_pattern & rhythm ops
    {
        const Json names = rhythmPatternsJson();
        CHECK(names.isArray() && names.size() == 16);

        Engine e;
        e.prepare(kSr);
        e.setParam(P::Limiter, 0.0f);
        e.setParam(P::fxChain(P::FxTempo), 180.0f);

        for (int pat = 0; pat < 16; ++pat) {
            Json cfg = Json::object();
            cfg.set("enabled", true);
            cfg.set("pattern", pat);
            cfg.set("levelDb", -6.0);
            cfg.set("swing", 0.1);
            cfg.set("fill", false);
            CHECK(e.setRhythmJson(cfg));
            std::vector<float> L, R;
            renderAppend(e, (int)(0.40f * kSr), L, R, 256);
            CHECK(allFinite(L) && allFinite(R));
            CHECK(rms(L, 0, L.size()) > 1e-3);
        }

        // Fill adds energy during the last 4 16th steps (steps 12..15) of a 16-step bar
        auto renderBarEnd = [&](bool fill) {
            Engine er;
            er.prepare(kSr);
            er.setParam(P::Limiter, 0.0f);
            er.setParam(P::fxChain(P::FxTempo), 240.0f);  // 4 beats/sec -> 16th = 62.5 ms; 16 steps = 1.0 s
            Json cfg = Json::object();
            cfg.set("enabled", true);
            cfg.set("pattern", 15);  // Click Groove (sparse on steps 12..15: only step 12 rim)
            cfg.set("levelDb", -3.0);
            cfg.set("fill", fill);
            CHECK(er.setRhythmJson(cfg));
            std::vector<float> L, R;
            renderAppend(er, (int)(1.0f * kSr), L, R, 128);
            // Last quarter of the 1-second bar (steps 12..15)
            return rms(L, (size_t)(0.75f * kSr), L.size());
        };
        const double noFillRms = renderBarEnd(false);
        const double withFillRms = renderBarEnd(true);
        INFO("rhythm bar-end rms noFill=%.5f, withFill=%.5f\n", noFillRms, withFillRms);
        CHECK(withFillRms > noFillRms * 1.4);

        // Remote "arp_pattern" & "rhythm" ops
        RemoteControl rc(e, RemoteControl::Config{});
        Json arpReply = rc.handle(parseJson(
            R"({"op":"arp_pattern","part":0,"config":{"length":4,"steps":[{"gate":1,"accent":120,"oct":0,"chord":false},{"gate":2},{"gate":0},{"gate":1,"accent":90,"oct":1,"chord":true}]}})"));
        CHECK(arpReply.find("ok")->boolean());
        CHECK((int)arpReply.find("config")->find("length")->number() == 4);
        CHECK(e.arpPattern(0).length == 4);
        CHECK(e.arpPattern(0).steps[1].gate == 2);
        CHECK(e.arpPattern(0).steps[2].gate == 0);
        CHECK(e.arpPattern(0).steps[3].chord);

        Json rhyReply = rc.handle(parseJson(
            R"({"op":"rhythm","config":{"enabled":true,"pattern":5,"levelDb":-9.0,"swing":0.25,"fill":true}})"));
        CHECK(rhyReply.find("ok")->boolean());
        CHECK(rhyReply.find("patterns")->isArray() && rhyReply.find("patterns")->size() == 16);
        CHECK((int)rhyReply.find("config")->find("pattern")->number() == 5);
        CHECK(rhyReply.find("config")->find("fill")->boolean());
        CHECK((int)e.rhythmJson().find("pattern")->number() == 5);
    }
}
