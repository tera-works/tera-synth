// Disk streaming of large samples (streaming.h): streamed playback must sound
// exactly like the same samples held in memory.
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/host_utils.h"
#include "hsynth/sample_set.h"
#include "hsynth/wav.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;
namespace fs = std::filesystem;

namespace {

std::string tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / "hsynth_tests" / name;
    fs::create_directories(p);
    return p.generic_string();
}

// Restores the global loader options at the end of a test.
struct OptionsGuard {
    SampleLoadOptions saved = sampleLoadOptions();
    ~OptionsGuard() { setSampleLoadOptions(saved); }
};

void setMode(StreamMode m, const std::string& cacheDir = "") {
    SampleLoadOptions o;
    o.mode = m;
    o.preloadFrames = 4096;
    o.cacheDir = cacheDir;
    setSampleLoadOptions(o);
}

// Stereo sweep (different on the two channels), `seconds` long.
void writeLongWav(const std::string& path, double seconds, bool float32) {
    const int n = (int)(seconds * 48000);
    std::vector<float> l((size_t)n), r((size_t)n);
    double ph1 = 0, ph2 = 0;
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / n;
        ph1 += 2.0 * 3.141592653589793 * (200.0 + 800.0 * t) / 48000.0;
        ph2 += 2.0 * 3.141592653589793 * (300.0 + 500.0 * t) / 48000.0;
        l[(size_t)i] = (float)(0.5 * std::sin(ph1));
        r[(size_t)i] = (float)(0.4 * std::sin(ph2) * (1.0 - 0.5 * t));
    }
    std::string err;
    writeWav(path, l, &r, 48000, float32, err);
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

std::unique_ptr<SampleSet> load(const std::string& sfz) {
    std::string err;
    auto set = loadSampleSet(sfz, err);
    if (!set) INFO("load failed: %s\n", err.c_str());
    return set;
}

// Renders a few notes (at, below and above the root: Hermite and sinc paths)
// with the set on layer A.
void render(std::unique_ptr<SampleSet> set, std::vector<float>& L, std::vector<float>& R, EngineStats* stats = nullptr,
            double seconds = 3.0) {
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::AmpSustain, 1.0f);
    e.setParam(P::AmpRelease, 0.05f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setSampleSet(0, std::move(set));
    RenderJob job;
    job.seconds = seconds;
    addNote(job, 0.00, 2.5, 60, 0.8f);
    addNote(job, 0.10, 2.0, 57, 0.7f);
    addNote(job, 0.20, 1.8, 63, 0.6f);
    addNote(job, 1.50, 1.0, 62, 0.9f);
    renderOffline(e, job, L, R);
    if (stats) *stats = e.stats();
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double d = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) d = std::max(d, (double)std::fabs(a[i] - b[i]));
    return d;
}

const char* kSfz = "<region> sample=long.wav lokey=55 hikey=65 pitch_keycenter=60\n";

}  // namespace

TEST(streaming_matches_in_memory) {
    OptionsGuard guard;
    const std::string dir = tempDir("stream_wav");
    writeLongWav(dir + "/long.wav", 4.0, false);
    writeText(dir + "/long.sfz", kSfz);

    setMode(StreamMode::Off);
    auto mem = load(dir + "/long.sfz");
    setMode(StreamMode::Always);
    auto str = load(dir + "/long.sfz");
    CHECK(mem && str);
    if (!mem || !str) return;
    INFO("in memory %zu KB, streamed %zu KB in memory + %lld KB on disk\n", mem->memoryBytes() / 1024,
         str->memoryBytes() / 1024, (long long)(str->diskBytes() / 1024));
    CHECK(mem->streamedSamples() == 0);
    CHECK(str->streamedSamples() == 1);
    CHECK(str->memoryBytes() * 10 < mem->memoryBytes());

    std::vector<float> l1, r1, l2, r2;
    EngineStats st;
    render(std::move(mem), l1, r1);
    render(std::move(str), l2, r2, &st);
    const double d = std::max(maxDiff(l1, l2), maxDiff(r1, r2));
    INFO("max |memory - streamed| = %.3g, %llu KB read from disk, %u underruns\n", d,
         (unsigned long long)(st.streamBytesRead / 1024), st.streamUnderruns);
    CHECK(rms(l1, 48000, 96000) > 0.05);
    CHECK(d < 1e-6);
    CHECK(st.streamUnderruns == 0);
    CHECK(st.streamBytesRead > 0);
    CHECK(st.streamVoices == 0);  // all notes ended: every slot released
}

