// Effect rack tests: every block under default / extreme / random settings,
// measured behaviour of individual effects, and rack features (bypass
// crossfade, spillover, FX modulation, tempo sync, looper, user IRs, legacy
// presets).
#include <chrono>
#include <cstring>
#include <memory>
#include <set>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/fx/convolver.h"
#include "hsynth/fx/ir_load.h"
#include "hsynth/host_utils.h"
#include "hsynth/preset.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

constexpr float kSr = 48000.0f;

// One block driven directly with its slice of a full parameter array.
struct Rig {
    int type;
    std::unique_ptr<fx::FxBlock> b;
    std::vector<float> p;
    fx::FxContext c;
    explicit Rig(int t, float sr = kSr) : type(t), b(fx::createBlock(t)), p((size_t)P::kNumParams) {
        defaultParams(p.data());
        p[(size_t)P::fxp(t, 0)] = 1.0f;
        c.sampleRate = sr;
        b->prepare(sr);
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

std::vector<float> sine(float hz, float amp, size_t n, float sr = kSr) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = amp * std::sin(kTwoPi * hz * (float)i / sr);
    return x;
}

// Test signal: saw chord, a noise burst, then silence.
void testSignal(std::vector<float>& L, std::vector<float>& R, size_t n) {
    L.assign(n, 0.0f);
    R.assign(n, 0.0f);
    Rng rng(7);
    float ph[3] = {0, 0, 0};
    const float f[3] = {110.0f, 138.6f, 164.8f};
    for (size_t i = 0; i < n && i < (size_t)kSr; ++i) {
        float s = 0;
        for (int k = 0; k < 3; ++k) { ph[k] += f[k] / kSr; if (ph[k] >= 1) ph[k] -= 1; s += 2 * ph[k] - 1; }
        L[i] = 0.15f * s;
        R[i] = 0.12f * s;
    }
    for (size_t i = (size_t)kSr; i < n && i < (size_t)(kSr * 1.3f); ++i) { L[i] = 0.4f * rng.bipolar(); R[i] = 0.4f * rng.bipolar(); }
}

double linToDbTest(double g) { return 20.0 * std::log10(std::max(g, 1e-12)); }
double toneAmp(const std::vector<float>& x, double f, size_t from, size_t n) { return goertzel(x, kSr, f, from, n); }
// Average of short-window amplitudes: for signals whose phase is not
// continuous (grain-based pitch shifting).
double shortAmp(const std::vector<float>& x, double f, size_t from, size_t to) {
    double s = 0;
    int k = 0;
    for (size_t a = from; a + 2048 <= to; a += 2048, ++k) s += goertzel(x, kSr, f, a, 2048);
    return k ? s / k : 0.0;
}

bool selfSustaining(int b) { return b == fx::Lofi; }  // hiss / crackle by design

}  // namespace

TEST(fx_param_layout) {
    const auto& t = paramTable();
    std::set<std::string> names;
    for (const auto& p : t) {
        CHECK(!p.name.empty());
        CHECK(names.insert(p.name).second);  // unique
        CHECK(p.def >= p.min && p.def <= p.max);
    }
    for (int b = 1; b < fx::BlockCount; ++b) {
        const ParamInfo& on = t[(size_t)P::fxp(b, 0)];
        CHECK(on.kind == ParamKind::Bool);
        CHECK(on.name == std::string("fx.") + fx::blockKey(b) + ".on");
        for (int k = 0; k < fx::kBlockParams[b]; ++k)
            CHECK(t[(size_t)P::fxp(b, k)].group == std::string("FX / ") + fx::blockTitle(b));
        CHECK(fx::createBlock(b) != nullptr);
    }
    CHECK(fxModTargets().size() > 200);
    INFO("%d parameters, %d effect blocks, %d FX mod targets\n", P::kNumParams, fx::BlockCount - 1, (int)fxModTargets().size() - 1);
}

TEST(fx_all_blocks_defaults_finite_and_decay) {
    for (int b = 1; b < fx::BlockCount; ++b) {
        Rig r(b);
        std::vector<float> L, R;
        testSignal(L, R, (size_t)(kSr * 10));
        r.run(L, R);
        const bool ok = allFinite(L) && allFinite(R) && peak(L) < 4.0 && peak(R) < 4.0;
        if (!ok) INFO("block %s: peak %.3f\n", fx::blockKey(b), peak(L));
        CHECK(ok);
        if (!selfSustaining(b)) {
            const double tail = rms(L, L.size() - (size_t)kSr, L.size());
            if (tail >= 1e-4) INFO("block %s does not decay: %.6f\n", fx::blockKey(b), tail);
            CHECK(tail < 1e-4);
        }
        // and it must pass audio (not silence) while signal is present
        const double active = rms(L, 4800, 40000);
        if (b != fx::Vocoder && b != fx::Gate) {
            if (active < 0.01) INFO("block %s silent: %.5f\n", fx::blockKey(b), active);
            CHECK(active > 0.01);
        }
    }
}

