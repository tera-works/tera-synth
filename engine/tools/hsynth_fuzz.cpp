// Mutation fuzzer for the file parsers (WAV, FLAC, JSON, presets, multis,
// SFZ, Scala, wavetables, impulse responses). Build the engine with
// -fsanitize=address,undefined to make it useful; see docs/TEST_REPORT.md.
//
// usage: hsynth_fuzz <target> <iterations> <workdir> <seed files...>
//   target: wav | flac | json | scala | preset | multi | sfz | wavetable | ir
//   workdir: scratch folder (SFZ / WAV targets write their input there; put
//            s.wav / st.wav next to it for SFZ seeds that reference them)
// Inputs slower than 0.5 s are reported and saved as workdir/slow_<n>.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "hsynth/dsp/wavetable.h"
#include "hsynth/flac.h"
#include "hsynth/fx/ir_load.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/preset.h"
#include "hsynth/sample_set.h"
#include "hsynth/tuning.h"
#include "hsynth/wav.h"
using namespace hsynth;
using Bytes = std::vector<uint8_t>;

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static uint32_t rnd(uint32_t n) { return n ? rnd() % n : 0; }

static const char* kTokens[] = {"<region>", "<group>", "<global>", "<master>", "<control>", "<curve>", "#define $X 5\n", "#include \"t.sfz\"\n",
    "sample=", "lokey=", "hikey=", "key=", "pitch_keycenter=", "loop_start=", "loop_end=", "loop_mode=loop_continuous ", "offset=", "end=",
    "seq_length=", "seq_position=", "sw_lokey=", "sw_hikey=", "sw_last=", "group=", "off_by=", "trigger=release ", "cutoff=", "resonance=",
    "xfin_lovel=", "xfout_hivel=", "xfin_lokey=", "xfout_hikey=", "loop_crossfade=", "delay=", "ampeg_hold=", "tune=", "transpose=",
    "-1", "0", "127", "128", "99999999999", "-2147483649", "nan", "inf", "1e308", "c#-1", "g9", "zz", "\"", "{", "}", "[", "]", ",", ":",
    "\"params\"", "\"name\"", "\"format\"", "\"hsynth-multi\"", "\"parts\"", "\"mix\"", "\"preset\"", "\"samples\"", "null", "true",
    "1e999", "-0", "\\u0000", "\\ud800", "\"a.type\"", "\"pcm\"", "\"channel\"", " ", "\n", "!", "/", "1/0", "0/1", "1200.0", "\x00", "\xff"};

static Bytes mutate(Bytes d, bool text) {
    const int n = 1 + (int)rnd(8);
    for (int k = 0; k < n; ++k) {
        switch (rnd(text ? 9 : 7)) {
            case 0: if (!d.empty()) d[rnd((uint32_t)d.size())] ^= (uint8_t)(1u << rnd(8)); break;
            case 1: if (!d.empty()) { static const uint8_t v[] = {0, 1, 0x7F, 0x80, 0xFF, 0xFE, 0x40}; d[rnd((uint32_t)d.size())] = v[rnd(7)]; } break;
            case 2: if (d.size() > 1) d.resize(rnd((uint32_t)d.size())); break;  // truncate
            case 3: if (!d.empty()) { size_t a = rnd((uint32_t)d.size()), l = rnd(16); d.erase(d.begin() + (long)a, d.begin() + (long)std::min(d.size(), a + l)); } break;
            case 4: if (!d.empty()) { size_t a = rnd((uint32_t)d.size()), l = std::min<size_t>(rnd(64), d.size() - a); Bytes c(d.begin() + (long)a, d.begin() + (long)(a + l)); d.insert(d.begin() + (long)rnd((uint32_t)d.size()), c.begin(), c.end()); } break;
            case 5: if (d.size() >= 4) { size_t a = rnd((uint32_t)d.size() - 3); uint32_t v = rnd(4) == 0 ? 0xFFFFFFFFu : rnd(4) == 0 ? 0x7FFFFFFFu : rnd(); std::memcpy(&d[a], &v, 4); } break;  // 32-bit sizes
            case 6: { const size_t a = d.empty() ? 0 : rnd((uint32_t)d.size()); d.insert(d.begin() + (long)a, (uint8_t)rnd(256)); break; }
            default: { const char* t = kTokens[rnd(sizeof kTokens / sizeof kTokens[0])]; const size_t a = d.empty() ? 0 : rnd((uint32_t)d.size()); d.insert(d.begin() + (long)a, t, t + std::strlen(t)); break; }
        }
    }
    return d;
}
static Bytes readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static void writeFile(const std::string& p, const Bytes& d) { std::ofstream f(p, std::ios::binary); f.write((const char*)d.data(), (std::streamsize)d.size()); }

static const char kStreamSfz[] = "<region> sample=s.wav lokey=55 hikey=65 pitch_keycenter=60\n";

