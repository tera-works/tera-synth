// FLAC decoder: lossless comparison against the synthetic fixture signal
// (engine/tests/data/flac/make_fixtures.py), loading through sample sets,
// robustness against corrupt input.
#include <filesystem>
#include <fstream>

#include "analysis.h"
#include "hsynth/flac.h"
#include "hsynth/host_utils.h"
#include "hsynth/sample_set.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;
namespace fs = std::filesystem;

namespace {

const std::string kDir = std::string(HSYNTH_TEST_DATA) + "/flac/";
constexpr int kN = 9000;

// Same formula as make_fixtures.py.
int64_t fixture(int64_t i, int ch, int bits) {
    const uint64_t x = ((uint64_t)i * 2654435761ull + (uint64_t)ch * 40503ull) & 0xFFFFFFFFull;
    const int64_t noise = (int64_t)((x >> 16) % 201) - 100;
    const int64_t period = ch == 0 ? 100 : 73;
    const int64_t ph = i % period;
    const int64_t tri = ph < period / 2 ? ph * 4 * 8000 / period - 8000 : 24000 - ph * 4 * 8000 / period;
    const int64_t sq = (i % 500) < 250 ? 3000 : -3000;
    int64_t v = tri + noise + (ch == 1 ? sq : 0);
    if (i >= 3000 && i < 3600) v = ch == 0 ? 0 : 1234;
    if (bits == 24) v = v * 256 + (noise & 0xFF);
    return v;
}

std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(flac_decode_lossless) {
    struct Case { const char* file; int channels, bits; };
    const Case cases[] = {{"m16_fixed.flac", 1, 16},      {"m16_lpc.flac", 1, 16},         {"s16_indep.flac", 2, 16},
                          {"s16_left_side.flac", 2, 16},  {"s16_right_side.flac", 2, 16}, {"s16_mid_side.flac", 2, 16},
                          {"s24_lpc.flac", 2, 24}};
    for (const Case& c : cases) {
        WavData w;
        std::string err;
        const bool ok = readFlac(kDir + c.file, w, err);
        if (!ok) INFO("%s: %s\n", c.file, err.c_str());
        CHECK(ok);
        if (!ok) continue;
        CHECK(w.channels == c.channels && w.frames == kN && w.sampleRate == 44100.0);
        const double scale = (double)(1 << (c.bits - 1));
        int bad = 0;
        for (int i = 0; i < kN; ++i)
            for (int ch = 0; ch < c.channels; ++ch) {
                const float v = ch == 0 ? w.left[(size_t)i] : w.right[(size_t)i];
                if ((int64_t)std::llround((double)v * scale) != fixture(i, ch, c.bits)) ++bad;
            }
        if (bad) INFO("%s: %d mismatching samples\n", c.file, bad);
        CHECK(bad == 0);
    }
}

TEST(flac_sample_set_and_sfz) {
    std::string err;
    auto set = loadSampleSet(kDir + "s16_mid_side.flac", err);
    CHECK(set && set->samples.size() == 1 && set->samples[0]->channels == 2 && set->samples[0]->frames == kN);
    const fs::path dir = fs::temp_directory_path() / "hsynth_tests" / "flac_sfz";
    fs::create_directories(dir);
    fs::copy_file(kDir + "m16_lpc.flac", dir / "a.flac", fs::copy_options::overwrite_existing);
    std::ofstream(dir / "t.sfz") << "<region> sample=a.flac key=60\n";
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    std::vector<std::string> warnings;
    CHECK(loadLayerSamples(e, 0, (dir / "t.sfz").generic_string(), err, warnings));
    RenderJob job;
    job.seconds = 0.3;
    addNote(job, 0.0, 0.2, 60, 1.0f);
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK(rms(L, 0, 4800) > 0.01);
    // user wavetable from FLAC
    CHECK(loadLayerWavetable(e, 1, kDir + "m16_fixed.flac", err));
}

TEST(flac_corrupt_input_is_rejected_safely) {
    const auto good = readAll(kDir + "s24_lpc.flac");
    CHECK(good.size() > 1000);
    WavData w;
    std::string err;
    CHECK(!readFlacMemory(good.data(), 3, w, err));
    const uint8_t junk[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    CHECK(!readFlacMemory(junk, sizeof junk, w, err));
    // truncated stream: either an error or the frames before the cut, never a crash
    for (size_t cut : {(size_t)50, good.size() / 3, good.size() - 7}) {
        WavData t;
        if (readFlacMemory(good.data(), cut, t, err)) CHECK(t.frames <= kN);
    }
    // random bit flips
    Rng r(11);
    int decoded = 0;
    for (int k = 0; k < 300; ++k) {
        std::vector<uint8_t> bad = good;
        for (int j = 0; j < 4; ++j) bad[(size_t)(r.unipolar() * (float)(bad.size() - 1))] ^= (uint8_t)(1u << (r.next() % 8));
        WavData t;
        if (readFlacMemory(bad.data(), bad.size(), t, err)) {
            ++decoded;
            CHECK(t.frames <= 4 * kN);
        }
    }
    INFO("corrupted streams decoded (partially) without error: %d / 300\n", decoded);
}