TEST(fx_all_blocks_every_enum_option) {
    const auto& t = paramTable();
    for (int b = 1; b < fx::BlockCount; ++b) {
        for (int k = 1; k < fx::kBlockParams[b]; ++k) {
            const ParamInfo& info = t[(size_t)P::fxp(b, k)];
            if (info.kind != ParamKind::Enum && info.kind != ParamKind::Bool) continue;
            for (int o = 0; o <= (int)info.max; ++o) {
                Rig r(b);
                r.set(k, (float)o);
                std::vector<float> L, R;
                testSignal(L, R, (size_t)(kSr * 1.5f));
                r.run(L, R, 97);  // odd block size on purpose
                const bool ok = allFinite(L) && allFinite(R) && peak(L) < 6.0;
                if (!ok) INFO("%s = %d: peak %.3f\n", info.name.c_str(), o, peak(L));
                CHECK(ok);
            }
        }
    }
}

TEST(fx_all_blocks_extreme_and_random_settings) {
    const auto& t = paramTable();
    Rng rng(1234);
    for (int b = 1; b < fx::BlockCount; ++b) {
        for (int trial = 0; trial < 14; ++trial) {
            Rig r(b);
            for (int k = 1; k < fx::kBlockParams[b]; ++k) {
                const ParamInfo& info = t[(size_t)P::fxp(b, k)];
                float v;
                if (trial == 0) v = info.min;
                else if (trial == 1) v = info.max;
                else v = info.min + rng.unipolar() * (info.max - info.min);
                r.p[(size_t)P::fxp(b, k)] = sanitizeParam(P::fxp(b, k), v);
            }
            r.b->update(r.at(), r.c);
            std::vector<float> L, R;
            testSignal(L, R, (size_t)(kSr * 3));
            r.run(L, R, 128);
            // gain parameters at their limits may legitimately add 40+ dB;
            // this catches instability (runaway feedback, NaN)
            const bool ok = allFinite(L) && allFinite(R) && peak(L) < 1000.0 && peak(R) < 1000.0;
            if (!ok) INFO("block %s trial %d: peak %.3f finite %d\n", fx::blockKey(b), trial, peak(L), (int)allFinite(L));
            CHECK(ok);
        }
    }
}

TEST(fx_all_blocks_other_sample_rates) {
    for (float sr : {44100.0f, 96000.0f}) {
        for (int b = 1; b < fx::BlockCount; ++b) {
            Rig r(b, sr);
            std::vector<float> L, R;
            testSignal(L, R, (size_t)(kSr * 2));
            r.run(L, R);
            CHECK(allFinite(L) && allFinite(R) && peak(L) < 4.0);
        }
    }
}

TEST(fx_parametric_eq_gain) {
    Rig r(fx::Peq);
    r.set(fx::peq::P2Freq, 1000).set(fx::peq::P2Gain, 12).set(fx::peq::P2Q, 1);
    auto L = sine(1000, 0.1f, 48000), R = L;
    r.run(L, R);
    CHECK_NEAR(toneAmp(L, 1000, 24000, 24000) / 0.1, 3.981, 0.08);
    Rig r2(fx::Peq);
    r2.set(fx::peq::P2Freq, 1000).set(fx::peq::P2Gain, 12);
    auto L2 = sine(60, 0.1f, 48000), R2 = L2;
    r2.run(L2, R2);
    CHECK_NEAR(toneAmp(L2, 60, 24000, 24000) / 0.1, 1.0, 0.05);
}

TEST(fx_graphic_eq_band) {
    Rig r(fx::Geq);
    r.set(fx::geq::B1k, -12);
    auto L = sine(1000, 0.2f, 48000), R = L;
    r.run(L, R);
    CHECK_NEAR(linToDbTest(toneAmp(L, 1000, 24000, 24000) / 0.2), -12.0, 0.5);
}

