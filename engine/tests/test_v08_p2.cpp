// Phase 2 (v0.8): Performance offsets (transpose + trim), Sample-accurate
// metronome & routes, YIN tuner & muteOut, AudioTap scope waveform/FFT, and
// pedals / curves / footswitches / Remote ops.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "analysis.h"
#include "hsynth/audio_tap.h"
#include "hsynth/dsp/metronome.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/midi.h"
#include "hsynth/performance_input.h"
#include "hsynth/preset.h"
#include "hsynth/remote.h"
#include "hsynth/tuner.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

constexpr float kSr = 48000.0f;

Preset makeSinePreset(const std::string& name, float vaLevel = 0.35f) {
    Preset p;
    p.name = name;
    p.params[(size_t)P::FilterType] = (float)FilterType::Off;
    p.params[(size_t)P::AmpAttack] = 0.002f;
    p.params[(size_t)P::AmpSustain] = 1.0f;
    p.params[(size_t)P::AmpRelease] = 0.015f;
    p.params[(size_t)P::Limiter] = 0.0f;
    p.params[(size_t)P::fxp(fx::Reverb, fx::reverb::On)] = 0.0f;
    p.params[(size_t)P::fxp(fx::Delay, fx::delay::On)] = 0.0f;
    p.params[(size_t)P::fxp(fx::Chorus, fx::chorus::On)] = 0.0f;
    p.params[(size_t)P::mod(0, P::ModAmt)] = 0.0f;
    p.params[(size_t)P::mod(0, P::ModVia)] = 0.0f;
    p.params[(size_t)P::layer(0, P::LType)] = (float)LayerType::VA;
    p.params[(size_t)P::layer(0, P::VaWave1)] = (float)VaWave::Sine;
    p.params[(size_t)P::layer(0, P::VaMix)] = 0.0f;
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

struct EngineSink : EventSink {
    explicit EngineSink(Engine& eng) : engine(eng) {}
    void onEvent(const Event& e) override { engine.sendEvent(e); }
    Engine& engine;
};

void sendMidi1(MidiTranslator& mt, Engine& e, const std::vector<uint8_t>& bytes) {
    EngineSink sink(e);
    mt.processMidi1(bytes.data(), bytes.size(), sink);
}

double midiHz(int key) {
    return 440.0 * std::pow(2.0, (double)(key - 69) / 12.0);
}

}  // namespace

