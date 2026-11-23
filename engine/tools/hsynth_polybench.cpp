// Polyphony benchmark: render time per audio block versus voice count, paced
// like a real audio callback (one block per period, the thread sleeps between
// blocks so that CPU frequency scaling and worker wake-ups behave as in an app).
//
// usage: hsynth_polybench [options]
//   --patch va|ss|fm|wt|sfz        sound (default va). sfz needs --sfz PATH
//   --sfz PATH                     SFZ / WAV / FLAC instrument for --patch sfz
//   --stream off|auto|always       disk streaming mode for --patch sfz (default auto)
//   --voices 32,64,128             voice counts to measure (default 32,64,128,192,256)
//   --seconds S                    measured audio time per voice count (default 4)
//   --block N                      frames per block (default 96)
//   --threads T                    render threads (default 1, 0 = auto)
//   --fx 0|1|2                     0 off, 1 LIVE rack + both sends, 2 also raise the rack FX macros
//   --quality 0|1|2                Engine Quality (default 1)
//   --nopace                       render blocks back to back (no sleeping)
//   --fifo P                       SCHED_FIFO priority for the rendering thread (Linux, root)
//   --uclamp U                     uclamp.min 0..1024 for every thread of the process (Linux, root):
//                                  emulates a boosted audio app (the CPU governor otherwise lowers
//                                  the clock to the duty cycle of a paced thread)
//   --pin MASK                     pin the rendering thread and the render helpers to the CPUs in
//                                  MASK (hex), other threads (disk streamer) stay free (app-like)
//   --appprio                      helpers SCHED_OTHER nice -19 like in an app (root would get FIFO)
//   --churn K                      VA/FM: K note-offs + K note-ons every 8 blocks (default 2)
//   --flacbench FILE               decode a FLAC file repeatedly, print frames per second
//   --dump FILE                    render a fixed 4 s sequence to FILE as raw
//                                  float32 stereo interleaved, for before/after comparisons
// Output: one line per voice count with mean / p50 / p99 / p99.9 / max block time in
// microseconds, deadline share, and phase 4 (rack + sends + mix + master)
// p99 / p99.9 / max in microseconds.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/flac.h"
#include "hsynth/sample_set.h"
#include "hsynth/streaming.h"

#if defined(__linux__) || defined(__ANDROID__)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

using namespace hsynth;