TEST(fx_delay_echo_position) {
    Rig r(fx::Delay);
    r.c.tempo = 120.0f;
    r.set(fx::delay::Mode, 0).set(fx::delay::Sync, (float)fx::SyncDiv::D1_8).set(fx::delay::Mix, 1.0f)
        .set(fx::delay::Feedback, 0).set(fx::delay::ModDepth, 0).set(fx::delay::HighCut, 20000).set(fx::delay::LowCut, 20);
    // let the delay-time glide settle on 0.25 s before the impulse
    std::vector<float> L(48000 * 2, 0.0f), R(48000 * 2, 0.0f);
    const size_t imp = 48000;
    L[imp] = R[imp] = 1.0f;
    r.run(L, R);
    size_t best = 0;
    for (size_t i = imp + 100; i < L.size(); ++i)
        if (std::fabs(L[i]) > std::fabs(L[best])) best = i;
    CHECK_NEAR((double)(best - imp), 12000.0, 2.0);  // 1/8 note at 120 bpm = 0.25 s
}

TEST(fx_pitch_shift_octave_up) {
    Rig r(fx::Pitch);
    r.set(fx::pitch::Semi1, 12).set(fx::pitch::Mix, 1.0f);
    auto L = sine(220, 0.3f, 48000), R = L;
    r.run(L, R);
    const double up = shortAmp(L, 440, 9600, 48000), orig = shortAmp(L, 220, 9600, 48000);
    INFO("pitch +12: 440 Hz %.4f, 220 Hz %.4f\n", up, orig);
    CHECK(up > 0.1);
    CHECK(up > 8.0 * orig);
}

TEST(fx_frequency_shifter_and_ring) {
    Rig r(fx::RingMod);
    r.set(fx::ringmod::Mode, 1).set(fx::ringmod::Freq, 100).set(fx::ringmod::Mix, 1.0f);
    auto L = sine(1000, 0.3f, 48000), R = L;
    r.run(L, R);
    const double up = toneAmp(L, 1100, 4800, 38400), down = toneAmp(L, 900, 4800, 38400), orig = toneAmp(L, 1000, 4800, 38400);
    INFO("freq shift: 1100 %.4f  900 %.4f  1000 %.4f\n", up, down, orig);
    CHECK(up > 0.2);
    CHECK(up > 30.0 * down);  // single sideband (> 30 dB rejection)
    CHECK(up > 30.0 * orig);
    Rig g(fx::RingMod);
    g.set(fx::ringmod::Freq, 300).set(fx::ringmod::Mix, 1.0f);
    auto L2 = sine(1000, 0.3f, 48000), R2 = L2;
    g.run(L2, R2);
    CHECK(toneAmp(L2, 700, 4800, 38400) > 0.1);
    CHECK(toneAmp(L2, 1300, 4800, 38400) > 0.1);
    CHECK(toneAmp(L2, 1000, 4800, 38400) < 0.01);
}

TEST(fx_compressor_static_curve) {
    Rig r(fx::Comp);
    r.set(fx::comp::Thresh, -20).set(fx::comp::Ratio, 4).set(fx::comp::Knee, 0).set(fx::comp::Attack, 1)
        .set(fx::comp::Release, 50).set(fx::comp::Detector, 0).set(fx::comp::ScHpf, 20).set(fx::comp::Mix, 1);
    auto L = sine(1000, 1.0f, 48000), R = L;
    r.run(L, R);
    // 0 dBFS peak, 20 dB over threshold at 4:1 -> -15 dBFS
    double pk = 0;
    for (size_t i = 24000; i < L.size(); ++i) pk = std::max(pk, (double)std::fabs(L[i]));
    CHECK_NEAR(linToDbTest(pk), -15.0, 0.6);
}

TEST(fx_limiter_ceiling) {
    Rig r(fx::Limiter);
    r.set(fx::limiter::Input, 12).set(fx::limiter::Ceiling, -1);
    std::vector<float> L, R;
    testSignal(L, R, 96000);
    r.run(L, R);
    CHECK(peak(L) <= 0.8913 + 1e-4);
    CHECK(peak(R) <= 0.8913 + 1e-4);
    CHECK(rms(L, 4800, 40000) > 0.2);  // still loud (it limits, not mutes)
}

TEST(fx_octaver_sub_octave) {
    Rig r(fx::Octaver);
    r.set(fx::octaver::Sub1, 1).set(fx::octaver::Dry, 0).set(fx::octaver::Tone, 0.5f);
    auto L = sine(220, 0.3f, 48000), R = L;
    r.run(L, R);
    const double sub = toneAmp(L, 110, 9600, 32768);
    INFO("octaver sub %.4f\n", sub);
    CHECK(sub > 0.05);
}

