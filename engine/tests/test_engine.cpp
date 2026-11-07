#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/wav.h"
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

// RPN 0 (pitch bend sensitivity) from MIDI overrides master.bendrange until the knob moves
TEST(engine_rpn_bend_range) {
    Engine e;
    e.prepare(48000);
    sineVoice(e);
    e.setParam(P::BendRange, 2);
    auto cc = [](int n, int v) { return Event::control(EventType::Controller, v / 127.0f, n); };
    RenderJob job;
    job.seconds = 1.0;
    job.events.push_back({0.0, cc(101, 0)});
    job.events.push_back({0.0, cc(100, 0)});
    job.events.push_back({0.0, cc(6, 12)});
    job.events.push_back({0.0, cc(38, 0)});
    job.events.push_back({0.0, cc(101, 127)});
    job.events.push_back({0.0, cc(100, 127)});
    job.events.push_back({0.0, cc(6, 1)});  // no RPN selected: ignored
    job.events.push_back({0.0, Event::control(EventType::PitchBend, 8191.0f / 8192.0f)});
    job.events.push_back({0.0, Event::noteOn(1, 57, 1.0f)});  // A3 + 12 st = 440
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 9600, 40000), 440.0, 1.0);
    // moving the knob takes the bend range back
    e.setParam(P::BendRange, 7);
    RenderJob job2;
    job2.seconds = 1.0;
    job2.events.push_back({0.0, Event::control(EventType::PitchBend, -1.0f)});
    job2.events.push_back({0.0, Event::noteOn(2, 76, 1.0f)});  // E5 - 7 st = A4
    renderOffline(e, job2, L, R);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 9600, 40000), 440.0, 1.0);
}

// second filter: serial low-pass after low-pass darkens more than one; parallel
// balance 1 is filter 2 alone
TEST(engine_dual_filter) {
    auto render = [](auto setup) {
        Engine e;
        e.prepare(48000);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Saw);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        e.setParam(P::mod(0, P::ModAmt), 0.0f);
        e.setParam(P::FilterType, (float)FilterType::LP12);
        e.setParam(P::FilterCutoff, 2000);
        e.setParam(P::FilterEnvAmt, 0);
        e.setParam(P::FilterKeytrack, 0);
        e.setParam(P::AmpSustain, 1.0f);
        setup(e);
        RenderJob job;
        job.seconds = 0.5;
        job.events.push_back({0.0, Event::noteOn(1, 45, 1.0f)});
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        return rms(L, 4800, 24000);
    };
    const double one = render([](Engine&) {});
    const double serial = render([](Engine& e) {
        e.setParam(P::Filter2Type, (float)FilterType::LP12);
        e.setParam(P::Filter2Cutoff, 300);
        e.setParam(P::Filter2Keytrack, 0);
    });
    const double parallelHp = render([](Engine& e) {
        e.setParam(P::Filter2Type, (float)FilterType::HP);
        e.setParam(P::Filter2Cutoff, 8000);
        e.setParam(P::Filter2Keytrack, 0);
        e.setParam(P::FilterRouting, 1);
        e.setParam(P::FilterBalance, 1);
    });
    INFO("rms: one filter %.4f, serial %.4f, parallel HP only %.4f\n", one, serial, parallelHp);
    CHECK(serial < one * 0.9);
    CHECK(parallelHp < one * 0.5);
    CHECK(parallelHp > 0.0001);
}

// scale lock moves keys into the scale; chord mode plays a chord per key and the
// note-off releases exactly those notes
TEST(engine_scale_lock_and_chord) {
    Engine e;
    e.prepare(48000);
    e.setParam(P::ScaleLock, 1);
    e.setParam(P::ScaleRoot, 0);   // C
    e.setParam(P::ScaleType, 0);   // major
    e.setParam(P::ChordMode, 1);   // major triad
    e.setParam(P::AmpRelease, 0.001f);
    std::vector<float> L(480), R(480);
    e.sendEvent(Event::noteOn(7, 61, 0.8f));  // C#4 -> C4; triad C E G (diatonic)
    e.process(L.data(), R.data(), 480);
    Engine::PartMeter m[kNumParts];
    e.partMeters(m);
    const uint32_t w = m[0].notes[1];  // keys 32..63
    const uint32_t w2 = m[0].notes[2]; // keys 64..95
    INFO("held: %08x %08x\n", w, w2);
    CHECK((w >> (60 - 32)) & 1u);       // C4
    CHECK((w2 >> (64 - 64)) & 1u);      // E4
    CHECK((w2 >> (67 - 64)) & 1u);      // G4
    CHECK(!((w >> (61 - 32)) & 1u));    // not C#4
    e.setParam(P::ChordMode, 0);        // switched off while the key is held
    e.sendEvent(Event::noteOff(7, 61));
    for (int b = 0; b < 20; ++b) e.process(L.data(), R.data(), 480);
    e.partMeters(m);
    CHECK(m[0].voices == 0);            // every chord note was released
}