int main(int argc, char** argv) {
    const std::string target = argv[1];
    const long iters = std::atol(argv[2]);
    const std::string dir = argv[3];
    std::vector<Bytes> seeds;
    for (int i = 4; i < argc; ++i) seeds.push_back(readFile(argv[i]));
    const bool text = target == "sfz" || target == "preset" || target == "multi" || target == "scala" || target == "json";
    Engine* engine = nullptr;
    if (target == "preset" || target == "multi") { engine = new Engine; engine->prepare(48000); }
    long ok = 0;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    for (long it = 0; it < iters; ++it) {
        if (it % 200 == 0) std::printf("progress %ld\n", it);
        Bytes d = mutate(seeds[rnd((uint32_t)seeds.size())], text);
        if (rnd(10) == 0 && seeds.size() > 1) { const Bytes& o = seeds[rnd((uint32_t)seeds.size())]; d.insert(d.end(), o.begin() + (long)rnd((uint32_t)o.size()), o.end()); }
        std::string err;
        const std::string s(d.begin(), d.end());
        const auto t0 = std::chrono::steady_clock::now();
        struct Slow { std::chrono::steady_clock::time_point t0; const Bytes& d; const std::string& dir; long it;
            ~Slow() { const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (sec > 0.5) { std::printf("SLOW input %ld: %.2f s, %zu bytes\n", it, sec, d.size()); writeFile(dir + "/slow_" + std::to_string(it), d); } } } slow{t0, d, dir, it};
        if (target == "wav") { WavData w; ok += readWavMemory(d.data(), d.size(), w, err); }
        else if (target == "flac") { WavData w; ok += readFlacMemory(d.data(), d.size(), w, err); }
        else if (target == "json") { Json j; ok += Json::parse(s, j, err); if (ok && rnd(4) == 0) (void)j.dump(2); }
        else if (target == "scala") { ScalaScale sc; if (parseScala(s, sc, err)) { float o[12]; ok += scalaToOffsets(sc, (int)rnd(30) - 10, o, err); } }
        else if (target == "preset") {
            Preset p; if (presetFromJson(s, p, err)) { ++ok; std::vector<std::string> w; applyPreset(*engine, p, dir, w); (void)presetToJson(p);
                std::vector<float> L(64), R(64); engine->sendEvent(Event::noteOn(1, 60, 0.8f)); engine->process(L.data(), R.data(), 64); engine->collectGarbage(); }
        } else if (target == "multi") {
            MultiPreset m; if (multiFromJson(s, m, err)) { ++ok; std::vector<std::string> w; applyMulti(*engine, m, dir, w); (void)multiToJson(m);
                std::vector<float> L(64), R(64); engine->process(L.data(), R.data(), 64); engine->collectGarbage(); }
        } else if (target == "sfz") {
            writeFile(dir + "/f.sfz", d);
            auto set = loadSfzSampleSet(dir + "/f.sfz", err);
            if (set) { ++ok; Engine e; e.prepare(48000); e.setParam(P::layer(0, P::LType), 3); e.setSampleSet(0, std::move(set));
                std::vector<float> L(256), R(256); for (int k : {20, 60, 100}) e.sendEvent(Event::noteOn((uint32_t)k, k, 0.7f));
                for (int b = 0; b < 4; ++b) e.process(L.data(), R.data(), 256); for (int k : {20, 60, 100}) e.sendEvent(Event::noteOff((uint32_t)k, k)); e.process(L.data(), R.data(), 256); }
        } else if (target == "stream") {
            // WAV header / data parsing of the disk-streaming loader (streaming.h) and streamed playback
            writeFile(dir + "/s.wav", d);
            writeFile(dir + "/s.sfz", Bytes(std::begin(kStreamSfz), std::end(kStreamSfz) - 1));
            SampleLoadOptions o;
            o.mode = StreamMode::Always;
            o.preloadFrames = 4096;
            setSampleLoadOptions(o);
            auto set = loadSfzSampleSet(dir + "/s.sfz", err);
            if (set) { ++ok; Engine e; e.prepare(48000); e.setParam(P::layer(0, P::LType), 3); e.setSampleSet(0, std::move(set));
                e.setOfflineRender(true);
                std::vector<float> L(512), R(512); for (int k : {55, 60, 65}) e.sendEvent(Event::noteOn((uint32_t)k, k, 0.7f));
                for (int b = 0; b < 40; ++b) e.process(L.data(), R.data(), 512); }
        } else if (target == "wavetable") { writeFile(dir + "/f.wav", d); ok += loadWavetableFile(dir + "/f.wav", err) != nullptr; }
        else if (target == "ir") { writeFile(dir + "/f.wav", d); ok += fx::loadImpulseResponse(dir + "/f.wav", 48000, (int)rnd(2), err) != nullptr; }
    }
    std::printf("%s: %ld iterations, %ld parsed OK\n", target.c_str(), iters, ok);
    delete engine;
}