TEST(fx_slicer_tempo_gate) {
    Rig r(fx::Slicer);
    r.c.tempo = 120.0f;
    r.set(fx::slicer::Division, 3).set(fx::slicer::Pattern, 0).set(fx::slicer::Duty, 0.5f).set(fx::slicer::Depth, 1)
        .set(fx::slicer::Smooth, 0);
    std::vector<float> L(48000, 1.0f), R(48000, 1.0f);
    r.run(L, R, 64);
    int rises = 0;
    for (size_t i = 1; i < L.size(); ++i)
        if (L[i - 1] < 0.5f && L[i] >= 0.5f) ++rises;
    CHECK(rises == 7 || rises == 8);  // 1/16 at 120 bpm = 8 steps per second
}

TEST(fx_convolver_matches_direct) {
    const int len = 6000, n = 16000;
    std::vector<float> h(len), x(n);
    Rng rng(3);
    for (int i = 0; i < len; ++i) h[(size_t)i] = rng.bipolar() * std::exp(-(float)i / 1500.0f);
    for (int i = 0; i < n; ++i) x[(size_t)i] = rng.bipolar();
    auto ir = fx::ConvIr::build(h.data(), nullptr, len, fx::ConvIr::MonoIrStereoIn);
    fx::Convolver c;
    c.prepare(1 << 15);
    c.setIr(ir.get());
    std::vector<float> L = x, R(n, 0.0f);
    for (int off = 0; off < n; off += 77) c.process(&L[(size_t)off], &R[(size_t)off], std::min(77, n - off));
    double err = 0;
    for (int i = 0; i < n; i += 11) {
        double y = 0;
        for (int t = 0; t < len && t <= i; ++t) y += h[(size_t)t] * x[(size_t)(i - t)];
        err = std::max(err, std::fabs(y - L[(size_t)i]));
    }
    CHECK(err < 1e-4);
}

// ---------------------------------------------------------------- engine-level

namespace {
void sawVoice(Engine& e) {
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::AmpSustain, 1.0f);
    e.setParam(P::mod(0, P::ModAmt), 0.0f);
    for (int s = 0; s < fx::kFxSlots; ++s) e.setParam(P::fxSlot(s), (float)fx::None);
}
}  // namespace

TEST(fx_rack_bypass_is_click_free) {
    Engine e;
    e.prepare(kSr);
    sawVoice(e);
    e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
    e.setParam(P::layer(0, P::VaMix), 0.0f);
    e.setParam(P::fxSlot(0), (float)fx::Peq);
    e.setParam(P::fxp(fx::Peq, fx::peq::P2Freq), 440.0f);
    e.setParam(P::fxp(fx::Peq, fx::peq::P2Gain), 12.0f);
    e.setParam(P::fxp(fx::Peq, fx::peq::On), 0.0f);
    std::vector<float> L(96000), R(96000);
    e.sendEvent(Event::noteOn(1, 69, 0.8f));
    for (int off = 0; off < 96000; off += 128) {
        if (off == 48000) e.setParam(P::fxp(fx::Peq, fx::peq::On), 1.0f);
        e.process(&L[(size_t)off], &R[(size_t)off], 128);
    }
    const double amp = peak(L);
    double maxStep = 0;
    for (size_t i = 24000; i < L.size(); ++i) maxStep = std::max(maxStep, (double)std::fabs(L[i] - L[i - 1]));
    // a 440 Hz sine of amplitude A changes by at most A * 2 pi f / sr per sample
    CHECK(maxStep < amp * kTwoPi * 440.0 / kSr * 1.1);
    CHECK(rms(L, 72000, 96000) > 2.0 * rms(L, 24000, 47000));  // and the EQ did engage
}

TEST(fx_reverb_spillover_tail) {
    Engine e;
    e.prepare(kSr);
    sawVoice(e);
    e.setParam(P::fxSlot(0), (float)fx::Reverb);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::Mix), 0.4f);
    RenderJob job;
    job.seconds = 1.5;
    addNote(job, 0.0, 0.3, 60, 0.8f);
    std::vector<float> L(72064), R(72064);
    e.sendEvent(Event::noteOn(1, 60, 0.8f));
    for (int off = 0; off < 72064; off += 128) {
        if (off == 14336) e.sendEvent(Event::noteOff(1, 60, 0));
        if (off == 24064) e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);  // switch off during the tail
        e.process(&L[(size_t)off], &R[(size_t)off], 128);
    }
    CHECK(rms(L, 33600, 38400) > 1e-3);  // tail keeps ringing after the switch
}

