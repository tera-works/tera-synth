// Measured behaviour of the v0.6 effect blocks (fx_extra.cpp) and the Juno
// chorus modes. The generic all-block tests in test_fx.cpp cover defaults,
// every enum option, extreme / random settings and other sample rates.
#include <cmath>
#include <memory>

#include "analysis.h"
#include "hsynth/engine.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

constexpr float kSr = 48000.0f;

struct Rig {
    int type;
    std::unique_ptr<fx::FxBlock> b;
    std::vector<float> p;
    fx::FxContext c;
    explicit Rig(int t) : type(t), b(fx::createBlock(t)), p((size_t)P::kNumParams) {
        defaultParams(p.data());
        p[(size_t)P::fxp(t, 0)] = 1.0f;
        c.sampleRate = kSr;
        b->prepare(kSr);
        b->update(at(), c);
    }
    float* at() { return &p[(size_t)P::fxp(type, 0)]; }
    Rig& set(int local, float v) {
        p[(size_t)P::fxp(type, local)] = sanitizeParam(P::fxp(type, local), v);
        b->update(at(), c);
        return *this;
    }
    void run(std::vector<float>& L, std::vector<float>& R, int block = 64) {
        for (size_t off = 0; off < L.size(); off += (size_t)block) {
            const int n = (int)std::min<size_t>((size_t)block, L.size() - off);
            b->process(&L[off], &R[off], n, c);
            c.beat += (double)c.tempo / 60.0 * n / c.sampleRate;
        }
    }
};

std::vector<float> sine(float hz, float amp, size_t n) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = amp * std::sin(kTwoPi * hz * (float)i / kSr);
    return x;
}
double amp(const std::vector<float>& x, double f, size_t from, size_t n) { return goertzel(x, kSr, f, from, n); }
double db(double g) { return 20.0 * std::log10(std::max(g, 1e-12)); }

}  // namespace

TEST(fx6_isolator_kills_a_band) {
    for (float f : {100.0f, 1000.0f, 8000.0f}) {
        Rig r(fx::Isolator);
        r.set(fx::isolator::Low, -60);
        auto L = sine(f, 0.3f, 48000), R = L;
        r.run(L, R);
        const double g = db(amp(L, f, 24000, 24000) / 0.3);
        INFO("isolator low killed: %.0f Hz -> %.1f dB\n", f, g);
        if (f < 200) CHECK(g < -30.0);
        else CHECK(g > -3.0 && g < 1.5);
    }
}

TEST(fx6_deesser_only_reduces_sibilance) {
    auto run = [](float f) {
        Rig r(fx::DeEsser);
        r.set(fx::deesser::Freq, 7000).set(fx::deesser::Thresh, -40).set(fx::deesser::Range, 18);
        auto L = sine(f, 0.4f, 48000), R = L;
        r.run(L, R);
        return db(amp(L, f, 24000, 24000) / 0.4);
    };
    const double s = run(7000), v = run(300);
    INFO("de-esser: 7 kHz %.1f dB, 300 Hz %.1f dB\n", s, v);
    CHECK(s < -8.0);
    CHECK(std::fabs(v) < 1.0);
}

TEST(fx6_multitap_tap_positions) {
    Rig r(fx::MultiTap);
    r.set(fx::multitap::Sync, 0).set(fx::multitap::Time, 400).set(fx::multitap::Feedback, 0).set(fx::multitap::Mix, 1)
        .set(fx::multitap::Tone, 20000);
    std::vector<float> L(48000, 0.0f), R(48000, 0.0f);
    L[1000] = R[1000] = 1.0f;
    r.run(L, R);
    // taps at 0.25 / 0.5 / 0.75 / 1.0 of 400 ms
    for (int k = 1; k <= 4; ++k) {
        const size_t at = 1000 + (size_t)(0.1 * k * 48000);
        float pk = 0;
        for (size_t i = at - 3; i <= at + 3; ++i) pk = std::max(pk, std::fabs(L[i]) + std::fabs(R[i]));
        CHECK(pk > 0.2f);
    }
}

TEST(fx6_pump_ducks_on_the_beat) {
    Rig r(fx::Pump);
    r.c.tempo = 120.0f;
    r.set(fx::pump::Division, 2).set(fx::pump::Depth, 0.9f);  // quarter notes = 0.5 s
    std::vector<float> L(48000, 0.5f), R(48000, 0.5f);
    r.run(L, R);
    const float onBeat = L[24000 + 600], late = L[24000 + 20000];
    INFO("pump: just after the beat %.3f, late in the beat %.3f\n", onBeat, late);
    CHECK(onBeat < 0.2f);
    CHECK(late > 0.4f);
}

TEST(fx6_tape_stop_slows_to_silence) {
    Rig r(fx::TapeStop);
    r.set(fx::tapestop::Time, 0.5f).set(fx::tapestop::Stop, 1);
    auto L = sine(440, 0.3f, 48000), R = L;
    r.run(L, R);
    CHECK(rms(L, 2000, 6000) > 0.1);      // still playing (slowing down)
    CHECK(rms(L, 30000, 48000) < 1e-3);   // stopped after 0.5 s
    r.set(fx::tapestop::Stop, 0);        // released: live signal again
    auto L2 = sine(440, 0.3f, 9600), R2 = L2;
    r.run(L2, R2);
    CHECK_NEAR(rms(L2, 4800, 9600), 0.3 / std::sqrt(2.0), 0.01);
}