TEST(perf_offsets_transpose_trim_and_isolation) {
    Engine e;
    e.prepare(kSr);
    Preset base = makeSinePreset("PerfSine", 0.25f);
    std::vector<std::string> warnings;
    CHECK(applyPreset(e, base, "", warnings));

    // Clamping checks
    e.setPerfOffsets(20, 12.0f);
    CHECK(e.perfTranspose() == 12);
    CHECK_NEAR(e.perfTrimDb(), 6.0f, 1e-4f);
    e.setPerfOffsets(-25, -24.0f);
    CHECK(e.perfTranspose() == -12);
    CHECK_NEAR(e.perfTrimDb(), -12.0f, 1e-4f);
    e.setPerfOffsets(0, 0.0f);

    // 1. Pitch shift verification: key 60 at 0, +7, -5
    auto measureKeyHz = [&](int transpose, int key) -> double {
        e.setPerfOffsets(transpose, 0.0f);
        std::vector<float> L, R;
        e.sendEvent(Event::noteOn(0, key, 0.8f));
        renderAppend(e, 4800, L, R);
        const double f = zeroCrossFreq(L, kSr, 1200, L.size());
        e.sendEvent(Event::noteOff(0, key));
        renderAppend(e, 2400, L, R);
        return f;
    };
    const double f0 = measureKeyHz(0, 60);
    const double fPlus7 = measureKeyHz(7, 60);
    const double fMinus5 = measureKeyHz(-5, 60);
    INFO("perf_offsets pitch: 0=%g Hz, +7=%g Hz, -5=%g Hz\n", f0, fPlus7, fMinus5);
    CHECK_NEAR(f0, midiHz(60), 1.5);
    CHECK_NEAR(fPlus7, midiHz(67), 2.0);
    CHECK_NEAR(fMinus5, midiHz(55), 1.5);

    // 2. Held note-off tracking when transpose changes mid-note (Poly & Mono, noteId == 0)
    for (int mode = 0; mode < 2; ++mode) {
        e.setParam(P::VoiceMode, mode == 0 ? (float)VoiceMode::Poly : (float)VoiceMode::Mono);
        e.setPerfOffsets(5, 0.0f);
        std::vector<float> L, R;
        e.sendEvent(Event::noteOn(0, 60, 0.8f));
        renderAppend(e, 1024, L, R);
        CHECK(e.stats().activeVoices == 1);
        const double fHeld = zeroCrossFreq(L, kSr, 256, L.size());
        CHECK_NEAR(fHeld, midiHz(65), 2.0);

        // Change transpose while key 60 is still held
        e.setPerfOffsets(-4, 0.0f);
        renderAppend(e, 1024, L, R);
        CHECK(e.stats().activeVoices == 1);

        // Release using original untransposed key 60 (noteId == 0)
        e.sendEvent(Event::noteOff(0, 60));
        L.clear();
        R.clear();
        renderAppend(e, 2400, L, R);
        CHECK(e.stats().activeVoices == 0);
        CHECK(rms(L, 1800, L.size()) < 1e-4);
    }
    e.setParam(P::VoiceMode, (float)VoiceMode::Poly);

    // 3. Smooth trim gain delta (-6 dB and +6 dB)
    e.setPerfOffsets(0, 0.0f);
    e.sendEvent(Event::noteOn(101, 69, 0.8f));  // 440 Hz
    std::vector<float> L, R;
    renderAppend(e, 4800, L, R);
    const double rms0 = rms(L, 2400, 4800);
    CHECK(rms0 > 0.05);

    // Measure normal sample-to-sample delta before trim change
    double steadyMaxStep = 0.0;
    for (size_t i = 2401; i < 4800; ++i)
        steadyMaxStep = std::max(steadyMaxStep, std::fabs((double)L[i] - L[i - 1]));

    // Switch trim to -6 dB: ramp should be smooth (no sudden jump > 1.5x normal sine step)
    e.setPerfOffsets(0, -6.0f);
    const size_t cutIdx = L.size();
    renderAppend(e, 4800, L, R);
    double rampMaxStep = 0.0;
    for (size_t i = cutIdx; i < cutIdx + 1024; ++i)
        rampMaxStep = std::max(rampMaxStep, std::fabs((double)L[i] - L[i - 1]));
    const double rmsMinus6 = rms(L, L.size() - 2400, L.size());
    const double ratioMinus6 = rmsMinus6 / rms0;
    INFO("perf trim -6 dB ratio=%.4f (expected 0.5012), steadyStep=%.4f, rampStep=%.4f\n",
         ratioMinus6, steadyMaxStep, rampMaxStep);
    CHECK(rampMaxStep < steadyMaxStep * 1.5);
    CHECK_NEAR(ratioMinus6, std::pow(10.0, -6.0 / 20.0), 0.015);

    // Switch trim to +6 dB
    e.setPerfOffsets(0, 6.0f);
    L.clear();
    R.clear();
    renderAppend(e, 4800, L, R);
    const double rmsPlus6 = rms(L, 2400, 4800);
    const double ratioPlus6 = rmsPlus6 / rms0;
    INFO("perf trim +6 dB ratio=%.4f (expected 1.9953)\n", ratioPlus6);
    CHECK_NEAR(ratioPlus6, std::pow(10.0, 6.0 / 20.0), 0.03);
    e.sendEvent(Event::noteOff(101, 69));
    renderAppend(e, 2400, L, R);

    // 4. Survival across applyPreset, applyMulti, applyPresetSst
    e.setPerfOffsets(4, -3.5f);
    Preset p2 = makeSinePreset("Patch2", 0.35f);
    CHECK(applyPreset(e, p2, "", warnings));
    CHECK(e.perfTranspose() == 4);
    CHECK_NEAR(e.perfTrimDb(), -3.5f, 1e-4f);

    MultiPreset multi;
    multi.name = "MultiSurvive";
    multi.parts[0] = p2;
    multi.mix[0][Mix::Channel] = 1.0f;
    CHECK(applyMulti(e, multi, "", warnings));
    CHECK(e.perfTranspose() == 4);
    CHECK_NEAR(e.perfTrimDb(), -3.5f, 1e-4f);

    e.setSstEnabled(true);
    e.setSstTimeMs(80.0f);
    uint32_t serial = 0;
    CHECK(applyPresetSst(e, base, "", warnings, &serial, true));
    renderAppend(e, 6000, L, R);
    CHECK(e.liveChangeApplied(serial));
    CHECK(e.perfTranspose() == 4);
    CHECK_NEAR(e.perfTrimDb(), -3.5f, 1e-4f);

    // Verify transpose +4 is actually applied to new note after SST
    L.clear();
    R.clear();
    e.sendEvent(Event::noteOn(200, 60, 0.8f));
    renderAppend(e, 4800, L, R);
    CHECK_NEAR(zeroCrossFreq(L, kSr, 1200, L.size()), midiHz(64), 2.0);
}