TEST(fx_modwheel_drives_filter_cutoff) {
    auto render = [](float wheel) {
        Engine e;
        e.prepare(kSr);
        sawVoice(e);
        e.setParam(P::fxSlot(0), (float)fx::Filter);
        e.setParam(P::fxp(fx::Filter, fx::filter::Cutoff), 200.0f);
        e.setParam(P::fxMod(0, P::FxModSrc), (float)fx::FxModSource::ModWheel);
        e.setParam(P::fxMod(0, P::FxModDst), 0.0f);
        // find the target index of fx.filter.cutoff
        const auto& targets = fxModTargets();
        for (size_t i = 0; i < targets.size(); ++i)
            if (targets[i] == P::fxp(fx::Filter, fx::filter::Cutoff)) e.setParam(P::fxMod(0, P::FxModDst), (float)i);
        e.setParam(P::fxMod(0, P::FxModAmt), 1.0f);
        e.sendEvent(Event::control(EventType::ModWheel, wheel));
        e.sendEvent(Event::noteOn(1, 45, 0.8f));
        std::vector<float> L(48000), R(48000);
        for (int off = 0; off < 48000; off += 128) e.process(&L[(size_t)off], &R[(size_t)off], 128);
        return goertzel(L, kSr, 1980, 12000, 24000);  // 18th harmonic of 110 Hz
    };
    const double closed = render(0.0f), open = render(1.0f);
    INFO("harmonic at 1980 Hz: wheel 0 %.5f, wheel 1 %.5f\n", closed, open);
    CHECK(open > 20.0 * closed);
}

TEST(fx_looper_records_and_plays) {
    Engine e;
    e.prepare(kSr);
    sawVoice(e);
    std::vector<float> L(48000 * 3), R(48000 * 3);
    e.looperCommand(fx::LooperCmd::Tap);  // record
    e.sendEvent(Event::noteOn(1, 57, 0.8f));
    for (int off = 0; off < (int)L.size(); off += 128) {
        if (off == 24064) e.sendEvent(Event::noteOff(1, 57, 0));
        if (off == 48000) e.looperCommand(fx::LooperCmd::Tap);  // close loop -> play
        e.process(&L[(size_t)off], &R[(size_t)off], 128);
    }
    const auto st = e.looperStatus();
    CHECK(st.state == fx::LooperState::Playing);
    CHECK_NEAR(st.lengthSec, 1.0, 0.01);
    // the second and third seconds replay the recorded note (0.8 level)
    const double first = rms(L, 4800, 20000), replay = rms(L, 48000 + 4800, 48000 + 20000);
    CHECK_NEAR(replay / first, 0.8, 0.1);
    CHECK(rms(L, 48000 * 2 + 4800, 48000 * 2 + 20000) > 0.5 * first);
    e.looperCommand(fx::LooperCmd::Clear);
    std::vector<float> L2(4800), R2(4800);
    for (int off = 0; off < 4800; off += 128) e.process(&L2[(size_t)off], &R2[(size_t)off], std::min(128, 4800 - off));
    CHECK(e.looperStatus().state == fx::LooperState::Empty);
}

TEST(fx_user_cabinet_ir) {
    Engine e;
    e.prepare(kSr);
    sawVoice(e);
    e.setParam(P::fxSlot(0), (float)fx::Cab);
    e.setParam(P::fxp(fx::Cab, fx::cab::Model), 8.0f);  // user ir
    e.setParam(P::fxp(fx::Cab, fx::cab::LowCut), 20.0f);
    e.setParam(P::fxp(fx::Cab, fx::cab::HighCut), 20000.0f);
    // IR = pure delay of 300 samples
    std::vector<float> h(600, 0.0f);
    h[300] = 1.0f;
    e.setFxImpulse(0, fx::makeImpulseResponse(h.data(), nullptr, 600, kSr, kSr, 0, "delay300"));
    CHECK(e.fxImpulseName(0) == "delay300");
    std::vector<float> L(9600), R(9600), D(9600), DR(9600);
    e.sendEvent(Event::noteOn(1, 60, 0.8f));
    for (int off = 0; off < 9600; off += 128) e.process(&L[(size_t)off], &R[(size_t)off], 128);
    e.collectGarbage();
    // reference: same note without the cabinet
    Engine d;
    d.prepare(kSr);
    sawVoice(d);
    d.sendEvent(Event::noteOn(1, 60, 0.8f));
    for (int off = 0; off < 9600; off += 128) d.process(&D[(size_t)off], &DR[(size_t)off], 128);
    // compare after the master-volume start-up ramp (it is applied after the
    // cabinet, so it would not line up with a delayed copy)
    double err = 0;
    for (size_t i = 8000; i < 9600; ++i) err = std::max(err, (double)std::fabs(L[i] - D[i - 300]));
    CHECK(err < 1e-4);
}

