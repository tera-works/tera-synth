// Micro benchmark: CPU cost per voice for each layer type / feature.
// usage: hsynth_bench [voices=32] [seconds=3] [render threads=1, 0 = auto] [name filter]
// With several render threads, "load" is the wall time of the audio thread
// (what decides drop-outs), not the total CPU time.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/sample_set.h"

using namespace hsynth;

static int g_threads = 1;
static const char* g_filter = nullptr;
static bool skip(const char* name) { return g_filter && !std::strstr(name, g_filter); }

static double bench(const char* name, int voices, double seconds, const std::function<void(Engine&)>& setup) {
    if (skip(name)) return 0.0;
    Engine e;
    e.prepare(48000);
    e.setRenderThreads(g_threads);
    e.setMaxVoices(kMaxVoices);
    e.setParam(P::AmpSustain, 1.0f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    setup(e);
    for (int i = 0; i < voices; ++i) e.sendEvent(Event::noteOn((uint32_t)i + 1, 36 + (i % 64), 0.7f));
    std::vector<float> L(128), R(128);
    e.process(L.data(), R.data(), 128);
    const int blocks = (int)(48000 * seconds / 128);
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) e.process(L.data(), R.data(), 128);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double nsPerVoiceSample = sec * 1e9 / ((double)blocks * 128 * voices);
    std::printf("%-28s load %6.1f%%  %7.1f ns/voice-sample  (active %d)\n", name, sec / seconds * 100.0, nsPerVoiceSample,
                e.stats().activeVoices);
    return sec;
}

// Multi-timbral: `parts` parts with `perPart` voices each, one MIDI channel per
// part, every part with its own copy of the VA saw x2 / LP24 patch.
static double benchMulti(const char* name, int parts, int perPart, double seconds, bool inserts = false) {
    if (skip(name)) return 0.0;
    Engine e;
    e.prepare(48000);
    e.setRenderThreads(g_threads);
    e.setMaxVoices(kMaxVoices);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setMultiTimbral(true);
    for (int k = 0; k < parts; ++k) {
        e.setPartParam(k, P::AmpSustain, 1.0f);
        e.setPartParam(k, P::FilterCutoff, 800.0f + 200.0f * (float)k);
        e.setPartMix(k, Mix::Pan, -1.0f + 2.0f * (float)k / (float)std::max(1, parts - 1));
        e.setPartMix(k, Mix::FxSend, k % 2 ? 1.0f : 0.5f);
        if (inserts) {  // two inserts per part: comp + (chorus | phaser | drive | peq)
            const int second[4] = {fx::Chorus, fx::Phaser, fx::Drive, fx::Peq};
            e.setPartParam(k, P::insSlot(0), (float)(fx::insertIndex(fx::Comp) + 1));
            e.setPartParam(k, P::insSlot(1), (float)(fx::insertIndex(second[k % 4]) + 1));
        }
        for (int i = 0; i < perPart; ++i) {
            Event ev = Event::noteOn((uint32_t)(k * 16 + i + 1), 40 + 3 * i + k, 0.7f);
            ev.channel = (uint8_t)k;
            e.sendEvent(ev);
        }
    }
    std::vector<float> L(128), R(128);
    e.process(L.data(), R.data(), 128);
    const int blocks = (int)(48000 * seconds / 128);
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) e.process(L.data(), R.data(), 128);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int voices = parts * perPart;
    std::printf("%-28s load %6.1f%%  %7.1f ns/voice-sample  (active %d)\n", name, sec / seconds * 100.0,
                sec * 1e9 / ((double)blocks * 128 * voices), e.stats().activeVoices);
    return sec;
}

// One looping 2 s stereo sample (bright saw), recorded pitch = root.
static void pcmLayer(Engine& e, int root) {
    auto set = std::make_unique<SampleSet>();
    auto smp = std::make_unique<Sample>();
    smp->channels = 2;
    smp->sampleRate = 48000;
    smp->frames = 96000;
    for (int c = 0; c < 2; ++c) {
        smp->data[c].assign(96000 + Sample::kPadFrames, 0.0f);
        for (int i = 0; i < 96000; ++i) smp->data[c][(size_t)i] = 0.5f * (float)((i * (c ? 7 : 5)) % 400) / 400.0f - 0.25f;
    }
    smp->loopStart = 1000;
    smp->loopEnd = 95000;
    Region r;
    r.sample = smp.get();
    r.keycenter = root;
    r.loopMode = LoopMode::Continuous;
    r.loopStart = smp->loopStart;
    r.loopEnd = smp->loopEnd;
    set->regions.push_back(r);
    set->samples.push_back(std::move(smp));
    set->prepareMips();
    e.setSampleSet(0, std::move(set));
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
}