TEST(metronome_sample_accurate_timing_and_routes) {
    auto detectClickOnsets = [](const std::vector<float>& buf) {
        std::vector<int> onsets;
        int quietCount = 2000;
        for (size_t i = 0; i < buf.size(); ++i) {
            const float a = std::fabs(buf[i]);
            if (a > 1e-4f) {
                if (quietCount >= 1000) onsets.push_back((int)i);
                quietCount = 0;
            } else {
                ++quietCount;
            }
        }
        return onsets;
    };

    // 1. Render across block sizes: 48f, 96f, 192f, 1024f (LCM = 3072; 40 * 3072 = 122880 frames = 2.56 s)
    constexpr int kTotalFrames = 122880;
    const int blockSizes[] = {48, 96, 192, 1024};
    std::vector<int> refOnsets;
    std::vector<float> refL, refR;

    for (int bs : blockSizes) {
        Engine e;
        e.prepare(kSr);
        e.setParam(P::fxChain(P::FxTempo), 120.0f);
        MetronomeConfig mc;
        mc.enabled = true;
        mc.numerator = 4;
        mc.denominator = 4;
        mc.subdivision = 1;
        mc.countInBars = 0;
        mc.route = 0;  // Stereo
        mc.levelDb = -12.0f;
        mc.accentDb = 6.0f;
        e.setMetronomeConfig(mc);

        std::vector<float> L, R;
        renderAppend(e, kTotalFrames, L, R, bs);
        const auto onsets = detectClickOnsets(L);
        INFO("metronome bs=%d: %zu clicks (first=%d, second=%d)\n",
             bs, onsets.size(), onsets.empty() ? -1 : onsets[0], onsets.size() > 1 ? onsets[1] : -1);
        CHECK(onsets.size() == 6);  // frames ~0, 24000, 48000, 72000, 96000, 120000
        if (refOnsets.empty()) {
            refOnsets = onsets;
            refL = std::move(L);
            refR = std::move(R);
        } else {
            CHECK(onsets.size() == refOnsets.size());
            for (size_t k = 0; k < std::min(onsets.size(), refOnsets.size()); ++k) {
                CHECK(std::abs(onsets[k] - refOnsets[k]) <= 1);
            }
        }
    }

    // 2. Beat-1 accent peak > beat 2-4 peaks (around 0, 24000, 48000, 72000, 96000)
    auto windowPeak = [](const std::vector<float>& buf, int start, int len) {
        double p = 0.0;
        for (int i = start; i < start + len && i < (int)buf.size(); ++i)
            p = std::max(p, (double)std::fabs(buf[(size_t)i]));
        return p;
    };
    const double peakBeat1 = windowPeak(refL, 0, 1000);
    const double peakBeat2 = windowPeak(refL, 24000, 1000);
    const double peakBeat3 = windowPeak(refL, 48000, 1000);
    const double peakBeat4 = windowPeak(refL, 72000, 1000);
    const double peakBar2Beat1 = windowPeak(refL, 96000, 1000);
    INFO("metronome peaks: b1=%.4f b2=%.4f b3=%.4f b4=%.4f b1_next=%.4f\n",
         peakBeat1, peakBeat2, peakBeat3, peakBeat4, peakBar2Beat1);
    CHECK(peakBeat1 > peakBeat2 * 1.4);
    CHECK(peakBeat1 > peakBeat3 * 1.4);
    CHECK(peakBeat1 > peakBeat4 * 1.4);
    CHECK(peakBar2Beat1 > peakBeat2 * 1.4);

    // 3. Route Left (1) / Right (2) channel isolation & currentBeat() advancement
    for (int route : {1, 2}) {
        Engine e;
        e.prepare(kSr);
        e.setParam(P::fxChain(P::FxTempo), 120.0f);
        MetronomeConfig mc;
        mc.enabled = true;
        mc.numerator = 4;
        mc.denominator = 4;
        mc.subdivision = 1;
        mc.route = (uint8_t)route;
        mc.levelDb = -12.0f;
        mc.accentDb = 4.0f;
        e.setMetronomeConfig(mc);

        std::vector<float> L, R;
        // Advance beat-by-beat (24000 frames per beat at 120 BPM)
        int seenBeats[5] = {};
        for (int b = 0; b < 5; ++b) {
            std::vector<float> bl, br;
            renderAppend(e, 24000, bl, br, 256);
            seenBeats[b] = e.clickBeat();
            L.insert(L.end(), bl.begin(), bl.end());
            R.insert(R.end(), br.begin(), br.end());
        }
        CHECK(seenBeats[0] == 0);
        CHECK(seenBeats[1] == 1);
        CHECK(seenBeats[2] == 2);
        CHECK(seenBeats[3] == 3);
        CHECK(seenBeats[4] == 0);

        const double pL = peak(L);
        const double pR = peak(R);
        if (route == 1) {
            CHECK(pL > 0.1);
            CHECK(pR == 0.0);
        } else {
            CHECK(pL == 0.0);
            CHECK(pR > 0.1);
        }

        e.setMetronomeEnabled(false);
        std::vector<float> sl, sr;
        renderAppend(e, 512, sl, sr, 256);
        CHECK(e.clickBeat() == -1);
    }

    // 4. Remote "metronome" op roundtrip
    {
        Engine e;
        e.prepare(kSr);
        RemoteControl rc(e, RemoteControl::Config{});
        Json r = rc.handle(parseJson(
            R"({"op":"metronome","enabled":true,"numerator":3,"denominator":4,"subdivision":2,"countInBars":1,"route":2,"levelDb":-10.0,"accentDb":5.0})"));
        CHECK(r.find("ok")->boolean());
        const Json* cfg = r.find("config");
        CHECK(cfg && cfg->find("enabled")->boolean());
        CHECK((int)cfg->find("numerator")->number() == 3);
        CHECK((int)cfg->find("denominator")->number() == 4);
        CHECK((int)cfg->find("subdivision")->number() == 2);
        CHECK((int)cfg->find("countInBars")->number() == 1);
        CHECK((int)cfg->find("route")->number() == 2);
        CHECK_NEAR(cfg->find("levelDb")->number(), -10.0, 1e-4);
        CHECK_NEAR(cfg->find("accentDb")->number(), 5.0, 1e-4);
        CHECK(r.find("beat") != nullptr);

        Json r2 = rc.handle(parseJson(R"({"op":"metronome"})"));
        CHECK(r2.find("ok")->boolean());
        CHECK((int)r2.find("config")->find("numerator")->number() == 3);
        CHECK((int)r2.find("config")->find("route")->number() == 2);

        Json bad = rc.handle(parseJson(R"({"op":"metronome","denominator":5})"));
        CHECK(!bad.find("ok")->boolean());
    }
}

