#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

void sineVoice(Engine& e) {
    e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
    e.setParam(P::layer(0, P::VaMix), 0.0f);
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setParam(P::mod(0, P::ModAmt), 0.0f);
    e.setParam(P::AmpSustain, 1.0f);
}

}  // namespace

TEST(engine_note_on_off) {
    Engine e;
    e.prepare(48000);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);  // release only, no reverb tail
    RenderJob job;
    job.seconds = 2.0;
    addNote(job, 0.1, 0.5, 60, 0.8f);
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK(allFinite(L) && allFinite(R));
    CHECK(rms(L, 0, 4000) < 1e-6);               // silence before the note
    CHECK(rms(L, 9600, 24000) > 0.01);           // sound while held
    CHECK(rms(L, 48000 + 24000, 96000) < 1e-4);  // released
    CHECK(e.stats().activeVoices == 0);
    CHECK(peak(L) <= 1.0);
}

TEST(engine_pitch_and_per_note_expression) {
    Engine e;
    e.prepare(48000);
    sineVoice(e);
    RenderJob job;
    job.seconds = 1.5;
    job.events.push_back({0.0, Event::noteOn(1, 69, 1.0f)});
    // pitch event for a different note id must not affect note 1
    job.events.push_back({0.2, Event::perNote(EventType::NotePitch, 2, 69, 12.0f)});
    job.events.push_back({0.7, Event::perNote(EventType::NotePitch, 1, 69, 12.0f)});
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 14400, 31000), 440.0, 1.0);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 40000, 70000), 880.0, 2.0);
}

TEST(engine_pitch_bend_range) {
    Engine e;
    e.prepare(48000);
    sineVoice(e);
    e.setParam(P::BendRange, 12);
    RenderJob job;
    job.seconds = 1.0;
    job.events.push_back({0.0, Event::control(EventType::PitchBend, -1.0f)});
    job.events.push_back({0.0, Event::noteOn(1, 69, 1.0f)});
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 9600, 40000), 220.0, 1.0);
}

TEST(engine_polyphony_limit_and_stealing) {
    Engine e;
    e.prepare(48000);
    e.setMaxVoices(16);
    RenderJob job;
    job.seconds = 1.0;
    for (int i = 0; i < 40; ++i) job.events.push_back({0.01 * i, Event::noteOn((uint32_t)(i + 1), 40 + i, 0.9f)});
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    const EngineStats s = e.stats();
    INFO("active=%d peak=%d\n", s.activeVoices, s.peakVoices);
    CHECK(s.activeVoices <= 16);
    CHECK(s.peakVoices <= 16 + kVoicePoolExtra);
    CHECK(allFinite(L));
    CHECK(peak(L) <= 1.0);
}

TEST(engine_sustain_pedal) {
    Engine e;
    e.prepare(48000);
    sineVoice(e);
    e.setParam(P::AmpRelease, 0.01f);
    RenderJob job;
    job.seconds = 2.0;
    job.events.push_back({0.0, Event::control(EventType::Sustain, 1.0f)});
    addNote(job, 0.05, 0.1, 60, 1.0f);
    job.events.push_back({1.0, Event::control(EventType::Sustain, 0.0f)});
    std::sort(job.events.begin(), job.events.end(), [](auto& a, auto& b) { return a.time < b.time; });
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK(rms(L, 30000, 45000) > 0.05);  // still sounding at ~0.8 s
    CHECK(rms(L, 60000, 90000) < 1e-4);  // released after pedal up
}

TEST(engine_hybrid_layers_and_morph) {
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(1, P::LType), (float)LayerType::FM);
    e.setParam(P::mod(3, P::ModSrc), (float)ModSource::ModWheel);
    e.setParam(P::mod(3, P::ModDst), (float)ModDest::Morph);
    e.setParam(P::mod(3, P::ModAmt), 1.0f);
    e.setParam(P::Morph, -1.0f);  // only layer A at modwheel 0
    RenderJob job;
    job.seconds = 1.0;
    job.events.push_back({0.0, Event::noteOn(1, 60, 1.0f)});
    job.events.push_back({0.5, Event::control(EventType::ModWheel, 1.0f)});
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK(allFinite(L));
    CHECK(rms(L, 4800, 20000) > 0.01);
    CHECK(rms(L, 30000, 47000) > 0.01);
}

TEST(engine_deterministic_render) {
    auto render = [] {
        Engine e;
        e.prepare(48000);
        e.setParam(P::layer(0, P::VaNoise), 0.2f);
        e.setParam(P::layer(1, P::LType), (float)LayerType::FM);
        e.setParam(P::fxp(fx::Chorus, 0), 1.0f); e.setParam(P::fxp(fx::Chorus, fx::chorus::Mix), 0.5f);
        e.setParam(P::fxp(fx::Delay, 0), 1.0f); e.setParam(P::fxp(fx::Delay, fx::delay::Mix), 0.3f);
        RenderJob job;
        job.seconds = 1.5;
        for (int i = 0; i < 8; ++i) addNote(job, 0.1 * i, 0.3, 48 + i * 3, 0.5f + 0.05f * i);
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        return L;
    };
    const auto a = render(), b = render();
    CHECK(a == b);
}

