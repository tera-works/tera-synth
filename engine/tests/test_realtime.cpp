// Real-time contract (engine.h): process() never allocates. Global operator
// new is replaced in the test binary; while a test's "audio thread" is inside
// process(), every allocation is counted.
#include <atomic>
#include <cstdlib>
#include <new>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "hsynth/wav.h"

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
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