TEST(tuner_yin_pitch_detection_and_mute) {
    Engine e;
    e.prepare(kSr);
    Preset p = makeSinePreset("TunerSynth", 0.5f);
    std::vector<std::string> warnings;
    CHECK(applyPreset(e, p, "", warnings));

    auto feedSineAndAnalyze = [&](float freqHz, float a4Hz, bool muteOut, TunerReading& reading) -> bool {
        e.tuner().setEnabled(false);
        e.tuner().configure(a4Hz, muteOut);
        e.tuner().setEnabled(true);
        constexpr int kBlock = 512;
        constexpr int kBlocks = 18;  // 9216 frames > 8192 capacity
        std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
        double phase = 0.0;
        const double inc = 2.0 * (double)kPi * freqHz / kSr;
        for (int b = 0; b < kBlocks; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                const float s = 0.4f * (float)std::sin(phase);
                inL[(size_t)i] = inR[(size_t)i] = s;
                phase += inc;
            }
            e.setInputBuffer(inL.data(), inR.data(), kBlock);
            e.process(outL.data(), outR.data(), kBlock);
        }
        return e.tuner().analyze(reading);
    };

    auto centsDiff = [](double detected, double expected) {
        return 1200.0 * std::log2(detected / expected);
    };

    // 1. 440.0 Hz (A4)
    TunerReading r440;
    CHECK(feedSineAndAnalyze(440.0f, 440.0f, false, r440));
    INFO("tuner 440.0 Hz -> %.3f Hz (centsOff=%.2f, conf=%.3f)\n", r440.freqHz, r440.centsOff, r440.confidence);
    CHECK(std::fabs(centsDiff(r440.freqHz, 440.0)) <= 2.0);
    CHECK_NEAR(r440.centsOff, 0.0f, 2.0f);
    CHECK(r440.confidence > 0.9f);

    // 2. 82.41 Hz (E2)
    TunerReading rE2;
    CHECK(feedSineAndAnalyze(82.41f, 440.0f, false, rE2));
    INFO("tuner 82.41 Hz -> %.3f Hz (midi=%.2f, centsOff=%.2f, conf=%.3f)\n",
         rE2.freqHz, rE2.midiNoteFloat, rE2.centsOff, rE2.confidence);
    CHECK(std::fabs(centsDiff(rE2.freqHz, 82.41)) <= 2.0);
    CHECK_NEAR(rE2.midiNoteFloat, 40.0f, 0.05f);
    CHECK(rE2.confidence > 0.9f);

    // 3. 442.5 Hz (against A4=440 -> +9.81 cents; against A4=442.5 -> ~0 cents)
    TunerReading r442;
    CHECK(feedSineAndAnalyze(442.5f, 440.0f, false, r442));
    INFO("tuner 442.5 Hz (A4=440) -> %.3f Hz (centsOff=%.2f, conf=%.3f)\n",
         r442.freqHz, r442.centsOff, r442.confidence);
    CHECK(std::fabs(centsDiff(r442.freqHz, 442.5)) <= 2.0);
    CHECK_NEAR(r442.centsOff, 1200.0f * std::log2(442.5f / 440.0f), 2.0f);
    CHECK(r442.confidence > 0.9f);

    TunerReading r442Cal;
    CHECK(feedSineAndAnalyze(442.5f, 442.5f, false, r442Cal));
    CHECK_NEAR(r442Cal.centsOff, 0.0f, 2.0f);

    // 4. Silence & noise -> rejected or low confidence
    {
        e.tuner().setEnabled(false);
        e.tuner().setEnabled(true);
        std::vector<float> zero(512, 0.0f), outL(512), outR(512);
        for (int b = 0; b < 18; ++b) {
            e.setInputBuffer(zero.data(), zero.data(), 512);
            e.process(outL.data(), outR.data(), 512);
        }
        TunerReading silentRead;
        const bool okSilent = e.tuner().analyze(silentRead);
        CHECK(!okSilent || silentRead.confidence < 0.5f);

        e.tuner().setEnabled(false);
        e.tuner().setEnabled(true);
        std::vector<float> noise(512);
        uint32_t rng = 0x12345678u;
        for (int b = 0; b < 18; ++b) {
            for (int i = 0; i < 512; ++i) {
                rng = rng * 1664525u + 1013904223u;
                noise[(size_t)i] = 0.35f * ((float)(rng >> 8) / 8388608.0f - 1.0f);
            }
            e.setInputBuffer(noise.data(), noise.data(), 512);
            e.process(outL.data(), outR.data(), 512);
        }
        TunerReading noiseRead;
        const bool okNoise = e.tuner().analyze(noiseRead);
        CHECK(!okNoise || noiseRead.confidence < 0.5f);
    }

    // 5. muteOut silences synth output cleanly without blocking tuner input
    {
        e.tuner().setEnabled(false);
        e.tuner().configure(440.0f, true);
        e.tuner().setEnabled(true);
        e.sendEvent(Event::noteOn(1, 60, 0.9f));

        constexpr int kBlock = 512;
        std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock), tailL, tailR;
        double phase = 0.0;
        const double inc = 2.0 * (double)kPi * 440.0 / kSr;
        for (int b = 0; b < 18; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                inL[(size_t)i] = inR[(size_t)i] = 0.4f * (float)std::sin(phase);
                phase += inc;
            }
            e.setInputBuffer(inL.data(), inR.data(), kBlock);
            e.process(outL.data(), outR.data(), kBlock);
            if (b >= 4) {  // well past 15 ms (720 sample) ramp
                tailL.insert(tailL.end(), outL.begin(), outL.end());
                tailR.insert(tailR.end(), outR.begin(), outR.end());
            }
        }
        CHECK(peak(tailL) == 0.0);
        CHECK(peak(tailR) == 0.0);

        TunerReading mutedRead;
        CHECK(e.tuner().analyze(mutedRead));
        CHECK(std::fabs(centsDiff(mutedRead.freqHz, 440.0)) <= 2.0);
        CHECK(mutedRead.confidence > 0.9f);

        // Remote "tuner" op roundtrip
        RemoteControl rc(e, RemoteControl::Config{});
        Json r = rc.handle(parseJson(R"({"op":"tuner","enabled":true,"a4Hz":442.0,"muteOut":false})"));
        CHECK(r.find("ok")->boolean());
        CHECK(r.find("config")->find("enabled")->boolean());
        CHECK_NEAR(r.find("config")->find("a4Hz")->number(), 442.0, 1e-3);
        CHECK(!r.find("config")->find("muteOut")->boolean());
        CHECK(r.find("valid")->boolean());
        CHECK(std::fabs(centsDiff(r.find("freqHz")->number(), 440.0)) <= 2.0);
    }
}

