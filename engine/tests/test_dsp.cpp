#include "analysis.h"
#include "hsynth/dsp/envelope.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/dsp/oscillators.h"
#include "hsynth/dsp/svf.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

TEST(adsr_stages) {
    Adsr e;
    e.setSampleRate(48000);
    e.set(0.01f, 0.1f, 0.5f, 0.2f);
    e.gate(true);
    int attackSamples = 0;
    while (e.stage() == Adsr::Stage::Attack && attackSamples < 48000) { e.process(); ++attackSamples; }
    // analog-style attack reaches 1.0 a little earlier than the nominal time
    CHECK(attackSamples > 200 && attackSamples <= 500);
    for (int i = 0; i < 48000; ++i) e.process();
    CHECK(e.stage() == Adsr::Stage::Sustain);
    CHECK_NEAR(e.level(), 0.5, 1e-4);
    e.gate(false);
    int rel = 0;
    while (e.active() && rel < 48000 * 2) { e.process(); ++rel; }
    CHECK(!e.active());
    CHECK(rel > 4800 && rel < 48000);
}

TEST(va_oscillator_frequency_and_dc) {
    const float sr = 48000;
    for (int w = 0; w < (int)VaWave::Count; ++w) {
        VaOsc o;
        std::vector<float> x(48000);
        for (auto& v : x) v = o.tick((VaWave)w, 441.0f / sr, 0.3f);
        double mean = 0;
        for (float v : x) mean += v;
        mean /= (double)x.size();
        CHECK(std::fabs(mean) < 0.02);
        CHECK(peak(x) < 1.5);  // DC-corrected pulse reaches 1 + |2pw-1|
        if (w != (int)VaWave::Pulse) CHECK_NEAR(zeroCrossFreq(x, sr, 1000, x.size()), 441.0, 1.0);
    }
}

TEST(polyblep_reduces_aliasing) {
    // 4567 Hz saw: count energy at a non-harmonic frequency region (alias)
    const float sr = 48000, f = 4567;
    std::vector<float> blep(16384), naive(16384);
    VaOsc o;
    float ph = 0;
    for (size_t i = 0; i < blep.size(); ++i) {
        blep[i] = o.tick(VaWave::Saw, f / sr, 0.5f);
        naive[i] = 2.0f * ph - 1.0f;
        ph += f / sr;
        if (ph >= 1) ph -= 1;
    }
    // aliased harmonics: k*f folded. Measure several fold-back components
    double aliasB = 0, aliasN = 0;
    for (int k = 6; k <= 20; ++k) {
        double fk = std::fmod(k * (double)f, (double)sr);
        if (fk > sr / 2) fk = sr - fk;
        // skip if near a true harmonic
        bool nearHarm = false;
        for (int h = 1; h * f < sr / 2; ++h)
            if (std::fabs(fk - h * f) < 60) nearHarm = true;
        if (nearHarm) continue;
        aliasB += goertzel(blep, sr, fk, 0, 16384);
        aliasN += goertzel(naive, sr, fk, 0, 16384);
    }
    INFO("alias energy blep=%.5f naive=%.5f\n", aliasB, aliasN);
    CHECK(aliasB < aliasN * 0.5);
}

TEST(svf_lowpass_response) {
    const float sr = 48000;
    auto measure = [&](FilterType t, float freq) {
        VoiceFilter f;
        f.set(t, 1000.0f, 0.0f, 0.0f, sr);
        std::vector<float> y(24000);
        for (size_t i = 0; i < y.size(); ++i) y[i] = f.process(std::sin(2.0f * kPi * freq * (float)i / sr));
        return rms(y, 12000, 24000) / 0.70710678;
    };
    CHECK(measure(FilterType::LP24, 100.0f) > 0.9);
    CHECK(measure(FilterType::LP24, 10000.0f) < 0.002);  // > 54 dB down
    CHECK(measure(FilterType::LP12, 10000.0f) < 0.02);
    CHECK(measure(FilterType::HP, 100.0f) < 0.02);
    CHECK(measure(FilterType::HP, 10000.0f) > 0.9);
    CHECK(measure(FilterType::BP, 1000.0f) > 0.9);
    CHECK(measure(FilterType::Notch, 1000.0f) < 0.05);
}

TEST(svf_stable_under_modulation) {
    VoiceFilter f;
    Rng r;
    float maxAbs = 0;
    for (int b = 0; b < 3000; ++b) {
        f.set(FilterType::LP24, 20.0f + r.unipolar() * 20000.0f, r.unipolar(), r.unipolar(), 48000);
        for (int i = 0; i < 16; ++i) {
            const float y = f.process(r.bipolar());
            CHECK(std::isfinite(y));
            maxAbs = std::fmax(maxAbs, std::fabs(y));
        }
    }
    CHECK(maxAbs < 50.0f);
}

