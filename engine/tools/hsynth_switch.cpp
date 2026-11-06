// Song-switch glitch check: plays song A in real time (paced audio blocks on
// one thread) while a loader thread switches to song B the way the apps do
// (stop, prepareSong, setSong, seek 0, play). Reports the slowest audio
// blocks and when they happened, so loading work that leaks into the audio
// thread shows up as a spike.
//
//   hsynth_switch a.mid b.mid --presets dir[;dir] --library dir [--block 192] [--threads N] [--switches N]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/gm_bank.h"
#include "hsynth/preset_index.h"
#include "hsynth/smf.h"

using namespace hsynth;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    std::vector<std::string> songs;
    std::string presetDirs, library = ".";
    int block = 192, threads = 0, switches = 4;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--presets") presetDirs = next();
        else if (a == "--library") library = next();
        else if (a == "--block") block = std::atoi(next().c_str());
        else if (a == "--threads") threads = std::atoi(next().c_str());
        else if (a == "--switches") switches = std::atoi(next().c_str());
        else songs.push_back(a);
    }
    if (songs.size() < 2) {
        std::fprintf(stderr, "usage: hsynth_switch a.mid b.mid [...] --presets dir[;dir] --library dir\n");
        return 2;
    }
    std::vector<std::pair<std::string, bool>> folders;
    for (size_t a = 0; a <= presetDirs.size();) {
        size_t b = presetDirs.find(';', a);
        if (b == std::string::npos) b = presetDirs.size();
        if (b > a) folders.push_back({presetDirs.substr(a, b - a), false});
        a = b + 1;
    }
    PresetIndex idx;
    idx.setFolders(folders);
    idx.rescan();
    GmBank bank;
    bank.build(idx.all());

    const double sr = 48000.0;
    Engine engine;
    engine.prepare(sr);
    engine.setMaxVoices(128);
    engine.setRenderThreads(threads);
    std::string loaded[kNumParts];

    std::atomic<bool> running{true};
    std::atomic<int> phase{0};  // 0 playing, 1 loading, 2 starting
    struct Slow { double ms, at; int phase; float prof[Engine::kProfPhases]; };
    std::vector<Slow> blocks;
    blocks.reserve(1 << 20);
    const double budget = block / sr * 1000.0;

    std::thread audio([&] {
        std::vector<float> L((size_t)block), R((size_t)block);
        auto t = Clock::now();
        const auto t0 = t;
        const auto period = std::chrono::nanoseconds((long long)(block / sr * 1e9));
        while (running.load()) {
            engine.resetPeakStats();
            const auto a = Clock::now();
            engine.process(L.data(), R.data(), block);
            const auto b = Clock::now();
            Slow s{std::chrono::duration<double, std::milli>(b - a).count(), std::chrono::duration<double>(a - t0).count(),
                   phase.load(), {}};
            engine.peakProfile(s.prof);
            blocks.push_back(s);
            t += period;
            std::this_thread::sleep_until(t);
        }
    });

    auto load = [&](const std::string& path) {
        auto song = std::make_unique<Song>();
        std::string err;
        if (!loadSmf(path, *song, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return; }
        phase = 1;
        engine.songPlay(false);
        const auto a = Clock::now();
        std::vector<std::string> report;
        prepareSong(engine, *song, bank, library, loaded, report);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        {
            int chain[64];
            const int nc = engine.fxChain(chain);
            std::printf("  master chain:");
            for (int i = 0; i < nc; ++i) std::printf(" %d", chain[i]);
            std::printf("  sendA type %.0f level %.2f  sendB level %.2f\n", engine.getParam(P::fxChain(P::SendAType)),
                        engine.getParam(P::fxChain(P::SendALevel)), engine.getParam(P::fxChain(P::SendBLevel)));
        }
        phase = 2;
        engine.setSong(std::move(song));
        engine.songSeek(0.0);
        engine.songPlay(true);
        std::printf("loaded %s in %.0f ms\n", path.c_str(), ms);
    };
    for (int s = 0; s <= switches; ++s) {
        load(songs[(size_t)s % songs.size()]);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        phase = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(3600));
        engine.collectGarbage();
    }
    running = false;
    audio.join();

    const char* names[] = {"playing", "loading", "starting"};
    int over[3] = {}, count[3] = {};
    double worst[3] = {};
    for (const auto& b : blocks) {
        count[b.phase]++;
        worst[b.phase] = std::max(worst[b.phase], b.ms);
        if (b.ms > budget * 0.8) over[b.phase]++;
    }
    std::printf("block %d (%.2f ms budget), %zu blocks\n", block, budget, blocks.size());
    for (int p = 0; p < 3; ++p)
        std::printf("  %-9s %7d blocks  worst %6.2f ms  over 80%% budget: %d\n", names[p], count[p], worst[p], over[p]);
    {
        double sum[Engine::kProfPhases] = {};
        for (const auto& b : blocks)
            for (int i = 0; i < 6; ++i) sum[i] += b.prof[i];
        std::printf("average us: params %.0f events %.0f voices %.0f inserts %.0f rack %.0f stats %.0f\n", sum[0] / blocks.size(),
                    sum[1] / blocks.size(), sum[2] / blocks.size(), sum[3] / blocks.size(), sum[4] / blocks.size(),
                    sum[5] / blocks.size());
    }
    auto sorted = blocks;
    std::sort(sorted.begin(), sorted.end(), [](const Slow& a, const Slow& b) { return a.ms > b.ms; });
    std::printf("slowest blocks:\n");
    for (size_t i = 0; i < std::min<size_t>(12, sorted.size()); ++i)
        std::printf("  %6.2f ms at %7.3f s (%-8s) us: params %.0f events %.0f voices %.0f inserts %.0f rack %.0f stats %.0f\n",
                    sorted[i].ms, sorted[i].at, names[sorted[i].phase], sorted[i].prof[0], sorted[i].prof[1],
                    sorted[i].prof[2], sorted[i].prof[3], sorted[i].prof[4], sorted[i].prof[5]);
    return 0;
}