TEST(audio_tap_scope_waveform_and_fft) {
    Engine e;
    e.prepare(kSr);
    Preset p = makeSinePreset("ScopeSine", 0.5f);
    // Key 83 (987.7666 Hz) + LFine +21.3075 cents = 1000.00 Hz
    p.params[(size_t)P::layer(0, P::LFine)] = 21.3075f;
    std::vector<std::string> warnings;
    CHECK(applyPreset(e, p, "", warnings));

    e.audioTap().setEnabled(true);
    CHECK(e.audioTap().enabled());
    e.sendEvent(Event::noteOn(1, 83, 0.9f));

    std::vector<float> L, R;
    renderAppend(e, 4096, L, R, 256);

    std::vector<float> wave(256, 0.0f);
    std::vector<float> spec(64, 0.0f);
    const float f0 = e.audioTap().readFrame(wave.data(), (int)wave.size(), spec.data(), (int)spec.size());
    INFO("audio_tap 1 kHz: f0=%.2f Hz, wavePeak=%.4f, wave[0]=%.4f wave[1]=%.4f\n",
         f0, peak(wave), wave[0], wave[1]);

    // Coherent non-zero waveform starting near rising zero crossing
    CHECK(peak(wave) > 0.1);
    CHECK(wave[0] > 0.0f && wave[1] > wave[0]);
    CHECK_NEAR(f0, 1000.0f, 15.0f);

    // 64-bin log spectrum: bin = floor(64 * log(1000/20) / log(1000)) = 36
    int peakBin = 0;
    for (int i = 1; i < 64; ++i)
        if (spec[(size_t)i] > spec[(size_t)peakBin]) peakBin = i;
    INFO("audio_tap spectrum peakBin=%d (val=%.3f), bin10=%.3f, bin55=%.3f\n",
         peakBin, spec[(size_t)peakBin], spec[10], spec[55]);
    CHECK(std::abs(peakBin - 36) <= 1);
    CHECK(spec[(size_t)peakBin] > 0.6f);
    CHECK(spec[(size_t)peakBin] > spec[10] + 0.4f);
    CHECK(spec[(size_t)peakBin] > spec[55] + 0.4f);

    // Disabling observer clears output cleanly and does not break silentIdle_
    e.audioTap().setEnabled(false);
    CHECK(!e.audioTap().enabled());
    CHECK(e.audioTap().readFrame(wave.data(), (int)wave.size(), spec.data(), (int)spec.size()) == 0.0f);
    CHECK(peak(wave) == 0.0);
    CHECK(peak(spec) == 0.0);

    // Let engine reach silentIdle_, then toggle AudioTap and verify silentIdle_ stays intact
    e.sendEvent(Event::noteOff(1, 83));
    for (int b = 0; b < 12000 && !e.isSilentIdle(); ++b) {
        std::vector<float> bl(48), br(48);
        e.process(bl.data(), br.data(), 48);
    }
    CHECK(e.isSilentIdle());
    e.audioTap().setEnabled(true);
    {
        std::vector<float> bl(128), br(128);
        e.process(bl.data(), br.data(), 128);
    }
    CHECK(e.isSilentIdle());
    CHECK(e.audioTap().readFrame(wave.data(), (int)wave.size(), spec.data(), (int)spec.size()) == 0.0f);
    e.audioTap().setEnabled(false);
    {
        std::vector<float> bl(128), br(128);
        e.process(bl.data(), br.data(), 128);
    }
    CHECK(e.isSilentIdle());
}