TEST(fm_zero_index_is_sine) {
    float p[P::kNumParams];
    defaultParams(p);
    p[P::layer(0, P::FmAlgo)] = 0;
    FmVoice v;
    v.setSampleRate(48000);
    v.noteOn(1.0f);
    v.control(p, 0);
    std::vector<float> y(48000);
    for (size_t i = 0; i < y.size(); i += kControlBlock) v.render(&y[i], kControlBlock, 440.0f / 48000.0f, 440.0f / 48000.0f, 0.0f, 0.0f);
    CHECK_NEAR(zeroCrossFreq(y, 48000, 4800, y.size()), 440.0, 0.5);
    const double fund = goertzel(y, 48000, 440, 4800, 32768);
    const double h2 = goertzel(y, 48000, 880, 4800, 32768);
    CHECK(h2 < fund * 0.01);
}

TEST(fm_modulation_adds_sidebands) {
    float p[P::kNumParams];
    defaultParams(p);
    p[P::layer(0, P::FmAlgo)] = 0;
    for (int op = 0; op < P::kFmOps; ++op) {
        p[P::fmOp(0, op, P::OpSustain)] = 1.0f;
        p[P::fmOp(0, op, P::OpLevel)] = op <= 1 ? 1.0f : 0.0f;
    }
    FmVoice v;
    v.setSampleRate(48000);
    v.noteOn(1.0f);
    v.control(p, 0);
    std::vector<float> y(48000);
    for (size_t i = 0; i < y.size(); i += kControlBlock) v.render(&y[i], kControlBlock, 440.0f / 48000.0f, 440.0f / 48000.0f, 1.0f, 0.0f);
    CHECK(goertzel(y, 48000, 880, 4800, 32768) > 0.05);
    CHECK(allFinite(y));
}


// ---------------------------------------------------------------- wavetable

#include "hsynth/dsp/wavetable.h"
#include "hsynth/wav.h"

TEST(wavetable_builtins_and_pitch) {
    CHECK(builtinWavetableCount() == 8);
    const Wavetable& basic = builtinWavetable(0);
    CHECK(basic.frames == 64);
    WtOsc osc;
    Rng rng(1);
    osc.reset(rng, 1);
    std::vector<float> L(48000), R(48000);
    for (size_t i = 0; i < L.size(); i += 16) osc.render(basic, 0.0f, 440.0f / 48000.0f, WtWarp::None, 0, 1, 0, 0, &L[i], &R[i], 16);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 43000), 440.0, 0.5);  // position 0 = sine
    INFO("sine frame peak %.3f\n", peak(L));
    CHECK(peak(L) > 0.95 && peak(L) <= 1.0);  // built-in frames are normalised one by one
}

TEST(wavetable_band_limited) {
    // saw frame (basic, position 0.5) at 4567 Hz: harmonics above Nyquist must not fold back
    const Wavetable& basic = builtinWavetable(0);
    WtOsc osc;
    Rng rng(1);
    osc.reset(rng, 1);
    const double f0 = 4567.0;
    std::vector<float> L(48000), R(48000);
    for (size_t i = 0; i < L.size(); i += 16) osc.render(basic, 0.5f, (float)(f0 / 48000.0), WtWarp::None, 0, 1, 0, 0, &L[i], &R[i], 16);
    double harm = 0, alias = 0;
    for (int k = 1; k * f0 < 24000; ++k) harm += goertzel(L, 48000, k * f0, 4800, 32768);
    for (double f = 300; f < 23500; f += 97.0) {
        const double r = std::fmod(f, f0) / f0;
        if (r > 0.02 && r < 0.98) alias += goertzel(L, 48000, f, 4800, 32768);
    }
    INFO("wavetable harmonics %.4f, alias probe sum %.5f\n", harm, alias);
    CHECK(alias < 0.01 * harm);
}

TEST(wavetable_wav_import) {
    const std::string dir = std::string(HSYNTH_TEST_DATA) + "/../../build-desktop";
    // two 2048-sample frames (sine, square)
    std::vector<float> two(4096);
    for (int i = 0; i < 2048; ++i) {
        two[(size_t)i] = std::sin(kTwoPi * i / 2048.0f);
        two[(size_t)(2048 + i)] = i < 1024 ? 0.8f : -0.8f;
    }
    std::string err;
    CHECK(writeWav(dir + "/wt_two.wav", two, nullptr, 48000, true, err));
    auto wt = loadWavetableFile(dir + "/wt_two.wav", err);
    CHECK(wt != nullptr);
    if (wt) CHECK(wt->frames == 2);
    // a 600-sample single cycle (AKWF style) becomes one resampled frame
    std::vector<float> one(600);
    for (int i = 0; i < 600; ++i) one[(size_t)i] = 2.0f * i / 600.0f - 1.0f;
    CHECK(writeWav(dir + "/wt_one.wav", one, nullptr, 44100, false, err));
    auto w1 = loadWavetableFile(dir + "/wt_one.wav", err);
    CHECK(w1 != nullptr);
    if (w1) {
        CHECK(w1->frames == 1);
        WtOsc osc;
        Rng rng(1);
        osc.reset(rng, 1);
        std::vector<float> L(24000), R(24000);
        for (size_t i = 0; i < L.size(); i += 16) osc.render(*w1, 0, 220.0f / 48000.0f, WtWarp::None, 0, 1, 0, 0, &L[i], &R[i], 16);
        CHECK(goertzel(L, 48000, 220, 2400, 16384) > 0.3);
    }
}