TEST(fx6_resonator_rings_at_the_chord) {
    Rig r(fx::Resonator);
    r.set(fx::resonator::KeyFollow, 0).set(fx::resonator::Root, 57).set(fx::resonator::Chord, 3)
        .set(fx::resonator::Decay, 2).set(fx::resonator::Mix, 1);
    std::vector<float> L(48000, 0.0f), R(48000, 0.0f);
    Rng rng(3);
    for (size_t i = 0; i < 480; ++i) L[i] = R[i] = 0.5f * rng.bipolar();
    r.run(L, R);
    // A3 major: 220, 277.2, 329.6, 440 Hz ring on; an off-chord frequency does not
    const double a = amp(L, 220.0, 9600, 19200), c = amp(L, 277.18, 9600, 19200), off = amp(L, 250.0, 9600, 19200);
    INFO("resonator: 220 %.4f  277 %.4f  250 (off) %.4f\n", a, c, off);
    CHECK(a > 5.0 * off && c > 5.0 * off);
    CHECK(allFinite(L) && peak(L) < 2.0);
}

TEST(fx6_reverse_reverb_swells_gated_cuts) {
    auto env = [](int mode, size_t from, size_t to) {
        Rig r(fx::GatedVerb);
        r.set(fx::gatedverb::Mode, (float)mode).set(fx::gatedverb::Size, 400).set(fx::gatedverb::PreDelay, 0)
            .set(fx::gatedverb::Mix, 1);
        std::vector<float> L(48000, 0.0f), R(48000, 0.0f);
        L[0] = R[0] = 1.0f;
        r.run(L, R);
        return rms(L, from, to);
    };
    // reverse: the end of the 400 ms window is louder than the start
    CHECK(env(1, 15000, 19000) > 3.0 * env(1, 500, 4500));
    // gated: flat inside the window, nothing after it
    const double in = env(0, 2000, 18000), after = env(0, 21000, 30000);
    INFO("gated: inside %.4f, after %.6f\n", in, after);
    CHECK(in > 0.005 && after < in * 0.02);
}

TEST(fx6_granular_and_step_filter_and_talkbox) {
    {
        Rig r(fx::Granular);
        r.set(fx::granular::Pitch, 12).set(fx::granular::Spray, 0).set(fx::granular::Mix, 1);
        auto L = sine(220, 0.3f, 96000), R = L;
        r.run(L, R);
        const double up = amp(L, 440, 48000, 48000), orig = amp(L, 220, 48000, 48000);
        INFO("granular +12: 440 Hz %.4f, 220 Hz %.4f\n", up, orig);
        CHECK(up > 3.0 * orig && allFinite(L));
    }
    {  // the cutoff follows the steps: brightness changes from step to step
        Rig r(fx::StepFilter);
        r.c.tempo = 120.0f;
        r.set(fx::stepfilter::Glide, 0).set(fx::stepfilter::Reso, 0.2f);
        std::vector<float> L(48000), R(48000);
        Rng rng(5);
        for (size_t i = 0; i < L.size(); ++i) L[i] = R[i] = 0.3f * rng.bipolar();
        r.run(L, R);
        // 1/16 at 120 bpm = 6000 samples per step; step 1 = 1.0 (open), step 2 = 0.2
        const double s1 = rms(L, 1000, 5500), s2 = rms(L, 7000, 11500);
        INFO("step filter: step 1 %.4f, step 2 %.4f\n", s1, s2);
        CHECK(s1 > 1.5 * s2);
    }
    {
        Rig r(fx::Talkbox);
        r.set(fx::talkbox::Mode, 0);
        auto L = sine(800, 0.2f, 24000), R = L;
        auto L2 = L, R2 = R;
        r.run(L, R);  // "a": formant at 800 Hz passes
        Rig r2(fx::Talkbox);
        r2.set(fx::talkbox::Mode, 0).set(fx::talkbox::Vowel, 0.5f);  // "i": 300 / 2200 Hz
        r2.run(L2, R2);
        CHECK(rms(L, 12000, 24000) > 3.0 * rms(L2, 12000, 24000));
    }
}

TEST(fx6_juno_chorus_and_doubler_widen) {
    for (int mode : {3, 4, 5}) {
        Rig r(fx::Chorus);
        r.set(fx::chorus::Mode, (float)mode).set(fx::chorus::Mix, 0.5f);
        auto L = sine(440, 0.3f, 48000), R = L;
        r.run(L, R);
        double side = 0;
        for (size_t i = 24000; i < 48000; ++i) side += std::fabs(L[i] - R[i]);
        CHECK(side / 24000 > 0.005);  // opposite-phase sweep: stereo from a mono input
        CHECK(allFinite(L) && peak(L) < 1.0);
    }
    Rig d(fx::Doubler);
    auto L = sine(440, 0.3f, 48000), R = L;
    d.run(L, R);
    double side = 0;
    for (size_t i = 24000; i < 48000; ++i) side += std::fabs(L[i] - R[i]);
    CHECK(side / 24000 > 0.005);
}

TEST(fx6_swell_fades_notes_in) {
    Rig r(fx::Swell);
    r.set(fx::swell::Attack, 500).set(fx::swell::Thresh, -40);
    std::vector<float> L(48000, 0.0f), R(48000, 0.0f);
    for (size_t i = 4800; i < 48000; ++i) L[i] = R[i] = 0.3f * std::sin(kTwoPi * 330.0f * (float)i / kSr);
    r.run(L, R);
    const double early = rms(L, 5000, 7000), late = rms(L, 40000, 44000);
    INFO("swell: 5 ms after the onset %.4f, 0.8 s after %.4f\n", early, late);
    CHECK(early < 0.2 * late);
    CHECK_NEAR(late, 0.3 / std::sqrt(2.0), 0.01);
}