// the Super Knob is one value for all parts, a mod source in each
TEST(engine_super_knob_source) {
    Engine e;
    e.prepare(48000);
    sineVoice(e);
    e.setParam(P::mod(1, P::ModSrc), (float)ModSource::Super);
    e.setParam(P::mod(1, P::ModDst), (float)ModDest::Pitch);
    e.setParam(P::mod(1, P::ModAmt), 0.5f);  // +12 semitones at full knob
    e.setParam(P::SuperKnob, 1.0f);
    CHECK(!isPartParam(P::SuperKnob));
    RenderJob job;
    job.seconds = 0.5;
    job.events.push_back({0.0, Event::noteOn(1, 57, 1.0f)});  // A3 + 12 = 440
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 20000), 440.0, 2.0);
}

// granular layer: grains of a 440 Hz sine (root A4) played at A4 keep the pitch;
// an octave up doubles it; output finite, stereo spread
TEST(engine_granular_layer) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hsynth_tests" / "granular";
    fs::create_directories(dir);
    const std::string wav = (dir / "sine440.wav").generic_string();
    std::vector<float> s(48000 * 2);
    for (size_t i = 0; i < s.size(); ++i) s[i] = 0.5f * std::sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 48000.0);
    std::string err;
    CHECK(writeWav(wav, s, nullptr, 48000, true, err, 69));
    for (int octave = 0; octave < 2; ++octave) {
        Engine e;
        e.prepare(48000);
        e.setParam(P::FilterType, (float)FilterType::Off);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        e.setParam(P::mod(0, P::ModAmt), 0.0f);
        e.setParam(P::AmpSustain, 1.0f);
        e.setParam(P::layer(0, P::LType), (float)LayerType::Granular);
        e.setParam(P::layer(0, P::GrSize), 0.1f);
        e.setParam(P::layer(0, P::GrDensity), 20.0f);
        e.setParam(P::layer(0, P::GrPosRand), 0.0f);
        e.setParam(P::layer(0, P::GrPitchRand), 0.0f);
        std::vector<std::string> warnings;
        CHECK(loadLayerSamples(e, 0, wav, err, warnings));
        RenderJob job;
        job.seconds = 1.0;
        job.events.push_back({0.0, Event::noteOn(1, 69 + 12 * octave, 1.0f)});
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        CHECK(allFinite(L) && allFinite(R));
        const double level = rms(L, 9600, 48000);
        const double f = zeroCrossFreq(L, 48000, 9600, 48000);
        double diff = 0.0;
        for (size_t i = 0; i < L.size(); ++i) diff = std::max(diff, (double)std::fabs(L[i] - R[i]));
        INFO("octave %d: rms %.4f, %.1f Hz, L/R differ %d\n", octave, level, f, (int)(diff > 1e-4));
        CHECK(level > 0.02);
        CHECK_NEAR(f, 440.0 * (1 << octave), 440.0 * (1 << octave) * 0.05);
    }
}

// sampling: trim the silence around a recording, normalise, find a loop in a
// steady tone, save it as an instrument and play it from the SFZ
TEST(engine_sampling_helpers) {
    const int sr = 48000;
    std::vector<float> l(sr * 3, 0.0f), r(sr * 3, 0.0f);
    for (int i = sr / 2; i < sr * 2; ++i) {  // 0.5 .. 2.0 s: 220 Hz at 0.25
        const float v = 0.25f * (float)std::sin(2.0 * 3.14159265358979 * 220.0 * (double)i / sr);
        l[(size_t)i] = v;
        r[(size_t)i] = v * 0.8f;
    }
    int64_t start = 0, end = 0;
    sampleBounds(l, r, -40.0f, start, end);
    INFO("bounds %.3f .. %.3f s\n", (double)start / sr, (double)end / sr);
    CHECK(std::llabs(start - sr / 2) < 200);
    CHECK(std::llabs(end - sr * 2) < 200);
    const float g = normalizeGain(l, r, start, end, -1.0f);
    CHECK_NEAR(g * 0.25f, std::pow(10.0f, -1.0f / 20.0f), 0.01f);
    int64_t ls = 0, le = 0;
    CHECK(findLoop(l, r, start, end, ls, le));
    const double periods = (double)(le - ls) * 220.0 / sr;
    INFO("loop %lld .. %lld (%.3f periods)\n", (long long)ls, (long long)le, periods);
    CHECK(std::fabs(periods - std::round(periods)) < 0.02);  // a whole number of periods: seamless
    const std::string dir = (std::filesystem::temp_directory_path() / "hsynth_tests" / "sampled").generic_string();
    std::string sfz, err;
    CHECK(saveSampledInstrument(dir, "tone", l, r, sr, start, end, g, ls, le, 57, sfz, err));
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setParam(P::mod(0, P::ModAmt), 0.0f);
    e.setParam(P::AmpSustain, 1.0f);
    std::vector<std::string> warnings;
    CHECK(loadLayerSamples(e, 0, sfz, err, warnings));
    RenderJob job;
    job.seconds = 3.0;  // longer than the sample: the loop keeps it going
    job.events.push_back({0.0, Event::noteOn(1, 69, 1.0f)});  // root A3 (57) played at A4: 440 Hz
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    const double f = zeroCrossFreq(L, 48000, 24000, 48000 * 3 - 1000);
    const double tail = rms(L, 48000 * 2, 48000 * 3 - 1000);
    INFO("played back: %.1f Hz, level at 2..3 s %.4f\n", f, tail);
    CHECK_NEAR(f, 440.0, 3.0);
    CHECK(tail > 0.1);
}