// FLAC streams straight from the compressed file: the streamer decodes it a
// frame at a time (nothing decoded is written to disk), output identical to
// the fully decoded sample. The fixture has two FLAC frames (4608 + 4392) and
// a 4096-frame head, so streaming starts inside the first frame.
TEST(streaming_flac_direct) {
    OptionsGuard guard;
    const std::string dir = tempDir("stream_flac");
    const std::string cache = dir + "/cache";
    fs::remove_all(utf8Path(cache));
    const std::string flac = std::string(HSYNTH_TEST_DATA) + "/flac/s16_mid_side.flac";
    writeText(dir + "/f.sfz", "<region> sample=" + flac + " lokey=55 hikey=65 pitch_keycenter=60\n");

    setMode(StreamMode::Off);
    auto mem = load(dir + "/f.sfz");
    setMode(StreamMode::Always, cache);
    auto str = load(dir + "/f.sfz");
    CHECK(mem && str);
    if (!mem || !str) return;
    CHECK(str->streamedSamples() == 1);
    CHECK(!fs::exists(utf8Path(cache)));  // no decode cache any more
    CHECK(str->memoryBytes() * 2 < mem->memoryBytes());

    std::vector<float> l1, r1, l2, r2;
    EngineStats st;
    render(std::move(mem), l1, r1, nullptr, 1.0);
    render(std::move(str), l2, r2, &st, 1.0);
    const double d = std::max(maxDiff(l1, l2), maxDiff(r1, r2));
    INFO("FLAC: max |memory - streamed| = %.3g, %llu bytes read, %u underruns\n", d,
         (unsigned long long)st.streamBytesRead, st.streamUnderruns);
    CHECK(rms(l1, 0, 9000) > 0.001);
    CHECK(d < 1e-6);
    CHECK(st.streamUnderruns == 0);
    // The four retriggers share one compressed read and decoded frames.
    const auto fileSize = (uint64_t)fs::file_size(utf8Path(flac));
    CHECK(st.streamBytesRead == fileSize);
}

TEST(streaming_only_where_it_is_safe) {
    OptionsGuard guard;
    const std::string dir = tempDir("stream_rules");
    writeLongWav(dir + "/long.wav", 2.0, true);
    setMode(StreamMode::Always);
    // looped, pitched far up (needs mip levels), started far into the file:
    // kept in memory
    writeText(dir + "/loop.sfz", "<region> sample=long.wav loop_mode=loop_continuous loop_start=1000 loop_end=50000\n");
    writeText(dir + "/high.sfz", "<region> sample=long.wav lokey=0 hikey=127 pitch_keycenter=60\n");
    writeText(dir + "/offset.sfz", "<region> sample=long.wav lokey=55 hikey=65 pitch_keycenter=60 offset=40000\n");
    writeText(dir + "/ok.sfz", "<region> sample=long.wav lokey=0 hikey=66 pitch_keycenter=60\n");
    for (const char* name : {"loop", "high", "offset"}) {
        auto s = load(dir + "/" + name + ".sfz");
        CHECK(s && s->streamedSamples() == 0);
    }
    auto ok = load(dir + "/ok.sfz");
    CHECK(ok && ok->streamedSamples() == 1);
    // Auto: small sets stay in memory
    SampleLoadOptions o;
    o.mode = StreamMode::Auto;
    setSampleLoadOptions(o);
    auto small = load(dir + "/ok.sfz");
    CHECK(small && small->streamedSamples() == 0);
}