int main(int argc, char** argv) {
    const int voices = argc > 1 ? std::atoi(argv[1]) : 32;
    const double seconds = argc > 2 ? std::atof(argv[2]) : 3.0;
    if (argc > 3) g_threads = std::atoi(argv[3]);
    if (argc > 4) g_filter = argv[4];
    {
        Engine probe;
        probe.setRenderThreads(g_threads);
        std::printf("%d voices, %.1f s audio @48k, block 128, %d render thread(s)\n", voices, seconds, probe.renderThreads());
    }
    bench("empty (all layers off)", voices, seconds, [](Engine& e) { e.setParam(P::layer(0, P::LType), 0); });
    bench("VA saw x2, filter off", voices, seconds, [](Engine& e) { e.setParam(P::FilterType, 0); });
    bench("VA saw x2, LP24", voices, seconds, [](Engine&) {});
    bench("VA saw x2, LP24 + drive", voices, seconds, [](Engine& e) { e.setParam(P::FilterDrive, 0.5f); });
    bench("VA saw x2, LP24 + drive (High)", voices, seconds, [](Engine& e) {
        e.setParam(P::FilterDrive, 0.5f);
        e.setQuality(Quality::High);
    });
    bench("VA saw x2, ladder + drive", voices, seconds, [](Engine& e) {
        e.setParam(P::FilterType, (float)FilterType::Ladder);
        e.setParam(P::FilterDrive, 0.5f);
    });
    bench("VA saw x2, comb+", voices, seconds, [](Engine& e) { e.setParam(P::FilterType, (float)FilterType::CombPlus); });
    bench("VA saw x2, formant", voices, seconds, [](Engine& e) { e.setParam(P::FilterType, (float)FilterType::Formant); });
    bench("VA saw x2, LP24 + vfx ring", voices, seconds, [](Engine& e) { e.setParam(P::VfxType, (float)VfxType::Ring); });
    bench("VA saw x2, LP24 + vfx saturate", voices, seconds, [](Engine& e) { e.setParam(P::VfxType, (float)VfxType::Saturate); });
    bench("  same, High (ADAA)", voices, seconds, [](Engine& e) {
        e.setParam(P::VfxType, (float)VfxType::Saturate);
        e.setQuality(Quality::High);
    });
    bench("VA saw x2, LP24 + vfx crush", voices, seconds, [](Engine& e) { e.setParam(P::VfxType, (float)VfxType::Crush); });
    bench("FM 4-op, filter off", voices, seconds, [](Engine& e) {
        e.setParam(P::FilterType, 0);
        e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
    });
    bench("FM 6-op (DX 1), filter off", voices, seconds, [](Engine& e) {
        e.setParam(P::FilterType, 0);
        e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
        e.setParam(P::layer(0, P::FmAlgo), 8.0f);
    });
    bench("FM 8-op (7>1 fan-in), filter off", voices, seconds, [](Engine& e) {
        e.setParam(P::FilterType, 0);
        e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
        e.setParam(P::layer(0, P::FmAlgo), 32.0f);  // "33: 8op (8,7,6,5,4,3,2)>1"
    });
    bench("FM 8-op, non-sine waves", voices, seconds, [](Engine& e) {
        e.setParam(P::FilterType, 0);
        e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
        e.setParam(P::layer(0, P::FmAlgo), 16.0f);  // "17: 8op 8>7>6>5>4>3>2>1"
        for (int op = 0; op < P::kFmOps; ++op) e.setParam(P::fmOp(0, op, P::OpWave), (float)(1 + op % 7));
    });
    bench("VA+FM+VA, LP24", voices, seconds, [](Engine& e) {
        e.setParam(P::layer(1, P::LType), (float)LayerType::FM);
        e.setParam(P::layer(2, P::LType), (float)LayerType::VA);
    });
    bench("WT 1 voice, LP24", voices, seconds, [](Engine& e) { e.setParam(P::layer(0, P::LType), (float)LayerType::WT); });
    bench("WT unison 7, LP24", voices, seconds, [](Engine& e) {
        e.setParam(P::layer(0, P::LType), (float)LayerType::WT);
        e.setParam(P::layer(0, P::WtUnison), 7.0f);
        e.setParam(P::layer(0, P::WtWarpMode), 2.0f);
    });
    bench("PCM at/below root (cubic)", voices, seconds, [](Engine& e) { pcmLayer(e, 100); });
    bench("PCM up 0-31 st (sinc + mips)", voices, seconds, [](Engine& e) { pcmLayer(e, 36); });
    bench("PCM up, Quality Low (cubic)", voices, seconds, [](Engine& e) { pcmLayer(e, 36); e.setQuality(Quality::Low); });
    bench("VA + all FX", voices, seconds, [](Engine& e) {
        e.setParam(P::fxp(fx::Chorus, 0), 1.0f); e.setParam(P::fxp(fx::Chorus, fx::chorus::Mix), 0.5f);
        e.setParam(P::fxp(fx::Delay, 0), 1.0f); e.setParam(P::fxp(fx::Delay, fx::delay::Mix), 0.3f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::Mix), 0.3f);
    });
    // full rack: 12 slots of the heavier blocks
    for (int q = 0; q < 3; ++q) {
        const char* names[3] = {"VA + 12-slot rack (Low)", "VA + 12-slot rack (Normal)", "VA + 12-slot rack (High)"};
        bench(names[q], voices, seconds, [q](Engine& e) {
            e.setQuality((Quality)q);
            const int chain[12] = {fx::Comp, fx::Drive, fx::Amp, fx::Cab, fx::Peq, fx::Chorus, fx::Phaser, fx::Pitch,
                                   fx::Delay, fx::Reverb, fx::Convo, fx::Limiter};
            for (int s = 0; s < 12; ++s) {
                e.setParam(P::fxSlot(s), (float)chain[s]);
                e.setParam(P::fxp(chain[s], 0), 1.0f);
            }
        });
    }
    // multi-timbral: 16 parts x 4 voices vs. the same 64 voices in single-part mode
    bench("single part, 64 voices", 64, seconds, [](Engine&) {});
    // default polyphony (128): typical and heavy patches
    std::printf("-- 128 voices (default polyphony)\n");
    bench("128 x VA saw x2, LP24", 128, seconds, [](Engine&) {});
    bench("128 x PCM (cubic, at root)", 128, seconds, [](Engine& e) {
        e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
        std::vector<float> s(48000 * 40);  // long enough for notes transposed 3 octaves up
        for (size_t i = 0; i < s.size(); ++i) s[i] = std::sin(6.2831853f * 220.0f * (float)i / 48000.0f) * 0.5f;
        auto set = std::make_unique<SampleSet>();
        auto smp = std::make_unique<Sample>();
        smp->channels = 1;
        smp->sampleRate = 48000;
        smp->frames = (int64_t)s.size();
        smp->data[0] = s;
        smp->data[0].resize(s.size() + 64, 0.0f);
        Region r;
        r.sample = smp.get();
        r.keycenter = 60;
        set->samples.push_back(std::move(smp));
        set->regions.push_back(r);
        e.setSampleSet(0, std::move(set));
    });
    bench("128 x VA+FM+VA, LP24", 128, seconds, [](Engine& e) {
        e.setParam(P::layer(1, P::LType), (float)LayerType::FM);
        e.setParam(P::layer(2, P::LType), (float)LayerType::VA);
    });
    bench("128 x VA + 12-slot rack (Normal)", 128, seconds, [](Engine& e) {
        const int chain[12] = {fx::Comp, fx::Drive, fx::Amp, fx::Cab, fx::Peq, fx::Chorus, fx::Phaser, fx::Pitch,
                               fx::Delay, fx::Reverb, fx::Convo, fx::Limiter};
        for (int s = 0; s < 12; ++s) {
            e.setParam(P::fxSlot(s), (float)chain[s]);
            e.setParam(P::fxp(chain[s], 0), 1.0f);
        }
    });
    benchMulti("multi 16 parts x 8 voices (128)", 16, 8, seconds, true);
    benchMulti("multi 16 parts x 4 voices", 16, 4, seconds);
    benchMulti("multi 16x4 + 2 inserts/part", 16, 4, seconds, true);
    return 0;
}
