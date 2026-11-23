// Real-time contract (engine.h): process() never allocates. Global operator
// new is replaced in the test binary; while a test's "audio thread" is inside
// process(), every allocation is counted.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

#include "hsynth/wav.h"

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/sample_set.h"
#include "test.h"

namespace {
thread_local bool t_inAudio = false;
std::atomic<long> g_audioAllocs{0};
void* volatile g_sink = nullptr;
}  // namespace

void* operator new(std::size_t n) {
    if (t_inAudio) g_audioAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace hsynth;
using namespace hsynth_test;

namespace {
long audioBlock(Engine& e, float* L, float* R, int n) {
    const long before = g_audioAllocs.load();
    t_inAudio = true;
    e.process(L, R, n);
    t_inAudio = false;
    return g_audioAllocs.load() - before;
}
std::unique_ptr<SampleSet> loopSet(float freq) {
    auto set = std::make_unique<SampleSet>();
    auto s = std::make_unique<Sample>();
    s->frames = 4800;
    s->data[0].resize(4800 + Sample::kPadFrames);
    for (int i = 0; i < 4800; ++i) s->data[0][(size_t)i] = 0.4f * std::sin(2.0f * kPi * freq * (float)i / 48000.0f);
    Region r;
    r.sample = s.get();
    r.keycenter = 48;  // most notes play above the root: sinc + mips
    r.loopMode = LoopMode::Continuous;
    r.loopStart = 0;
    r.loopEnd = 4800;
    set->regions.push_back(r);
    Region rel = r;
    rel.trigger = RegionTrigger::Release;
    rel.loopMode = LoopMode::NoLoop;
    set->regions.push_back(rel);
    set->hasReleaseRegions = true;
    set->samples.push_back(std::move(s));
    set->prepareMips();
    return set;
}
}  // namespace

TEST(realtime_process_never_allocates) {
    // the detector itself counts
    {
        const long before = g_audioAllocs.load();
        t_inAudio = true;
        auto* v = new std::vector<float>(100);  // escapes into a global: cannot be elided
        g_sink = v;
        t_inAudio = false;
        delete static_cast<std::vector<float>*>(g_sink);
        INFO("detector self-check: %ld allocations counted\n", g_audioAllocs.load() - before);
        CHECK(g_audioAllocs.load() - before == 2);  // the vector object and its buffer
    }
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    e.setMaxVoices(64);
    // every feature that runs on the audio thread
    const int chain[12] = {fx::Comp, fx::Drive, fx::Amp, fx::Cab, fx::Chorus, fx::Phaser, fx::Pitch, fx::Delay, fx::Reverb,
                           fx::Convo, fx::Slicer, fx::Limiter};
    for (int s = 0; s < 12; ++s) {
        e.setParam(P::fxSlot(s), (float)chain[s]);
        e.setParam(P::fxp(chain[s], 0), 1.0f);
    }
    e.setParam(P::fxChain(P::FxClockSync), 1.0f);
    e.setQuality(Quality::High);
    for (int k = 0; k < 6; ++k) {
        e.setPartParam(k, P::layer(1, P::LType), (float)(k % 2 ? LayerType::PCM : LayerType::WT));
        e.setPartParam(k, P::layer(2, P::LType), (float)LayerType::FM);
        e.setPartParam(k, P::layer(2, P::FmAlgo), 12.0f);
        e.setPartParam(k, P::FilterType, (float)(k % 4 + 6));  // ladder / comb / comb- / formant
        e.setPartParam(k, P::FilterDrive, 0.4f);
        e.setPartParam(k, P::VfxType, (float)(1 + k % 4));
        e.setPartParam(k, P::Unison, 2.0f);
        e.setPartParam(k, P::VoiceMode, (float)(k % 3));
        e.setPartParam(k, P::Glide, 0.05f);
        e.setPartParam(k, P::ArpOn, k == 5 ? 1.0f : 0.0f);
        e.setPartParam(k, P::insSlot(0), (float)(1 + k % fx::kNumInsertBlocks));
        e.setPartParam(k, P::insSlot(1), (float)(1 + (k + 5) % fx::kNumInsertBlocks));
        e.setPartSampleSet(k, 1, loopSet(220.0f + 30.0f * (float)k));
    }
    std::vector<float> L(2048), R(2048);
    long allocs = 0;
    uint32_t id = 1;
    for (int b = 0; b < 1000; ++b) {
        // host side (not counted): notes, controllers, clock, parameter edits, sample swaps
        Event on = Event::noteOn(id++, 36 + (b * 7) % 60, 0.8f);
        on.channel = (uint8_t)(b % 6);
        if (b % 3 == 0) e.sendEvent(on);
        Event off = Event::noteOff(id - 4, 0);
        off.channel = (uint8_t)((b + 1) % 6);
        if (b % 3 == 1) e.sendEvent(off);
        if (b % 2 == 0) e.sendEvent(Event::control(EventType::Clock, 0.0f, (int)ClockMsg::Tick));
        if (b % 50 == 0) e.sendEvent(Event::control(EventType::ModWheel, (float)(b % 100) / 100.0f));
        if (b % 70 == 0) e.setPartParam(b % 6, P::FilterCutoff, 300.0f + (float)(b % 5000));
        if (b % 300 == 150) e.setPartSampleSet((b / 300) % 6, 1, loopSet(300.0f));
        if (b % 400 == 399) e.sendEvent(Event::control(EventType::AllNotesOff, 0));
        if (b == 500) e.setMultiTimbral(false);
        if (b == 700) e.setMultiTimbral(true);
        allocs += audioBlock(e, L.data(), R.data(), b % 5 == 0 ? 2048 : 256 - (b % 7) * 16);  // odd sizes, > kMaxBlockSize
        e.collectGarbage();
    }
    INFO("allocations on the audio thread: %ld\n", allocs);
    CHECK(allocs == 0);
    CHECK(e.stats().peakVoices > 10);
}

TEST(realtime_parallel_render_never_allocates) {
    // multi-core rendering: the audio thread hands voices to the worker pool
    // without allocating (the pool and its buses are created beforehand)
    Engine e;
    e.prepare(48000);
    e.setRenderThreads(4);
    e.setMultiTimbral(true);
    for (int i = 0; i < 64; ++i) {
        Event on = Event::noteOn((uint32_t)(i + 1), 30 + i, 0.5f);
        on.channel = (uint8_t)(i % 8);
        e.sendEvent(on);
    }
    std::vector<float> L(512), R(512);
    long allocs = 0;
    for (int b = 0; b < 200; ++b) allocs += audioBlock(e, L.data(), R.data(), b % 2 ? 512 : 96);
    INFO("64 voices on 8 parts, 4 render threads: %ld allocations, %d voices active\n", allocs, e.stats().activeVoices);
    CHECK(allocs == 0);
    CHECK(e.stats().activeVoices == 64);
}

TEST(realtime_streamed_playback_never_allocates) {
    // disk streaming: claiming / releasing stream slots and reading the ring
    // buffers on the audio thread (and render helpers) does not allocate
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hsynth_tests" / "rt_stream";
    fs::create_directories(dir);
    {
        std::vector<float> l(96000), r(96000);
        for (size_t i = 0; i < l.size(); ++i) {
            l[i] = 0.3f * std::sin(0.02f * (float)i);
            r[i] = 0.3f * std::sin(0.03f * (float)i);
        }
        std::string err;
        writeWav((dir / "s.wav").generic_string(), l, &r, 48000, false, err);
        std::ofstream((dir / "s.sfz").generic_string()) << "<region> sample=s.wav lokey=40 hikey=70 pitch_keycenter=62\n";
    }
    const SampleLoadOptions saved = sampleLoadOptions();
    SampleLoadOptions o;
    o.mode = StreamMode::Always;
    o.preloadFrames = 4096;
    setSampleLoadOptions(o);
    std::string err;
    auto set = loadSampleSet((dir / "s.sfz").generic_string(), err);
    setSampleLoadOptions(saved);
    CHECK(set && set->streamedSamples() == 1);
    if (!set) return;
    Engine e;
    e.prepare(48000);
    e.setRenderThreads(2);
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    e.setSampleSet(0, std::move(set));
    std::vector<float> L(256), R(256);
    long allocs = 0;
    int streams = 0;
    for (int b = 0; b < 400; ++b) {
        if (b % 40 == 0)
            for (int k = 0; k < 12; ++k) e.sendEvent(Event::noteOn((uint32_t)(b * 16 + k + 1), 48 + k, 0.6f));
        if (b % 40 == 20)
            for (int k = 0; k < 12; ++k) e.sendEvent(Event::noteOff((uint32_t)((b - 20) * 16 + k + 1), 48 + k));
        allocs += audioBlock(e, L.data(), R.data(), 256);
        streams = std::max(streams, e.stats().streamVoices);
    }
    INFO("streamed notes on and off: %ld allocations, up to %d streams\n", allocs, streams);
    CHECK(allocs == 0);
    CHECK(streams >= 12);
}

TEST(realtime_silent_idle_fast_path) {
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    std::vector<float> L(48), R(48);
    long allocs = 0;
    // Play a short note, release it, let tails settle, and verify silentIdle_ engages.
    e.sendEvent(Event::noteOn(1, 60, 0.8f));
    for (int b = 0; b < 50; ++b) allocs += audioBlock(e, L.data(), R.data(), 48);
    CHECK(!e.isSilentIdle());
    e.sendEvent(Event::noteOff(1, 60));
    // Run enough blocks for envelope + default reverb/delay + 4 s rack idle + 0.5 s engine idle
    int settledBlocks = 0;
    for (int b = 0; b < 12000 && !e.isSilentIdle(); ++b) {
        allocs += audioBlock(e, L.data(), R.data(), 48);
        settledBlocks = b + 1;
    }
    INFO("silent-idle engaged after %d blocks (%.2f s)\n", settledBlocks, settledBlocks * 48.0 / 48000.0);
    CHECK(e.isSilentIdle());
    // While silent-idle, output is exact digital silence and 0 allocs.
    float silentMax = 0.0f;
    for (int b = 0; b < 100; ++b) {
        allocs += audioBlock(e, L.data(), R.data(), 48);
        for (int i = 0; i < 48; ++i) silentMax = std::max(silentMax, std::max(std::fabs(L[i]), std::fabs(R[i])));
    }
    CHECK(silentMax == 0.0f);
    // Incoming NoteOn wakes on the EXACT SAME 48-sample block with audible audio!
    e.sendEvent(Event::noteOn(2, 64, 0.9f));
    allocs += audioBlock(e, L.data(), R.data(), 48);
    CHECK(!e.isSilentIdle());
    float pk = 0.0f;
    for (int i = 0; i < 48; ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
    INFO("silent-idle wake peak on first 1 ms block: %.4f (allocs=%ld)\n", pk, allocs);
    CHECK(pk > 0.01f);
    CHECK(allocs == 0);
}

TEST(realtime_phase1_sst_master_never_allocates) {
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    e.setMaxVoices(64);
    e.setSstEnabled(true);
    e.setSstTimeMs(120.0f);

    auto setupMulti = [&](MultiPreset& m, float pitchOffset, float delayMix) {
        m.global.params[(size_t)P::fxSlot(0)] = (float)fx::Delay;
        m.global.params[(size_t)P::fxSlot(1)] = (float)fx::Reverb;
        m.global.params[(size_t)P::fxp(fx::Delay, fx::delay::On)] = 1.0f;
        m.global.params[(size_t)P::fxp(fx::Delay, fx::delay::Mix)] = delayMix;
        m.global.params[(size_t)P::fxp(fx::Reverb, fx::reverb::On)] = 1.0f;
        m.global.params[(size_t)P::fxp(fx::Reverb, fx::reverb::Mix)] = 0.35f;
        m.global.params[(size_t)P::fxChain(P::SendALevel)] = 0.5f;
        m.global.params[(size_t)P::fxChain(P::SendBLevel)] = 0.5f;
        for (int k = 0; k < 4; ++k) {
            m.parts[k].params[(size_t)P::layer(0, P::LType)] = (float)LayerType::VA;
            m.parts[k].params[(size_t)P::layer(0, P::LCoarse)] = pitchOffset;
            m.parts[k].params[(size_t)P::layer(1, P::LType)] = (float)(k % 2 ? LayerType::FM : LayerType::WT);
            m.parts[k].params[(size_t)P::SendA] = 0.5f;
            m.parts[k].params[(size_t)P::SendB] = 0.5f;
            m.parts[k].params[(size_t)P::AmpRelease] = 0.6f;
            m.mix[k][Mix::Channel] = 1.0f;
            m.mix[k][Mix::KeyLow] = (float)(36 + k * 12);
            m.mix[k][Mix::KeyHigh] = (float)(52 + k * 12);
            m.mix[k][Mix::KeyFadeLow] = 6.0f;
            m.mix[k][Mix::KeyFadeHigh] = 6.0f;
        }
    };

    MultiPreset mA, mB;
    setupMulti(mA, 0.0f, 0.4f);
    setupMulti(mB, 7.0f, 0.25f);
    std::vector<std::string> warnings;
    CHECK(applyMulti(e, mA, "", warnings));

    // Also keep PCM sample sets on parts 0 and 1 so held_[] + releaseHeld() run on SST switches
    e.setPartParam(0, P::layer(2, P::LType), (float)LayerType::PCM);
    e.setPartSampleSet(0, 2, loopSet(220.0f));
    e.setPartParam(1, P::layer(2, P::LType), (float)LayerType::PCM);
    e.setPartSampleSet(1, 2, loopSet(330.0f));

    MasterStageConfig ms;
    ms.bypass = false;
    ms.eqEnabled = true;
    ms.compEnabled = true;
    for (int i = 0; i < 5; ++i) {
        ms.bands[i].enabled = true;
        ms.bands[i].gainDb = (i % 2 == 0) ? 3.0f : -2.5f;
    }
    ms.comp.thresholdDb = -20.0f;
    ms.comp.ratio = 3.0f;
    e.setMasterStage(ms);

    std::vector<float> L(512), R(512);
    long allocs = 0;
    uint32_t id = 1;
    uint32_t lastSerial = 0;
    for (int b = 0; b < 600; ++b) {
        if (b % 4 == 0) {
            Event on = Event::noteOn(id++, 40 + (b * 5) % 48, 0.85f);
            on.channel = 0;
            e.sendEvent(on);
        }
        if (b % 4 == 2 && id > 5) {
            Event off = Event::noteOff(id - 4, 0);
            off.channel = 0;
            e.sendEvent(off);
        }
        // Trigger SST multi switches while voices, split crossfades, and FX/send tails are active
        if (b == 60 || b == 140 || b == 240 || b == 360 || b == 480) {
            MultiPreset& target = ((b / 60) % 2) ? mB : mA;
            target.parts[0].params[(size_t)P::layer(2, P::LType)] = (float)LayerType::PCM;
            if (applyMultiSst(e, target, "", warnings, &lastSerial)) {
                e.setPartSampleSet(0, 2, loopSet(240.0f + (float)(b % 50)));
            }
        }
        // Update MasterStage mid-stream to exercise lock-free mailbox sync + crossfade
        if (b % 90 == 30) {
            ms.bands[2].gainDb = (b % 180 == 30) ? 4.5f : -3.0f;
            e.setMasterStage(ms);
        }
        allocs += audioBlock(e, L.data(), R.data(), b % 3 == 0 ? 512 : 128);
        e.collectGarbage();
    }
    INFO("Phase 1 SST + Split Crossfade + MasterStage allocations on audio thread: %ld\n", allocs);
    CHECK(allocs == 0);
    CHECK(e.stats().peakVoices > 4);
}

TEST(realtime_phase2_utils_tap_pedals_never_allocates) {
    Engine e;
    e.prepare(48000);
    e.setParam(P::fxSlot(0), (float)fx::Rotary);
    e.setParam(P::fxChain(P::FxTempo), 120.0f);

    // Enable Metronome, Tuner, AudioTap, PerfOffsets, and Pedals/Footswitches
    MetronomeConfig mc;
    mc.enabled = true;
    mc.numerator = 4;
    mc.denominator = 4;
    mc.subdivision = 2;
    mc.countInBars = 0;
    mc.route = 0;
    mc.levelDb = -12.0f;
    mc.accentDb = 4.0f;
    e.setMetronomeConfig(mc);

    e.tuner().configure(440.0f, false);
    e.tuner().setEnabled(true);
    e.audioTap().setEnabled(true);
    e.setPerfOffsets(3, -2.0f);

    PerformanceInputConfig pic;
    pic.sustainInverted = true;
    pic.velocityCurve = -0.5f;
    pic.aftertouchCurve = 0.5f;
    pic.expMin = 0.1f;
    pic.expMax = 0.9f;
    pic.footswitches[0] = {66, 1, false};  // SceneNext (HostAction)
    pic.footswitches[1] = {67, 5, false};  // RotaryToggle
    pic.footswitches[2] = {68, 6, false};  // LooperTap
    pic.footswitches[3] = {69, 7, false};  // TapTempo
    e.setPerformanceInputConfig(pic);

    constexpr int kMaxBlock = 512;
    std::vector<float> L(kMaxBlock), R(kMaxBlock), inL(kMaxBlock), inR(kMaxBlock);
    std::vector<float> wave(256), spec(64);
    double phase = 0.0;
    const double inc = 2.0 * (double)kPi * 440.0 / 48000.0;

    long allocs = 0;
    uint32_t id = 1;
    int hostActions = 0;
    for (int b = 0; b < 400; ++b) {
        const int n = (b % 3 == 0) ? 512 : (b % 3 == 1 ? 192 : 96);
        for (int i = 0; i < n; ++i) {
            inL[(size_t)i] = inR[(size_t)i] = 0.35f * (float)std::sin(phase);
            phase += inc;
        }
        if (b % 8 == 0) {
            Event on = Event::noteOn(id++, 48 + (b % 24), 0.75f);
            e.sendEvent(on);
        }
        if (b % 8 == 4 && id > 1) {
            e.sendEvent(Event::noteOff(id - 1, 48 + ((b - 4) % 24)));
        }
        if (b % 16 == 0) {
            Event sus = Event::control(EventType::Sustain, (b % 32 == 0) ? 0.0f : 1.0f);
            sus.channel = 0;
            sus.rawCc = 64;
            sus.rawSustainValue = sus.value;
            e.sendEvent(sus);
        }
        if (b % 20 == 5) {
            Event at = Event::control(EventType::ChannelPressure, (float)(b % 100) / 100.0f);
            e.sendEvent(at);
        }
        if (b % 25 == 10) {
            const int cc = 66 + ((b / 25) % 4);
            Event fs = Event::control(EventType::Controller, ((b / 25) % 2 == 0) ? 1.0f : 0.0f, cc);
            fs.channel = 0;
            fs.rawCc = (uint8_t)cc;
            e.sendEvent(fs);
        }
        if (b % 40 == 20) {
            e.setPerfOffsets(((b / 40) % 5) - 2, ((b / 40) % 2) ? -3.0f : 0.0f);
            e.tuner().configure(440.0f, (b / 40) % 4 == 3);
        }
        if (b % 50 == 15) {
            mc.route = (uint8_t)((b / 50) % 3);
            e.setMetronomeConfig(mc);
        }

        // Feed input buffer and run process() under the allocation detector
        const long before = g_audioAllocs.load();
        t_inAudio = true;
        e.setInputBuffer(inL.data(), inR.data(), (b % 2 == 0) ? n : -1);
        e.process(L.data(), R.data(), n);
        t_inAudio = false;
        allocs += g_audioAllocs.load() - before;

        if (b % 10 == 9) {
            TunerReading tr;
            (void)e.tuner().analyze(tr);
            (void)e.audioTap().readFrame(wave.data(), (int)wave.size(), spec.data(), (int)spec.size());
        }
        while (e.takeHostAction() > 0) ++hostActions;
        e.collectGarbage();
    }
    INFO("Phase 2 Metronome + Tuner + AudioTap + PerfOffsets + Pedals allocs: %ld (hostActions=%d, clickBeat=%d)\n",
         allocs, hostActions, e.clickBeat());
    CHECK(allocs == 0);
    CHECK(hostActions > 0);
    CHECK(e.clickBeat() >= 0);
}

TEST(realtime_phase3_physical_morph_rhythm_never_allocates) {
    Engine e;
    e.prepare(48000.0f);
    e.setMultiTimbral(true);
    e.setMaxVoices(64);
    e.setParam(P::fxChain(P::FxTempo), 140.0f);

    // Part 0: Physical (string + modal + tine), Vintage + PanSpread, Piano realism, Pattern Arp + Strum, Morph A<->B
    e.setPartMix(0, Mix::Channel, 1.0f);
    e.setPartParam(0, P::layer(0, P::LType), (float)LayerType::Physical);
    e.setPartParam(0, P::layer(0, P::PhysModel), 0.0f);  // string
    e.setPartParam(0, P::layer(0, P::PhysExciter), 0.0f);
    e.setPartParam(0, P::layer(1, P::LType), (float)LayerType::Physical);
    e.setPartParam(0, P::layer(1, P::PhysModel), 1.0f);  // modal
    e.setPartParam(0, P::layer(2, P::LType), (float)LayerType::Physical);
    e.setPartParam(0, P::layer(2, P::PhysModel), 2.0f);  // tine
    e.setPartParam(0, P::FilterType, (float)FilterType::LP24);
    e.setPartParam(0, P::FilterCutoff, 1200.0f);
    e.setPartParam(0, P::FilterReso, 0.25f);
    e.setPartParam(0, P::VoiceVintage, 0.8f);
    e.setPartParam(0, P::VoiceVintageRate, 0.5f);
    e.setPartParam(0, P::VoicePanSpread, 0.7f);
    e.setPartParam(0, P::PianoPedalNoise, 0.8f);
    e.setPartParam(0, P::PianoSoftRelease, 0.7f);
    e.setPartParam(0, P::PianoDynComp, 0.6f);
    e.setPartParam(0, P::PianoLid, 0.5f);
    e.setPartParam(0, P::PianoHalfDamp, 0.6f);

    // Capture Morph A <-> B including Physical brightness and Filter cutoff/reso
    e.morphSetA(0);
    e.setPartParam(0, P::FilterCutoff, 5200.0f);
    e.setPartParam(0, P::FilterReso, 0.65f);
    e.setPartParam(0, P::layer(0, P::PhysBright), 0.9f);
    CHECK(e.morphCaptureB(0) == 3);
    e.setPartParam(0, P::PerfMorph, 0.0f);

    // Pattern Arp (with rest, tie, accent, octave, chord) + Strum on chord steps
    e.setPartParam(0, P::ArpOn, 1.0f);
    e.setPartParam(0, P::ArpMode, (float)ArpMode::Pattern);
    e.setPartParam(0, P::ArpRate, 10.0f);
    e.setPartParam(0, P::StrumOn, 1.0f);
    e.setPartParam(0, P::StrumOrder, 3.0f);  // alternate
    e.setPartParam(0, P::StrumWindow, 8.0f);
    e.setPartParam(0, P::StrumSpacing, 10.0f);
    e.setPartParam(0, P::StrumJitter, 1.5f);
    Json pat = Json::object(), steps = Json::array();
    pat.set("length", 4);
    for (int i = 0; i < 4; ++i) {
        Json s = Json::object();
        s.set("gate", i == 1 ? 2 : (i == 2 ? 0 : 1));
        s.set("accent", i == 3 ? 140 : 100);
        s.set("oct", i == 3 ? 1 : 0);
        s.set("chord", i == 3);
        steps.push(std::move(s));
    }
    pat.set("steps", std::move(steps));
    CHECK(e.setArpPatternJson(0, pat));

    // Part 1: Direct Strum chord notes on channel 2 (0-indexed 1)
    e.setPartMix(1, Mix::Channel, 2.0f);
    e.setPartParam(1, P::layer(0, P::LType), (float)LayerType::Physical);
    e.setPartParam(1, P::layer(0, P::PhysModel), 3.0f);  // reed
    e.setPartParam(1, P::layer(0, P::PhysExciter), 3.0f);  // bow
    e.setPartParam(1, P::VoiceVintage, 0.5f);
    e.setPartParam(1, P::StrumOn, 1.0f);
    e.setPartParam(1, P::StrumSpacing, 12.0f);

    // Rhythm pattern + fill
    Json rhy = Json::object();
    rhy.set("enabled", true);
    rhy.set("pattern", 3);
    rhy.set("levelDb", -6.0);
    rhy.set("swing", 0.2);
    rhy.set("fill", true);
    CHECK(e.setRhythmJson(rhy));

    // Hold chord on Part 0 for Pattern Arp
    for (int k = 0; k < 3; ++k) {
        Event on = Event::noteOn((uint32_t)(1 + k), 48 + k * 4, 0.8f);
        on.channel = 0;
        e.sendEvent(on);
    }

    std::vector<float> L(512), R(512);
    long allocs = 0;
    double maxPeak = 0.0;
    uint32_t id = 100;
    for (int b = 0; b < 500; ++b) {
        // Sweep Morph A <-> B
        const float morphPos = 0.5f - 0.5f * std::cos(0.05f * (float)b);
        e.setPartParam(0, P::PerfMorph, morphPos);

        // CC64 pedal edges (down, half, up) for Piano Realism
        if (b % 25 == 0) {
            const float val = (b % 75 == 0) ? 1.0f : (b % 75 == 25 ? 0.45f : 0.0f);
            Event sus = Event::control(EventType::Sustain, val >= 0.5f ? 1.0f : 0.0f);
            sus.channel = 0;
            sus.rawCc = 64;
            sus.rawSustainValue = val;
            e.sendEvent(sus);
        }
        // Trigger and release direct Strummed chords on Part 1
        if (b % 30 == 5) {
            for (int k = 0; k < 3; ++k) {
                Event on = Event::noteOn(id++, 60 + k * 4, 0.75f);
                on.channel = 1;
                e.sendEvent(on);
            }
        }
        if (b % 30 == 20 && id > 103) {
            for (int k = 0; k < 3; ++k) {
                Event off = Event::noteOff(id - 3 + k, 60 + k * 4);
                off.channel = 1;
                e.sendEvent(off);
            }
        }
        if (b % 60 == 40) e.setRhythmFill((b / 60) % 2 == 0);

        const int n = (b % 3 == 0) ? 512 : (b % 3 == 1 ? 192 : 96);
        allocs += audioBlock(e, L.data(), R.data(), n);
        for (int i = 0; i < n; ++i) {
            CHECK(std::isfinite(L[(size_t)i]) && std::isfinite(R[(size_t)i]));
            maxPeak = std::max(maxPeak, (double)std::max(std::fabs(L[(size_t)i]), std::fabs(R[(size_t)i])));
        }
        e.collectGarbage();
    }
    INFO("Phase 3 Physical+Vintage+Morph+Piano+PatternArp+Strum+Rhythm allocs: %ld (peakVoices=%d, maxPeak=%.4f)\n",
         allocs, e.stats().peakVoices, maxPeak);
    CHECK(allocs == 0);
    CHECK(e.stats().peakVoices > 2);
    CHECK(maxPeak > 0.05);
}

TEST(realtime_phase4_workstation_never_allocates) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hsynth_tests" / "rt_phase4";
    fs::create_directories(dir);
    auto writeSine = [&](const std::string& name, double sec, float freqL, float freqR) {
        const size_t n = (size_t)(sec * 48000.0);
        std::vector<float> l(n, 0.0f), r(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            const double t = double(i) / 48000.0;
            l[i] = 0.4f * float(std::sin(2.0 * double(kPi) * freqL * t));
            r[i] = 0.4f * float(std::sin(2.0 * double(kPi) * freqR * t));
        }
        std::string err;
        const std::string p = (dir / name).generic_string();
        CHECK(writeWav(p, l, &r, 48000, false, err));
        return p;
    };
    const std::string backingPath = writeSine("backing.wav", 1.5, 440.0, 660.0);
    const std::string pad0Path = writeSine("pad0.wav", 0.25, 220.0, 220.0);
    const std::string pad1Path = writeSine("pad1.wav", 0.25, 330.0, 330.0);

    Engine e;
    e.prepare(48000.0f);
    e.setMultiTimbral(true);
    e.setMaxVoices(64);

    // 4. External MIDI Zone Master (Part 0 EXT, Part 1 BOTH)
    MultiPreset m;
    m.parts[0].params[(size_t)P::layer(0, P::LType)] = (float)LayerType::VA;
    m.parts[1].params[(size_t)P::layer(0, P::LType)] = (float)LayerType::VA;
    m.mix[0][Mix::Channel] = 1.0f;
    m.mix[0][Mix::ZoneMode] = 1.0f;  // EXT
    m.mix[0][Mix::ZoneOutPort] = 1.0f;
    m.mix[0][Mix::ZoneOutChannel] = 2.0f;
    m.mix[0][Mix::ZoneBankMsb] = 10.0f;
    m.mix[0][Mix::ZoneBankLsb] = 20.0f;
    m.mix[0][Mix::ZoneProgram] = 5.0f;
    m.mix[1][Mix::Channel] = 2.0f;
    m.mix[1][Mix::ZoneMode] = 2.0f;  // BOTH
    m.mix[1][Mix::ZoneOutPort] = 1.0f;
    m.mix[1][Mix::ZoneOutChannel] = 3.0f;
    std::vector<std::string> warnings;
    CHECK(applyMulti(e, m, "", warnings));

    // 1. Backing Audio (with FX routing initialized on host before audio loop)
    e.setParam(P::fxSlot(0), (float)fx::Delay);
    e.setParam(P::fxp(fx::Delay, fx::delay::On), 1.0f);
    e.setBackingRoute(false);
    CHECK(e.backingPlayer().load(backingPath).find("ok")->boolean());
    e.backingPlayer().setLoop(0.1, 0.6, true);
    e.backingPlayer().setSpeed(1.25f);
    e.backingPlayer().setPitch(2.0f);
    e.backingPlayer().play(true);

    // 2. 16-Pad Sampler (OneShot + Loop + Gate + Choke)
    CHECK(e.padSampler().load(0, pad0Path).find("ok")->boolean());
    CHECK(e.padSampler().load(1, pad1Path).find("ok")->boolean());
    CHECK(e.padSampler().load(2, pad0Path).find("ok")->boolean());
    CHECK(e.padSampler().load(3, pad1Path).find("ok")->boolean());
    e.padSampler().setup(0, 0.8f, 0.0f, 0, 1, 1, 0.0f, 1.0f);
    e.padSampler().setup(1, 0.8f, 0.0f, 0, 1, 1, 0.0f, 1.0f);
    e.padSampler().setup(2, 0.8f, 0.0f, 2, 0, 1, 0.0f, 1.0f);
    e.padSampler().setup(3, 0.8f, 0.0f, 1, 0, 1, 0.0f, 1.0f);
    e.padSampler().trigger(3, 1.0f);
    e.padSampler().trigger(0, 0.9f);

    // 3. Live Sampling capture (source 0 first, then source 1 + audition)
    CHECK(e.liveSampler().start(0, 5.0f, -120.0f));

    constexpr int kMaxBlk = 512;
    std::vector<float> L(kMaxBlk), R(kMaxBlk), inL(kMaxBlk), inR(kMaxBlk);
    double phase = 0.0;
    const double inc = 2.0 * double(kPi) * 220.0 / 48000.0;

    long allocs = 0;
    double maxPeak = 0.0;
    uint32_t id = 1;
    for (int b = 0; b < 300; ++b) {
        const int n = (b % 3 == 0) ? 512 : (b % 3 == 1 ? 192 : 96);
        for (int i = 0; i < n; ++i) {
            inL[size_t(i)] = inR[size_t(i)] = 0.3f * float(std::sin(phase));
            phase += inc;
        }
        if (b == 150) {
            (void)e.liveSampler().stop();
            e.liveSampler().audition(true, true, 0, 4096);
            e.liveSampler().discard();
            CHECK(e.liveSampler().start(1, 5.0f, -120.0f));
        }
        if (b % 10 == 0) {
            Event on0 = Event::noteOn(id++, 48 + (b % 24), 0.8f);
            on0.channel = 0;
            e.sendEvent(on0);
            Event on1 = Event::noteOn(id++, 60 + (b % 12), 0.7f);
            on1.channel = 1;
            e.sendEvent(on1);
        }
        if (b % 10 == 5 && id > 2) {
            Event off0 = Event::noteOff(id - 2, 48 + ((b - 5) % 24));
            off0.channel = 0;
            e.sendEvent(off0);
            Event off1 = Event::noteOff(id - 1, 60 + ((b - 5) % 12));
            off1.channel = 1;
            e.sendEvent(off1);
        }
        if (b % 15 == 2) e.padSampler().trigger(b % 2, 0.85f);
        if (b % 20 == 4) e.padSampler().trigger(2, 0.8f);
        if (b % 20 == 12) e.padSampler().release(2);
        if (b % 30 == 7) {
            Event bend = Event::control(EventType::PitchBend, 0.25f);
            bend.channel = 0;
            e.sendEvent(bend);
        }
        if (b == 120 || b == 240) e.externalMidi().requestPanic(1);

        const long before = g_audioAllocs.load();
        t_inAudio = true;
        e.setInputBuffer(inL.data(), inR.data(), n);
        e.process(L.data(), R.data(), n);
        t_inAudio = false;
        allocs += g_audioAllocs.load() - before;

        for (int i = 0; i < n; ++i) {
            CHECK(std::isfinite(L[size_t(i)]) && std::isfinite(R[size_t(i)]));
            maxPeak = std::max(maxPeak, double(std::max(std::fabs(L[size_t(i)]), std::fabs(R[size_t(i)]))));
        }
        (void)e.externalMidi().next(0);
        e.collectGarbage();
    }
    e.backingPlayer().play(false);
    e.padSampler().stopAll();
    (void)e.liveSampler().stop();
    e.liveSampler().discard();

    INFO("Phase 4 Backing+Pads+LiveSample+ExtMidiZone allocs: %ld (maxPeak=%.4f)\n", allocs, maxPeak);
    CHECK(allocs == 0);
    CHECK(maxPeak > 0.05);
}

TEST(realtime_phase5_stems_and_npu_morph_never_allocates) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hsynth_tests" / "rt_phase5";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto writeSine = [&](const std::string& name, double sec, float freq) {
        const size_t n = (size_t)(sec * 48000.0);
        std::vector<float> l(n, 0.0f), r(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            const double t = double(i) / 48000.0;
            l[i] = r[i] = 0.20f * float(std::sin(2.0 * double(kPi) * freq * t));
        }
        std::string err;
        const std::string p = (dir / name).generic_string();
        CHECK(writeWav(p, l, &r, 48000, false, err));
        return p;
    };

    const std::string stemPaths[4] = {
        writeSine("vocals.wav", 1.5, 220.0f),
        writeSine("drums.wav",  1.5, 440.0f),
        writeSine("bass.wav",   1.5, 110.0f),
        writeSine("other.wav",  1.5, 880.0f),
    };

    Engine e;
    e.prepare(48000.0f);
    e.setMultiTimbral(false);
    e.setMaxVoices(64);
    e.setParam(P::MasterVolume, 0.9f);
    e.setParam(P::Limiter, 0.0f);
    e.setParam(P::FilterType, (float)FilterType::LP24);
    e.setParam(P::layer(0, P::LType), (float)LayerType::VA);
    e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Saw);
    e.setParam(P::layer(0, P::LLevel), 0.45f);
    e.setParam(P::fxSlot(0), (float)fx::Delay);
    e.setParam(P::fxp(fx::Delay, fx::delay::On), 1.0f);

    // 1. NpuMorph: capture 4 corners and compile off the audio thread
    auto& morph = e.npuMorph();
    const float corners[4][4] = {
        {0.0f, 0.0f, 400.0f,  0.15f},
        {1.0f, 0.0f, 3600.0f, 0.45f},
        {0.0f, 1.0f, 900.0f,  0.80f},
        {1.0f, 1.0f, 8400.0f, 0.30f},
    };
    for (int s = 0; s < 4; ++s) {
        e.setPartParam(0, P::FilterCutoff, corners[s][2]);
        e.setPartParam(0, P::FilterReso, corners[s][3]);
        e.setPartParam(0, P::Macro1, 0.2f * float(s + 1));
        CHECK(morph.captureAnchor(e, 0, s, corners[s][0], corners[s][1], "C" + std::to_string(s)));
    }
    CHECK(morph.compile().find("compiled")->boolean());

    // 2. 4-stem BackingPlayer load & start
    e.setBackingRoute(false);
    CHECK(e.backingPlayer().loadStems(stemPaths).find("ok")->boolean());
    CHECK(e.backingPlayer().isStemsLoaded());
    e.backingPlayer().setLoop(0.05, 0.85, true);
    e.backingPlayer().play(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> L(512), R(512);
    long allocs = 0;
    double maxPeak = 0.0;
    uint32_t id = 1;
    for (int b = 0; b < 250; ++b) {
        if (b % 12 == 0) e.sendEvent(Event::noteOn(id++, 48 + (b % 24), 0.7f));
        if (b % 12 == 6 && id > 1) e.sendEvent(Event::noteOff(id - 1, 48 + ((b - 6) % 24)));

        // Control-thread NpuMorph live evaluation -> engine parameter writes
        if (b % 4 == 0) {
            const float x = 0.5f + 0.45f * std::sin(0.07f * float(b));
            const float y = 0.5f + 0.45f * std::cos(0.05f * float(b));
            CHECK(morph.evaluate(e, 0, x, y) >= 0.0f);
        }

        // Control-thread 4-stem mix/solo/mute updates
        if (b % 10 == 0) {
            const int stem = (b / 10) % 4;
            const bool solo = (b / 40) % 2 == 1;
            const bool mute = !solo && ((b / 10) % 3 == 0);
            const float gain = 0.4f + 0.2f * float(stem);
            e.backingPlayer().setStemMix(stem, gain, mute, solo);
        }
        if (b == 125) e.setBackingRoute(true);

        const int n = (b % 3 == 0) ? 512 : (b % 3 == 1 ? 192 : 96);
        allocs += audioBlock(e, L.data(), R.data(), n);
        for (int i = 0; i < n; ++i) {
            CHECK(std::isfinite(L[(size_t)i]) && std::isfinite(R[(size_t)i]));
            maxPeak = std::max(maxPeak, double(std::max(std::fabs(L[(size_t)i]), std::fabs(R[(size_t)i]))));
        }
        e.collectGarbage();
    }
    e.backingPlayer().play(false);

    INFO("Phase 5 4-Stem Backing + NpuMorph allocs: %ld (maxPeak=%.4f)\n", allocs, maxPeak);
    CHECK(allocs == 0);
    CHECK(maxPeak > 0.05);
}