TEST(streaming_real_time_pace_and_set_swap) {
    // real-time pace (no waiting for the disk): the streamer keeps up; a set
    // swap while streamed notes play releases the slots and frees the set
    OptionsGuard guard;
    const std::string dir = tempDir("stream_rt");
    writeLongWav(dir + "/long.wav", 4.0, false);
    writeText(dir + "/long.sfz", kSfz);
    setMode(StreamMode::Always);
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    e.setParam(P::AmpSustain, 1.0f);
    e.setSampleSet(0, load(dir + "/long.sfz"));
    std::vector<float> L(128), R(128);
    e.process(L.data(), R.data(), 128);  // publish the set
    for (int k = 0; k < 8; ++k) e.sendEvent(Event::noteOn((uint32_t)(k + 1), 56 + k, 0.7f));
    const auto blockTime = std::chrono::microseconds(2667);
    auto next = std::chrono::steady_clock::now();
    int maxStreams = 0;
    for (int b = 0; b < 375; ++b) {  // 1 s
        e.process(L.data(), R.data(), 128);
        e.collectGarbage();
        maxStreams = std::max(maxStreams, e.stats().streamVoices);
        if (b == 300) e.setSampleSet(0, load(dir + "/long.sfz"));  // swap while playing
        next += blockTime;
        std::this_thread::sleep_until(next);
    }
    for (int b = 0; b < 40; ++b) {
        e.process(L.data(), R.data(), 128);
        e.collectGarbage();
    }
    const EngineStats st = e.stats();
    INFO("8 streamed notes at real-time pace: max %d streams, %u underruns, %llu KB read\n", maxStreams, st.streamUnderruns,
         (unsigned long long)(st.streamBytesRead / 1024));
    CHECK(maxStreams == 8);
    CHECK(st.streamUnderruns == 0);
    CHECK(st.streamVoices == 0);  // the swap stopped the old notes
}

TEST(streaming_leading_silence_trim) {
    OptionsGuard guard;
    const std::string dir = tempDir("stream_trim");
    // 2.0 s stereo WAV with 600 frames (~12.5 ms) of leading silence before onset
    constexpr int kSr = 48000;
    constexpr int kSilence = 600;
    constexpr int kLen = 2 * kSr;
    std::vector<float> l((size_t)kLen, 0.0f), r((size_t)kLen, 0.0f);
    for (int i = kSilence; i < kLen; ++i) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * (float)(i - kSilence) / kSr);
        l[(size_t)i] = v;
        r[(size_t)i] = -v;
    }
    std::string err;
    CHECK(writeWav(dir + "/pad.wav", l, &r, kSr, false, err));
    writeText(dir + "/pad.sfz", "<region> sample=pad.wav lokey=0 hikey=127 pitch_keycenter=60\n");
    writeText(dir + "/pad_loop.sfz",
              "<region> sample=pad.wav lokey=0 hikey=127 pitch_keycenter=60 loop_mode=loop_continuous loop_start=200 loop_end=20000\n");

    setMode(StreamMode::Off);
    auto ram = load(dir + "/pad.sfz");
    CHECK(ram && ram->regions.size() == 1);
    // onset is at kSilence + 1 (since sin(0) == 0); lead-in is 8 frames -> trimmed near 593
    CHECK(ram->regions[0].offset >= kSilence - 12 && ram->regions[0].offset <= kSilence);

    // Looped sample whose loopStart (200) is before onset must NOT trim past loopStart
    auto looped = load(dir + "/pad_loop.sfz");
    CHECK(looped && looped->regions.size() == 1);
    CHECK(looped->regions[0].offset == 0);

    // Streamed WAV trims to the exact same offset and renders bit-identically to RAM
    setMode(StreamMode::Always);
    auto st = load(dir + "/pad.sfz");
    CHECK(st && st->regions.size() == 1);
    CHECK(st->regions[0].offset == ram->regions[0].offset);
    std::vector<float> ramL, ramR, stL, stR;
    render(std::move(ram), ramL, ramR, nullptr, 0.5);
    EngineStats stats{};
    render(std::move(st), stL, stR, &stats, 0.5);
    CHECK(stats.streamUnderruns == 0);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < ramL.size(); ++i)
        maxDiff = std::max(maxDiff, std::max(std::fabs(ramL[i] - stL[i]), std::fabs(ramR[i] - stR[i])));
    CHECK(maxDiff == 0.0f);
}