namespace {

struct Opts {
    std::string patch = "va";
    std::string sfz;
    std::string stream = "auto";
    std::vector<int> voices{32, 64, 128, 192, 256};
    double seconds = 4.0;
    int block = 96;
    int threads = 1;
    int fx = 0;
    int quality = 1;
    bool pace = true;
    int fifo = 0;
    int churn = 2;
    int uclamp = -1;
    unsigned pin = 0;
    bool appPrio = false;
    std::string dump;
    std::string flacbench;
};

std::unique_ptr<SampleSet> loadSet(const Opts& o) {
    SampleLoadOptions lo = sampleLoadOptions();
    lo.mode = o.stream == "off" ? StreamMode::Off : o.stream == "always" ? StreamMode::Always : StreamMode::Auto;
    setSampleLoadOptions(lo);
    std::string err;
    auto set = loadSampleSet(o.sfz, err);
    if (!set) {
        std::fprintf(stderr, "cannot load %s: %s\n", o.sfz.c_str(), err.c_str());
        std::exit(1);
    }
    return set;
}

void setupPatch(Engine& e, const Opts& o) {
    e.setQuality((Quality)o.quality);
    e.setParam(P::AmpSustain, 1.0f);
    if (o.fx) {
        e.setMultiTimbral(true);
        const int rack[] = {fx::Comp, fx::Delay, fx::Reverb, fx::Limiter};
        for (int i = 0; i < fx::kFxSlots; ++i)
            e.setParam(P::fxSlot(i), i < 4 ? (float)rack[i] : (float)fx::None);
        e.setParam(P::fxChain(P::FxTempo), 120.0f);
        e.setParam(P::MasterVolume, 0.75f);
        e.setParam(P::Limiter, 1.0f);
        e.setParam(P::fxp(fx::Comp, fx::comp::On), 1.0f);
        e.setParam(P::fxp(fx::Comp, fx::comp::Thresh), -14.0f);
        e.setParam(P::fxp(fx::Comp, fx::comp::Ratio), 2.0f);
        e.setParam(P::fxp(fx::Comp, fx::comp::Attack), 20.0f);
        e.setParam(P::fxp(fx::Comp, fx::comp::Release), 150.0f);
        e.setParam(P::fxp(fx::Delay, fx::delay::On), 1.0f);
        e.setParam(P::fxp(fx::Delay, fx::delay::Sync), 9.0f);  // dotted eighth
        e.setParam(P::fxp(fx::Delay, fx::delay::Feedback), 0.45f);
        e.setParam(P::fxp(fx::Delay, fx::delay::Mix), 0.0f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 1.0f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::Type), 1.0f);  // hall
        e.setParam(P::fxp(fx::Reverb, fx::reverb::Decay), 4.0f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::Size), 0.8f);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::Mix), 0.0f);
        e.setParam(P::fxp(fx::Limiter, fx::limiter::On), 1.0f);
        e.setParam(P::fxp(fx::Limiter, fx::limiter::Ceiling), -0.5f);
        const auto& targets = fxModTargets();
        auto macro = [&](int slot, fx::FxModSource source, int param, float amount) {
            e.setParam(P::fxMod(slot, P::FxModSrc), (float)source);
            e.setParam(P::fxMod(slot, P::FxModDst), (float)(std::find(targets.begin(), targets.end(), param) - targets.begin()));
            e.setParam(P::fxMod(slot, P::FxModAmt), amount);
        };
        macro(0, fx::FxModSource::Macro1, P::fxp(fx::Reverb, fx::reverb::Mix), 0.45f);
        macro(1, fx::FxModSource::Macro2, P::fxp(fx::Delay, fx::delay::Mix), 0.4f);
        e.setParam(P::fxChain(P::FxMacro1), o.fx == 2 ? 0.67f : 0.0f);
        e.setParam(P::fxChain(P::FxMacro2), o.fx == 2 ? 0.625f : 0.0f);
        e.setParam(P::SendA, 0.25f);
        e.setParam(P::SendB, 0.15f);
        e.setParam(P::fxChain(P::SendADecay), 2.4f);
        e.setParam(P::fxChain(P::SendBSync), 9.0f);
        e.setParam(P::fxChain(P::SendBFeedback), 0.3f);
        e.setParam(P::fxChain(P::SendBLevel), 0.7f);
    } else {
        for (int i = 0; i < fx::kFxSlots; ++i) e.setParam(P::fxSlot(i), (float)fx::None);
    }
    if (o.patch == "va") {
        // two saws, LP24 with some resonance and filter envelope
        e.setParam(P::FilterType, (float)FilterType::LP24);
        e.setParam(P::FilterCutoff, 2500.0f);
        e.setParam(P::FilterReso, 0.3f);
    } else if (o.patch == "ss") {
        // supersaw (7 saws) + saw, LP24
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::SuperSaw);
        e.setParam(P::layer(0, P::VaSsMix), 0.8f);
        e.setParam(P::layer(0, P::VaSsDetune), 0.5f);
        e.setParam(P::FilterType, (float)FilterType::LP24);
        e.setParam(P::FilterCutoff, 4000.0f);
        e.setParam(P::FilterReso, 0.2f);
    } else if (o.patch == "fm") {
        // 6-op DX-style algorithm, filter off
        e.setParam(P::FilterType, 0);
        e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
        e.setParam(P::layer(0, P::FmAlgo), 8.0f);
    } else if (o.patch == "wt") {
        e.setParam(P::layer(0, P::LType), (float)LayerType::WT);
    } else if (o.patch == "sfz") {
        e.setParam(P::FilterType, 0);
        e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
        e.setParam(P::AmpRelease, 0.3f);
        e.setSampleSet(0, loadSet(o));
    } else {
        std::fprintf(stderr, "unknown patch %s\n", o.patch.c_str());
        std::exit(1);
    }
}