TEST(fx_vocoder_follows_input) {
    Rig r(fx::Vocoder);
    std::vector<float> L(48000), R(48000);
    float ph = 0;
    for (size_t i = 0; i < L.size(); ++i) { ph += 110.0f / kSr; if (ph >= 1) ph -= 1; L[i] = R[i] = 0.3f * (2 * ph - 1); }
    std::vector<float> in(48000, 0.0f);
    Rng rng(5);
    for (size_t i = 24000; i < 48000; ++i) in[i] = 0.3f * rng.bipolar();  // "voice" in the second half
    r.c.inL = nullptr;
    for (size_t off = 0; off < L.size(); off += 64) {
        r.c.inL = &in[off];
        r.c.inR = &in[off];
        r.b->process(&L[off], &R[off], 64, r.c);
    }
    const double silent = rms(L, 4800, 23000), voiced = rms(L, 28800, 47000);
    INFO("vocoder: no input %.5f, with input %.5f\n", silent, voiced);
    CHECK(silent < 1e-4);
    CHECK(voiced > 0.02);
}

TEST(fx_legacy_preset_migration) {
    const char* json = R"({"format":"hsynth-preset","version":1,"name":"Old",
        "params":{"chorus.mix":0.6,"chorus.rate":1.5,"delay.mix":0.3,"delay.pingpong":false,"delay.tone":0.6,
                  "reverb.mix":0.3,"reverb.size":0.5,"reverb.predelay":0.02,"eq.low":3.0,"eq.high":0.0}})";
    Preset p;
    std::string err;
    CHECK(presetFromJson(json, p, err));
    CHECK(p.warnings.empty());
    auto v = [&](int b, int l) { return p.params[(size_t)P::fxp(b, l)]; };
    CHECK(v(fx::Chorus, fx::chorus::On) == 1.0f);
    CHECK_NEAR(v(fx::Chorus, fx::chorus::Rate), 1.5, 1e-6);
    CHECK(v(fx::Delay, fx::delay::On) == 1.0f);
    CHECK(v(fx::Delay, fx::delay::Mode) == 0.0f);
    CHECK_NEAR(v(fx::Delay, fx::delay::HighCut), 6800, 300);
    CHECK_NEAR(v(fx::Reverb, fx::reverb::Decay), 2.3, 1e-4);
    CHECK_NEAR(v(fx::Reverb, fx::reverb::PreDelay), 20.0, 1e-3);
    CHECK_NEAR(v(fx::Peq, fx::peq::LsGain), 3.0, 1e-6);
    CHECK((int)p.params[(size_t)P::fxSlot(3)] == fx::Peq);
    // and the new names round-trip
    Preset q;
    CHECK(presetFromJson(presetToJson(p), q, err));
    for (int i = 0; i < P::kNumParams; ++i)
        if (std::fabs(q.params[(size_t)i] - p.params[(size_t)i]) > 1e-4f * (1.0f + std::fabs(p.params[(size_t)i]))) {
            INFO("round trip %s: %g vs %g\n", paramTable()[(size_t)i].name.c_str(), q.params[(size_t)i], p.params[(size_t)i]);
            CHECK(false);
        }
}

TEST(fx_block_cpu_cost) {
    // Informational: time to process 1 s of stereo audio per block (defaults).
    double total = 0;
    for (int b = 1; b < fx::BlockCount; ++b) {
        Rig r(b);
        std::vector<float> L, R;
        testSignal(L, R, (size_t)kSr);
        const auto t0 = std::chrono::steady_clock::now();
        r.run(L, R, 128);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total += ms;
        INFO("%-10s %7.2f ms per second of audio (%.2f%% of one core)\n", fx::blockKey(b), ms, ms / 10.0);
        CHECK(ms < 200.0);
    }
    INFO("all %d blocks in series: %.1f ms per second (%.1f%%)\n", fx::BlockCount - 1, total, total / 10.0);
}