TEST(engine_random_parameter_stress) {
    Engine e;
    e.prepare(48000);
    const auto& table = paramTable();
    Rng r(42);
    std::vector<float> L(128), R(128);
    bool finite = true;
    float maxPeak = 0;
    uint32_t id = 1;
    for (int b = 0; b < 48000 * 20 / 128; ++b) {  // 20 s
        for (int k = 0; k < 8; ++k) {
            const int pid = (int)(r.next() % (uint32_t)P::kNumParams);
            const ParamInfo& pi = table[(size_t)pid];
            if (pid == P::MasterVolume) continue;
            e.setParam(pid, pi.min + r.unipolar() * (pi.max - pi.min));
        }
        if (b % 7 == 0) e.sendEvent(Event::noteOn(id++, 24 + (int)(r.next() % 80), r.unipolar()));
        if (b % 5 == 0) e.sendEvent(Event::noteOff(id - 3, 0));
        if (b % 11 == 0) e.sendEvent(Event::perNote(EventType::NoteTimbre, id - 1, 0, r.unipolar()));
        e.process(L.data(), R.data(), 128);
        for (int i = 0; i < 128; ++i) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
            maxPeak = std::max(maxPeak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
    }
    CHECK(finite);
    CHECK(maxPeak <= 1.0f);
}

TEST(engine_sample_swap_concurrent) {
    // A loader thread swaps sample sets while the "audio thread" renders.
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    auto makeSet = [](float freq) {
        auto set = std::make_unique<SampleSet>();
        auto s = std::make_unique<Sample>();
        s->frames = 4800;
        s->sampleRate = 48000;
        s->data[0].resize(4800 + Sample::kPadFrames);
        for (int i = 0; i < 4800; ++i) s->data[0][(size_t)i] = 0.5f * std::sin(2.0f * kPi * freq * (float)i / 48000.0f);
        Region r;
        r.sample = s.get();
        r.loopMode = LoopMode::Continuous;
        r.loopStart = 0;
        r.loopEnd = 4800;
        set->regions.push_back(r);
        set->samples.push_back(std::move(s));
        return set;
    };
    std::atomic<bool> stop{false};
    std::thread loader([&] {
        int i = 0;
        while (!stop.load()) {
            e.setSampleSet(0, makeSet(100.0f + (float)(i++ % 10) * 10.0f));
            e.collectGarbage();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    std::vector<float> L(64), R(64);
    bool finite = true;
    for (int b = 0; b < 20000; ++b) {
        if (b % 50 == 0) e.sendEvent(Event::noteOn((uint32_t)b + 1, 60, 0.8f));
        if (b % 50 == 25) e.sendEvent(Event::noteOff((uint32_t)b - 24, 60));
        e.process(L.data(), R.data(), 64);
        for (float v : L) if (!std::isfinite(v)) finite = false;
    }
    stop = true;
    loader.join();
    e.collectGarbage();
    CHECK(finite);
}

TEST(engine_long_run_stability) {
    // Continuous random playing; default 2 minutes of audio, override with
    // HSYNTH_LONG_TEST_SECONDS (e.g. 3600 for a one-hour soak test).
    const char* envSec = std::getenv("HSYNTH_LONG_TEST_SECONDS");
    const int seconds = envSec ? std::max(10, std::atoi(envSec)) : 120;
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(1, P::LType), (float)LayerType::FM);
    e.setParam(P::fxp(fx::Delay, 0), 1.0f); e.setParam(P::fxp(fx::Delay, fx::delay::Mix), 0.3f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::Mix), 0.3f);
    e.setMaxVoices(32);
    Rng r(7);
    std::vector<float> L(256), R(256);
    bool finite = true;
    uint32_t id = 1;
    double tailRms = 0;
    const int blocks = 48000 * seconds / 256;
    for (int b = 0; b < blocks; ++b) {
        if (r.unipolar() < 0.2f) e.sendEvent(Event::noteOn(id++, 36 + (int)(r.next() % 48), 0.3f + 0.7f * r.unipolar()));
        if (r.unipolar() < 0.2f && id > 4) e.sendEvent(Event::noteOff(id - 4, 0));
        e.process(L.data(), R.data(), 256);
        for (float v : L) if (!std::isfinite(v)) finite = false;
        if (b > blocks - 100) tailRms += rms(L);
    }
    e.sendEvent(Event::control(EventType::AllNotesOff, 0));
    for (int b = 0; b < 48000 * 10 / 256; ++b) e.process(L.data(), R.data(), 256);
    CHECK(finite);
    CHECK(tailRms > 0.0);
    CHECK(e.stats().activeVoices == 0);
    CHECK(rms(L) < 1e-3);
}

TEST(engine_performance_benchmark) {
    // Worst-case-ish: 64 voices, 3 layers (VA + FM + VA), all effects.
    Engine e;
    e.prepare(48000);
    e.setMaxVoices(64);
    e.setParam(P::layer(1, P::LType), (float)LayerType::FM);
    e.setParam(P::layer(2, P::LType), (float)LayerType::VA);
    e.setParam(P::AmpSustain, 1.0f);
    e.setParam(P::fxp(fx::Chorus, 0), 1.0f); e.setParam(P::fxp(fx::Chorus, fx::chorus::Mix), 0.5f);
    e.setParam(P::fxp(fx::Delay, 0), 1.0f); e.setParam(P::fxp(fx::Delay, fx::delay::Mix), 0.3f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::Mix), 0.3f);
    for (int i = 0; i < 64; ++i) e.sendEvent(Event::noteOn((uint32_t)i + 1, 30 + i, 0.5f));
    std::vector<float> L(128), R(128);
    e.process(L.data(), R.data(), 128);
    const int blocks = 48000 * 5 / 128;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) e.process(L.data(), R.data(), 128);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO("64 voices x 3 layers: %.3f s CPU for 5 s audio -> load %.1f%% (desktop)\n", sec, sec / 5.0 * 100.0);
    CHECK(e.stats().activeVoices == 64);
}

