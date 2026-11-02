// Micro benchmark: CPU cost per voice for each layer type / feature.
// usage: hsynth_bench [voices=32] [seconds=3]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

#include "hsynth/engine.h"

using namespace hsynth;

static double bench(const char* name, int voices, double seconds, const std::function<void(Engine&)>& setup) {
    Engine e;
    e.prepare(48000);
    e.setMaxVoices(64);
    e.setParam(P::AmpSustain, 1.0f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    setup(e);
    for (int i = 0; i < voices; ++i) e.sendEvent(Event::noteOn((uint32_t)i + 1, 36 + i, 0.7f));
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
static double benchMulti(const char* name, int parts, int perPart, double seconds) {
    Engine e;
    e.prepare(48000);
    e.setMaxVoices(64);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setMultiTimbral(true);
    for (int k = 0; k < parts; ++k) {
        e.setPartParam(k, P::AmpSustain, 1.0f);
        e.setPartParam(k, P::FilterCutoff, 800.0f + 200.0f * (float)k);
        e.setPartMix(k, Mix::Pan, -1.0f + 2.0f * (float)k / (float)std::max(1, parts - 1));
        e.setPartMix(k, Mix::FxSend, k % 2 ? 1.0f : 0.5f);
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

int main(int argc, char** argv) {
    const int voices = argc > 1 ? std::atoi(argv[1]) : 32;
    const double seconds = argc > 2 ? std::atof(argv[2]) : 3.0;
    std::printf("%d voices, %.1f s audio @48k, block 128\n", voices, seconds);
    bench("empty (all layers off)", voices, seconds, [](Engine& e) { e.setParam(P::layer(0, P::LType), 0); });
    bench("VA saw x2, filter off", voices, seconds, [](Engine& e) { e.setParam(P::FilterType, 0); });
    bench("VA saw x2, LP24", voices, seconds, [](Engine&) {});
    bench("VA saw x2, LP24 + drive", voices, seconds, [](Engine& e) { e.setParam(P::FilterDrive, 0.5f); });
    bench("VA saw x2, ladder + drive", voices, seconds, [](Engine& e) {
        e.setParam(P::FilterType, (float)FilterType::Ladder);
        e.setParam(P::FilterDrive, 0.5f);
    });
    bench("VA saw x2, comb+", voices, seconds, [](Engine& e) { e.setParam(P::FilterType, (float)FilterType::CombPlus); });
    bench("VA saw x2, formant", voices, seconds, [](Engine& e) { e.setParam(P::FilterType, (float)FilterType::Formant); });
    bench("VA saw x2, LP24 + vfx ring", voices, seconds, [](Engine& e) { e.setParam(P::VfxType, (float)VfxType::Ring); });
    bench("VA saw x2, LP24 + vfx crush", voices, seconds, [](Engine& e) { e.setParam(P::VfxType, (float)VfxType::Crush); });
    bench("FM 4-op, filter off", voices, seconds, [](Engine& e) {
        e.setParam(P::FilterType, 0);
        e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
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
    benchMulti("multi 16 parts x 4 voices", 16, 4, seconds);
    return 0;
}