// uclamp.min for every thread of this process (the render helpers included)
void applyUclamp(int u) {
#if defined(__linux__) || defined(__ANDROID__)
    if (u < 0) return;
    struct Attr {
        uint32_t size, policy;
        uint64_t flags;
        int32_t nice;
        uint32_t prio;
        uint64_t runtime, deadline, period;
        uint32_t umin, umax;
    } a{};
    a.size = sizeof a;
    a.flags = 0x08 | 0x10 | 0x20;  // keep policy, keep params, util clamp min
    a.umin = (uint32_t)u;
    a.umax = 1024;
    DIR* d = opendir("/proc/self/task");
    if (!d) return;
    int fails = 0;
    while (dirent* de = readdir(d)) {
        const int tid = std::atoi(de->d_name);
        if (tid > 0 && syscall(SYS_sched_setattr, tid, &a, 0) != 0) ++fails;
    }
    closedir(d);
    if (fails) std::fprintf(stderr, "uclamp: %d threads failed\n", fails);
#else
    (void)u;
#endif
}

// app-like thread placement: the audio thread and the render helpers on `mask`
void pinRenderThreads(Engine& e, const Opts& o) {
#if defined(__linux__) || defined(__ANDROID__)
    int tids[16];
    const int n = e.renderThreadIds(tids, 16);
    if (o.pin) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 0; c < 32; ++c)
            if (o.pin & (1u << c)) CPU_SET(c, &set);
        sched_setaffinity(0, sizeof set, &set);
        for (int i = 0; i < n; ++i) sched_setaffinity(tids[i], sizeof set, &set);
    }
    if (o.appPrio) {
        sched_param sp{};
        for (int i = 0; i < n; ++i) {
            sched_setscheduler(tids[i], SCHED_OTHER, &sp);
            setpriority(PRIO_PROCESS, (id_t)tids[i], -19);
        }
    }
#else
    (void)e;
    (void)o;
#endif
}