// Golden-file regression: RMS envelope of a fixed scenario. Guards against
// unintended sound changes from refactoring / optimisation. Set
// HSYNTH_UPDATE_GOLDEN=1 to regenerate.
TEST(engine_golden_render) {
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(1, P::LType), (float)LayerType::FM);
    e.setParam(P::layer(0, P::VaWave2), (float)VaWave::Pulse);
    e.setParam(P::FilterCutoff, 1200.0f);
    e.setParam(P::FilterReso, 0.5f);
    e.setParam(P::fxp(fx::Delay, 0), 1.0f); e.setParam(P::fxp(fx::Delay, fx::delay::Mix), 0.2f);
    RenderJob job;
    job.seconds = 3.0;
    const int keys[] = {48, 55, 60, 64, 67, 72};
    for (int i = 0; i < 6; ++i) addNote(job, 0.2 * i, 0.8, keys[i], 0.4f + 0.1f * (float)i);
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    std::vector<double> env;
    for (size_t i = 0; i + 2400 <= L.size(); i += 2400) env.push_back(rms(L, i, i + 2400) + rms(R, i, i + 2400));

    const std::string path = std::string(HSYNTH_GOLDEN_DIR) + "/golden_render.txt";
    const char* update = std::getenv("HSYNTH_UPDATE_GOLDEN");
    std::ifstream in(path);
    if (!in || (update && update[0] == '1')) {
        std::ofstream out(path);
        for (double v : env) out << v << "\n";
        INFO("golden file written: %s\n", path.c_str());
        return;
    }
    std::vector<double> golden;
    double v;
    while (in >> v) golden.push_back(v);
    CHECK(golden.size() == env.size());
    double maxErr = 0;
    for (size_t i = 0; i < std::min(golden.size(), env.size()); ++i)
        maxErr = std::max(maxErr, std::fabs(golden[i] - env[i]) / (golden[i] + 1e-3));
    INFO("golden max relative error %.5f\n", maxErr);
    CHECK(maxErr < 0.02);
}

TEST(engine_wavetable_layer) {
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(0, P::LType), (float)LayerType::WT);
    e.setParam(P::layer(0, P::WtTable), 2.0f);  // harmonics
    e.setParam(P::layer(0, P::WtPosEnv), 1.0f);
    e.setParam(P::layer(0, P::WtWarpMode), (float)WtWarp::Bend);
    e.setParam(P::layer(0, P::WtWarpAmt), 0.7f);
    e.setParam(P::layer(0, P::WtUnison), 5.0f);
    e.setParam(P::mod(3, P::ModSrc), (float)ModSource::ModWheel);
    e.setParam(P::mod(3, P::ModDst), (float)ModDest::WtPos);
    e.setParam(P::mod(3, P::ModAmt), 0.5f);
    RenderJob job;
    job.seconds = 1.5;
    addNote(job, 0.0, 1.0, 48, 0.9f);
    addNote(job, 0.0, 1.0, 55, 0.9f);
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK(allFinite(L) && allFinite(R));
    CHECK(rms(L, 4800, 40000) > 0.02);
    // unison spread makes the layer stereo
    double diff = 0;
    for (size_t i = 4800; i < 40000; ++i) diff += std::fabs(L[i] - R[i]);
    CHECK(diff > 1.0);
}