TEST(pedal_polarity_curves_and_footswitches) {
    // 1. Inverted CC64 sustain polarity (127 -> up, 0 -> down) via MidiTranslator
    {
        Engine e;
        e.prepare(kSr);
        Preset p = makeSinePreset("PedalSine", 0.4f);
        std::vector<std::string> warnings;
        CHECK(applyPreset(e, p, "", warnings));

        PerformanceInputConfig cfg;
        cfg.sustainInverted = true;
        e.setPerformanceInputConfig(cfg);

        MidiTranslator mt;
        std::vector<float> L, R;
        // CC64 = 0 -> down when inverted
        sendMidi1(mt, e, {0xB0, 64, 0});
        renderAppend(e, 128, L, R);
        CHECK((e.pedalState() & 1) == 1);

        // Play note & release key while inverted sustain holds it
        sendMidi1(mt, e, {0x90, 60, 100});
        renderAppend(e, 512, L, R);
        sendMidi1(mt, e, {0x80, 60, 64});
        renderAppend(e, 2400, L, R);
        CHECK(e.stats().activeVoices == 1);
        CHECK(rms(L, L.size() - 512, L.size()) > 0.05);

        // CC64 = 127 -> up when inverted -> releases sustained note
        sendMidi1(mt, e, {0xB0, 64, 127});
        L.clear();
        R.clear();
        renderAppend(e, 2400, L, R);
        CHECK((e.pedalState() & 1) == 0);
        CHECK(e.stats().activeVoices == 0);
        CHECK(rms(L, 1800, L.size()) < 1e-4);
    }

    // 2. Global velocity curve, fixed velocity, and aftertouch curve
    {
        Engine e;
        e.prepare(kSr);
        Preset p = makeSinePreset("CurveSine", 0.4f);
        // Route ChannelPressure -> Pitch (+24 st * 0.5 = +12 st at pressure 1.0)
        p.params[(size_t)P::mod(0, P::ModSrc)] = (float)ModSource::ChannelPressure;
        p.params[(size_t)P::mod(0, P::ModVia)] = (float)ModSource::None;
        p.params[(size_t)P::mod(0, P::ModDst)] = (float)ModDest::Pitch;
        p.params[(size_t)P::mod(0, P::ModAmt)] = 0.5f;
        std::vector<std::string> warnings;
        CHECK(applyPreset(e, p, "", warnings));

        auto playHalfVelRms = [&](float velCurve, int velFixed) {
            PerformanceInputConfig cfg;
            cfg.velocityCurve = velCurve;
            cfg.velocityFixed = velFixed;
            e.setPerformanceInputConfig(cfg);
            std::vector<float> L, R;
            e.sendEvent(Event::noteOn(1, 60, 0.5f));
            renderAppend(e, 2048, L, R);
            const double r = rms(L, 512, 2048);
            e.sendEvent(Event::noteOff(1, 60));
            renderAppend(e, 2048, L, R);
            return r;
        };
        const double rmsSoft = playHalfVelRms(-0.8f, 0);
        const double rmsLin = playHalfVelRms(0.0f, 0);
        const double rmsHard = playHalfVelRms(0.8f, 0);
        INFO("vel curves at 0.5: soft=%.4f, lin=%.4f, hard=%.4f\n", rmsSoft, rmsLin, rmsHard);
        CHECK(rmsSoft > rmsLin * 1.25);
        CHECK(rmsHard < rmsLin * 0.85);

        // Fixed velocity overrides input velocity
        PerformanceInputConfig fixedCfg;
        fixedCfg.velocityFixed = 96;
        e.setPerformanceInputConfig(fixedCfg);
        auto playVel = [&](float vel) {
            std::vector<float> L, R;
            e.sendEvent(Event::noteOn(2, 60, vel));
            renderAppend(e, 2048, L, R);
            const double r = rms(L, 512, 2048);
            e.sendEvent(Event::noteOff(2, 60));
            renderAppend(e, 2048, L, R);
            return r;
        };
        CHECK_NEAR(playVel(0.15f), playVel(0.95f), 1e-3);

        // Aftertouch curve: pressure 0.5 produces higher pitch with soft (-0.8) than hard (+0.8)
        auto measureAtPitch = [&](float atCurve) {
            PerformanceInputConfig cfg;
            cfg.aftertouchCurve = atCurve;
            e.setPerformanceInputConfig(cfg);
            std::vector<float> L, R;
            e.sendEvent(Event::control(EventType::ChannelPressure, 0.5f));
            e.sendEvent(Event::noteOn(3, 60, 0.8f));
            renderAppend(e, 3072, L, R);
            const double hz = zeroCrossFreq(L, kSr, 1024, 3072);
            e.sendEvent(Event::noteOff(3, 60));
            renderAppend(e, 2048, L, R);
            return hz;
        };
        const double hzAtSoft = measureAtPitch(-0.8f);
        const double hzAtHard = measureAtPitch(0.8f);
        INFO("AT curves at 0.5 pressure: soft=%.2f Hz, hard=%.2f Hz\n", hzAtSoft, hzAtHard);
        CHECK(hzAtSoft > hzAtHard * 1.15);
    }

    // 3. Footswitch edge detection + hysteresis, HostActions, internal actions, lastCc & pedalState
    {
        Engine e;
        e.prepare(kSr);
        e.setParam(P::fxSlot(0), (float)fx::Rotary);
        e.setParam(P::fxp(fx::Rotary, fx::rotary::Fast), 0.0f);
        e.setParam(P::fxChain(P::FxTempo), 100.0f);

        // Slot 0: CC66 -> HostAction 1 (SceneNext)
        // Slot 1: CC67 -> HostAction 2 (ScenePrev), inverted
        // Slot 2: CC68 -> Action 5 (RotaryToggle)
        // Slot 3: CC69 -> Action 8 (MetronomeToggle)
        e.setFootswitch(0, 66, 1, false);
        e.setFootswitch(1, 67, 2, true);
        e.setFootswitch(2, 68, 5, false);
        e.setFootswitch(3, 69, 8, false);
        CHECK(e.footswitchConsumesCc(66));
        CHECK(e.footswitchConsumesCc(67));
        CHECK(!e.footswitchConsumesCc(74));

        auto sendCc = [&](int ch, int cc, float val) {
            Event ev = Event::control(EventType::Controller, val, cc);
            ev.channel = (uint8_t)ch;
            ev.rawCc = (uint8_t)cc;
            e.sendEvent(ev);
        };

        std::vector<float> L, R;
        // Hysteresis on slot 0 (CC66):
        // 0.50 (< 0.55) does NOT trigger
        sendCc(2, 66, 0.50f);
        renderAppend(e, 128, L, R);
        CHECK((e.pedalState() & (2 << 0)) == 0);
        CHECK(e.takeHostAction() == 0);
        // Check lastCc packing: seq=1, ch=2, cc=66
        const uint32_t lc1 = e.lastCc();
        CHECK((lc1 >> 16) == 1u);
        CHECK(((lc1 >> 8) & 0xFFu) == 2u);
        CHECK((lc1 & 0xFFu) == 66u);

        // 0.60 (>= 0.55) triggers once and sets bit 1 (2 << 0)
        sendCc(0, 66, 0.60f);
        renderAppend(e, 128, L, R);
        CHECK((e.pedalState() & (2 << 0)) != 0);
        CHECK(e.takeHostAction() == 1);
        CHECK(e.takeHostAction() == 0);

        // 0.70 while down does not re-trigger; 0.50 (> 0.45) keeps it held
        sendCc(0, 66, 0.70f);
        sendCc(0, 66, 0.50f);
        renderAppend(e, 128, L, R);
        CHECK((e.pedalState() & (2 << 0)) != 0);
        CHECK(e.takeHostAction() == 0);

        // 0.40 (<= 0.45) releases bit 1
        sendCc(0, 66, 0.40f);
        renderAppend(e, 128, L, R);
        CHECK((e.pedalState() & (2 << 0)) == 0);

        // Inverted slot 1 (CC67 -> ScenePrev): 0.0 -> down, 1.0 -> up
        sendCc(0, 67, 0.0f);
        renderAppend(e, 128, L, R);
        CHECK((e.pedalState() & (2 << 1)) != 0);
        CHECK(e.takeHostAction() == 2);
        sendCc(0, 67, 1.0f);
        renderAppend(e, 128, L, R);
        CHECK((e.pedalState() & (2 << 1)) == 0);

        // HostAction 3 (SetListNext) & 4 (SetListPrev)
        e.setFootswitch(0, 66, 3, false);
        e.setFootswitch(1, 67, 4, false);
        sendCc(0, 66, 1.0f);
        sendCc(0, 67, 1.0f);
        renderAppend(e, 128, L, R);
        CHECK(e.takeHostAction() == 3);
        CHECK(e.takeHostAction() == 4);
        CHECK(e.takeHostAction() == 0);

        // Action 5 (RotaryToggle) & Action 8 (MetronomeToggle)
        sendCc(0, 68, 1.0f);
        sendCc(0, 69, 1.0f);
        renderAppend(e, 128, L, R);
        CHECK(e.getParam(P::fxp(fx::Rotary, fx::rotary::Fast)) > 0.5f);
        CHECK(e.metronomeConfig().enabled);
        CHECK(e.clickBeat() >= 0);

        sendCc(0, 68, 0.0f);
        sendCc(0, 69, 0.0f);
        renderAppend(e, 128, L, R);
        sendCc(0, 68, 1.0f);
        sendCc(0, 69, 1.0f);
        renderAppend(e, 128, L, R);
        CHECK(e.getParam(P::fxp(fx::Rotary, fx::rotary::Fast)) < 0.5f);
        CHECK(!e.metronomeConfig().enabled);

        // Action 6 (LooperTap) & Action 7 (TapTempo)
        e.setFootswitch(2, 68, 6, false);
        e.setFootswitch(3, 69, 7, false);
        sendCc(0, 68, 0.0f);
        sendCc(0, 69, 0.0f);
        renderAppend(e, 128, L, R);

        CHECK(e.looperStatus().state == fx::LooperState::Empty);
        sendCc(0, 68, 1.0f);
        renderAppend(e, 128, L, R);
        CHECK(e.looperStatus().state == fx::LooperState::Recording);

        // Tap tempo: 3 taps spaced by 24,000 frames (0.5 s = 120 BPM)
        for (int t = 0; t < 3; ++t) {
            sendCc(0, 69, 1.0f);
            renderAppend(e, 128, L, R);
            sendCc(0, 69, 0.0f);
            renderAppend(e, 24000 - 128, L, R);
        }
        CHECK_NEAR(e.getParam(P::fxChain(P::FxTempo)), 120.0f, 1.0f);

        // Verify pedalState bitmask across sustain + multiple footswitches simultaneously
        sendCc(0, 64, 1.0f);  // sustain bit 0 (via externalCc? wait: CC64 needs rawSustainValue)
        {
            Event sus = Event::control(EventType::Sustain, 1.0f);
            sus.channel = 0;
            sus.rawCc = 64;
            sus.rawSustainValue = 1.0f;
            e.sendEvent(sus);
        }
        sendCc(0, 68, 0.0f);
        renderAppend(e, 128, L, R);
        sendCc(0, 68, 1.0f);  // slot 2 -> bit 3 (2 << 2 = 8)
        sendCc(0, 69, 1.0f);  // slot 3 -> bit 4 (2 << 3 = 16)
        renderAppend(e, 128, L, R);
        CHECK((e.pedalState() & 1) != 0);
        CHECK((e.pedalState() & (2 << 2)) != 0);
        CHECK((e.pedalState() & (2 << 3)) != 0);

        // 4. Remote pedal_config & perf_offsets ops
        RemoteControl rc(e, RemoteControl::Config{});
        Json pr = rc.handle(parseJson(
            R"({"op":"pedal_config","sustainInverted":true,"velocityCurve":-0.5,"velocityFixed":0,"aftertouchCurve":0.4,"expMin":0.1,"expMax":0.9,"footswitches":[{"cc":70,"action":1,"invert":false},{"cc":71,"action":8,"invert":true}]})"));
        CHECK(pr.find("ok")->boolean());
        const Json* pcfg = pr.find("config");
        CHECK(pcfg && pcfg->find("sustainInverted")->boolean());
        CHECK_NEAR(pcfg->find("velocityCurve")->number(), -0.5, 1e-4);
        CHECK_NEAR(pcfg->find("aftertouchCurve")->number(), 0.4, 1e-4);
        CHECK_NEAR(pcfg->find("expMin")->number(), 0.1, 1e-4);
        CHECK_NEAR(pcfg->find("expMax")->number(), 0.9, 1e-4);
        CHECK((int)pcfg->find("footswitches")->at(0).find("cc")->number() == 70);
        CHECK((int)pcfg->find("footswitches")->at(1).find("action")->number() == 8);
        CHECK(pcfg->find("footswitches")->at(1).find("invert")->boolean());
        CHECK(pr.find("lastCc") != nullptr && pr.find("pedalState") != nullptr);

        Json clampPedal = rc.handle(parseJson(R"({"op":"pedal_config","velocityCurve":2.5})"));
        CHECK(clampPedal.find("ok")->boolean());
        CHECK_NEAR(clampPedal.find("config")->find("velocityCurve")->number(), 1.0, 1e-4);
        Json badPedal = rc.handle(parseJson(R"({"op":"pedal_config","velocityCurve":"bad"})"));
        CHECK(!badPedal.find("ok")->boolean());

        Json orReply = rc.handle(parseJson(R"({"op":"perf_offsets","transpose":-5,"trimDb":-2.5})"));
        CHECK(orReply.find("ok")->boolean());
        CHECK((int)orReply.find("config")->find("transpose")->number() == -5);
        CHECK_NEAR(orReply.find("config")->find("trimDb")->number(), -2.5, 1e-4);
        CHECK(e.perfTranspose() == -5);
        CHECK_NEAR(e.perfTrimDb(), -2.5f, 1e-4f);

        Json clampOffsets = rc.handle(parseJson(R"({"op":"perf_offsets","transpose":24})"));
        CHECK(clampOffsets.find("ok")->boolean());
        CHECK((int)clampOffsets.find("config")->find("transpose")->number() == 12);
        Json badOffsets = rc.handle(parseJson(R"({"op":"perf_offsets","transpose":1.5})"));
        CHECK(!badOffsets.find("ok")->boolean());
    }
}