double nowUs() {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleepUntilUs(double t) {
#if defined(__linux__) || defined(__ANDROID__)
    const double ns = t * 1000.0;
    timespec ts;
    ts.tv_sec = (time_t)(ns / 1e9);
    ts.tv_nsec = (long)(ns - (double)ts.tv_sec * 1e9);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
#else
    const double d = t - nowUs();
    if (d > 0) std::this_thread::sleep_for(std::chrono::microseconds((int64_t)d));
#endif
}

// Note source: keeps `target` notes sounding. Sustained patches start all notes
// at once and churn a few; decaying samples (sfz) are re-triggered as they end.
void note(Engine& e, Event event) {
    event.channel = 0;
    e.sendEvent(event);
}

struct Player {
    uint32_t nextId = 1;
    std::vector<std::pair<uint32_t, int>> held;  // id, key (oldest first)
    int keyLo = 36, keyHi = 96;
    int vel = 0;
    void on(Engine& e) {
        const int span = keyHi - keyLo + 1;
        const int key = keyLo + (int)((nextId * 7u) % (uint32_t)span);
        const float v = (vel++ % 3 == 0) ? 0.3f : (vel % 3 == 1 ? 0.65f : 0.95f);
        note(e, Event::noteOn(nextId, key, v));
        held.push_back({nextId, key});
        ++nextId;
    }
    void offOldest(Engine& e) {
        if (held.empty()) return;
        note(e, Event::noteOff(held.front().first, held.front().second));
        held.erase(held.begin());
    }
};

struct Result {
    double mean = 0, p50 = 0, p99 = 0, p999 = 0, max = 0;
    double fxP99 = 0, fxP999 = 0, fxMax = 0;
    int active = 0;
    uint32_t underruns = 0;
};

Result runOne(const Opts& o, int voices) {
    Engine e;
    e.prepare(48000.0f);
    e.setRenderThreads(o.threads);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // helpers started
    applyUclamp(o.uclamp);
    pinRenderThreads(e, o);
    e.setMaxVoices(voices);
    setupPatch(e, o);
    const bool decaying = o.patch == "sfz";
    std::vector<float> L((size_t)o.block), R((size_t)o.block);
    Player pl;
    if (decaying) { pl.keyLo = 24; pl.keyHi = 103; }
    // start: all notes at once for sustained patches; sfz ramps up 8 per block
    if (!decaying)
        for (int i = 0; i < voices; ++i) pl.on(e);
    // warm-up (1 s): voices reach steady state, the streamer fills, caches warm
    const int warm = (int)(48000 / o.block);
    auto feed = [&](int b) {
        if (decaying) {
            const int active = e.stats().activeVoices;
            // like a pianist holding the pedal: one new note per block while
            // fewer than `voices` sound; notes ring until their samples end
            if (active < voices) {
                pl.on(e);
                if ((int)pl.held.size() > voices) pl.offOldest(e);
            }
        } else if (o.churn > 0 && b % 8 == 0) {
            for (int i = 0; i < o.churn; ++i) {
                pl.offOldest(e);
                pl.on(e);
            }
        }
    };
    const double period = 1e6 * o.block / 48000.0;
    double next = nowUs();
    for (int b = 0; b < warm; ++b) {
        if (o.pace) {
            next += period;
            sleepUntilUs(next);
        }
        feed(b);
        e.process(L.data(), R.data(), o.block);
    }
    const int blocks = (int)(48000.0 * o.seconds / o.block);
    std::vector<double> t((size_t)blocks), phase4((size_t)blocks);
    next = nowUs();
    const uint32_t under0 = e.stats().streamUnderruns;
    long activeSum = 0;
    for (int b = 0; b < blocks; ++b) {
        if (o.pace) {
            next += period;
            sleepUntilUs(next);
        }
        feed(b);
        const double t0 = nowUs();
        e.process(L.data(), R.data(), o.block);
        t[(size_t)b] = nowUs() - t0;
        phase4[(size_t)b] = e.lastFxMicros();
        activeSum += e.stats().activeVoices;
    }
    Result r;
    for (double x : t) r.mean += x;
    r.mean /= blocks;
    std::sort(t.begin(), t.end());
    std::sort(phase4.begin(), phase4.end());
    auto pct = [&](const std::vector<double>& values, double p) {
        return values[std::min((size_t)blocks - 1, (size_t)(p * blocks))];
    };
    r.p50 = pct(t, 0.5);
    r.p99 = pct(t, 0.99);
    r.p999 = pct(t, 0.999);
    r.max = t.back();
    r.fxP99 = pct(phase4, 0.99);
    r.fxP999 = pct(phase4, 0.999);
    r.fxMax = phase4.back();
    r.active = (int)(activeSum / blocks);
    r.underruns = e.stats().streamUnderruns - under0;
    return r;
}

void dump(const Opts& o) {
    Engine e;
    e.prepare(48000.0f);
    e.setRenderThreads(o.threads);
    e.setMaxVoices(o.voices.empty() ? 64 : o.voices.back());
    setupPatch(e, o);
    // offline streaming: wait for the disk instead of playing silence
    e.setOfflineRender(true);
    std::FILE* f = std::fopen(o.dump.c_str(), "wb");
    if (!f) { std::perror(o.dump.c_str()); std::exit(1); }
    std::vector<float> L((size_t)o.block), R((size_t)o.block), I((size_t)o.block * 2);
    const int blocks = (int)(48000 * 4 / o.block);
    uint32_t id = 1;
    for (int b = 0; b < blocks; ++b) {
        // a chord every 25 blocks (50 ms at 96), released 100 blocks later
        if (b % 25 == 0 && b < blocks - 150) {
            for (int k = 0; k < 4; ++k)
                note(e, Event::noteOn(id + k, 36 + (int)((id * 5 + k * 7) % 60), 0.4f + 0.15f * (float)k));
            id += 4;
        }
        if (b % 25 == 0 && b >= 100) {
            const uint32_t first = (uint32_t)((b - 100) / 25) * 4 + 1;
            for (int k = 0; k < 4; ++k) note(e, Event::noteOff(first + k, 36 + (int)((first * 5 + k * 7) % 60)));
        }
        e.process(L.data(), R.data(), o.block);
        for (int i = 0; i < o.block; ++i) { I[(size_t)i * 2] = L[(size_t)i]; I[(size_t)i * 2 + 1] = R[(size_t)i]; }
        std::fwrite(I.data(), sizeof(float), I.size(), f);
    }
    std::fclose(f);
    std::printf("wrote %s (%d blocks)\n", o.dump.c_str(), blocks);
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(1); }
            return argv[++i];
        };
        if (a == "--patch") o.patch = val();
        else if (a == "--sfz") o.sfz = val();
        else if (a == "--stream") o.stream = val();
        else if (a == "--voices") {
            o.voices.clear();
            const std::string s = val();
            for (size_t p = 0; p < s.size();) {
                o.voices.push_back(std::atoi(s.c_str() + p));
                const size_t c = s.find(',', p);
                if (c == std::string::npos) break;
                p = c + 1;
            }
        } else if (a == "--seconds") o.seconds = std::atof(val().c_str());
        else if (a == "--block") o.block = std::atoi(val().c_str());
        else if (a == "--threads") o.threads = std::atoi(val().c_str());
        else if (a == "--fx") o.fx = std::atoi(val().c_str());
        else if (a == "--quality") o.quality = std::atoi(val().c_str());
        else if (a == "--nopace") o.pace = false;
        else if (a == "--fifo") o.fifo = std::atoi(val().c_str());
        else if (a == "--uclamp") o.uclamp = std::atoi(val().c_str());
        else if (a == "--pin") o.pin = (unsigned)std::strtoul(val().c_str(), nullptr, 16);
        else if (a == "--appprio") o.appPrio = true;
        else if (a == "--churn") o.churn = std::atoi(val().c_str());
        else if (a == "--dump") o.dump = val();
        else if (a == "--flacbench") o.flacbench = val();
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }
    if (o.block < 1 || o.block > kMaxBlockSize || !std::isfinite(o.seconds) ||
        o.seconds * 48000.0 < o.block || o.fx < 0 || o.fx > 2) {
        std::fprintf(stderr, "invalid --block, --seconds or --fx\n");
        return 1;
    }
    if (!o.flacbench.empty()) {
        std::FILE* f = std::fopen(o.flacbench.c_str(), "rb");
        if (!f) { std::perror(o.flacbench.c_str()); return 1; }
        std::vector<uint8_t> data;
        uint8_t buf[65536];
        for (size_t k; (k = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + k);
        std::fclose(f);
        std::vector<float> l, r;
        std::string err;
        int64_t frames = 0;
        const double t0 = nowUs();
        int reps = 0;
        while (nowUs() - t0 < o.seconds * 1e6) {
            FlacFrameDecoder dec;
            if (!dec.open(data.data(), data.size(), err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
            for (int k; (k = dec.next(l, r, err)) > 0;) frames += k;
            ++reps;
        }
        const double sec = (nowUs() - t0) * 1e-6;
        std::printf("flac decode: %.2f M frames/s (%d passes, %.1fx real time at 48 kHz)\n", frames / sec * 1e-6, reps,
                    frames / sec / 48000.0);
        return 0;
    }
    if (!o.dump.empty()) {
        dump(o);
        return 0;
    }
#if defined(__linux__) || defined(__ANDROID__)
    if (o.fifo > 0) {
        sched_param sp{};
        sp.sched_priority = o.fifo;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) std::fprintf(stderr, "SCHED_FIFO failed\n");
    }
#endif
    int rt = 1;
    {
        Engine probe;
        probe.setRenderThreads(o.threads);
        rt = probe.renderThreads();
    }
    const double period = 1e6 * o.block / 48000.0;
    std::printf("# patch %s  fx %d  block %d (%.0f us)  render threads %d  quality %d  %s\n", o.patch.c_str(),
                o.fx, o.block, period, rt, o.quality, o.pace ? "paced" : "back-to-back");
    std::printf("%6s %7s %8s %8s %8s %8s %8s %7s %6s %10s %10s %10s\n", "voices", "active", "mean_us", "p50_us",
                "p99_us", "p999_us", "max_us", "p99%", "under", "fx_p99_us", "fx_p999_us", "fx_max_us");
    for (int v : o.voices) {
        const Result r = runOne(o, v);
        std::printf("%6d %7d %8.1f %8.1f %8.1f %8.1f %8.1f %6.1f%% %6u %10.1f %10.1f %10.1f\n", v, r.active, r.mean,
                    r.p50, r.p99, r.p999, r.max, r.p99 / period * 100.0, r.underruns, r.fxP99, r.fxP999, r.fxMax);
        std::fflush(stdout);
    }
    return 0;
}
