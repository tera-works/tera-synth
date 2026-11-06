// v0.7: VA sync / cross-mod / sub / supersaw, tonewheel organ, FM pitch EG /
// rate scaling / LFO AM, MSEG, macros, motion lanes, step sequencer
// (sample-accurate scheduling, also with render threads), half pedal, send
// buses, string resonance, scanner vibrato.
#include <cmath>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

constexpr double kSr = 48000.0;

void plain(Engine& e) {
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::AmpSustain, 1.0f);
    e.setParam(P::AmpRelease, 0.01f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setParam(P::mod(0, P::ModAmt), 0.0f);
    e.setParam(P::mod(0, P::ModVia), 0.0f);
    e.setParam(P::mod(1, P::ModAmt), 0.0f);
    e.setParam(P::mod(2, P::ModAmt), 0.0f);
}

std::vector<float> play(Engine& e, int key, double seconds, double hold = -1.0, float vel = 0.8f) {
    RenderJob job;
    job.seconds = seconds;
    addNote(job, 0.0, hold < 0 ? seconds : hold, key, vel);
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    return L;
}

double amp(const std::vector<float>& x, double f, size_t from, size_t n) { return goertzel(x, kSr, f, from, n); }

// first sample index (from `from`) where |x| exceeds thr
size_t onset(const std::vector<float>& x, size_t from, float thr = 0.01f) {
    for (size_t i = from; i < x.size(); ++i)
        if (std::fabs(x[i]) > thr) return i;
    return x.size();
}

}  // namespace

TEST(v07_va_sub_sync_xmod_supersaw) {
    auto render = [](auto setup) {
        Engine e;
        e.prepare(48000);
        plain(e);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaWave2), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        setup(e);
        return play(e, 69, 0.5);
    };
    const auto base = render([](Engine&) {});
    const auto sub = render([](Engine& e) { e.setParam(P::layer(0, P::VaSub), 0.8f); });
    INFO("sub: 220 Hz %.4f -> %.4f\n", amp(base, 220, 4800, 9600), amp(sub, 220, 4800, 9600));
    CHECK(amp(sub, 220, 4800, 9600) > 20.0 * (amp(base, 220, 4800, 9600) + 1e-4));
    // osc 2 alone (mix 1), 1.5 x osc 1, hard-synced: the period follows osc 1 (440 Hz harmonics only)
    const auto sync = render([](Engine& e) {
        e.setParam(P::layer(0, P::VaMix), 1.0f);
        e.setParam(P::layer(0, P::VaWave2), (float)VaWave::Saw);
        e.setParam(P::layer(0, P::VaOsc2Semi), 7.0f);
        e.setParam(P::layer(0, P::VaSync), 1.0f);
    });
    const double h440 = amp(sync, 440, 4800, 19200), h660 = amp(sync, 659.26, 4800, 19200);
    INFO("hard sync: 440 Hz %.4f, 659 Hz (free osc 2) %.4f\n", h440, h660);
    CHECK(h440 > 3.0 * h660);
    const auto xm = render([](Engine& e) {
        e.setParam(P::layer(0, P::VaMix), 1.0f);
        e.setParam(P::layer(0, P::VaXmod), 0.5f);
    });
    CHECK(amp(xm, 880, 4800, 19200) > 0.02);  // FM sidebands
    const auto ss = render([](Engine& e) {
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::SuperSaw);
        e.setParam(P::layer(0, P::VaSsDetune), 0.6f);
    });
    CHECK(allFinite(ss) && rms(ss, 4800, 24000) > 0.05 && peak(ss) <= 1.0);
}

TEST(v07_organ_drawbars_percussion_click) {
    auto render = [](auto setup, double seconds = 0.6) {
        Engine e;
        e.prepare(48000);
        plain(e);
        e.setParam(P::layer(0, P::LType), (float)LayerType::Organ);
        for (int k = 0; k < 9; ++k) e.setParam(P::layer(0, P::OrgDrawbar1 + k), 0.0f);
        e.setParam(P::layer(0, P::OrgClick), 0.0f);
        e.setParam(P::layer(0, P::OrgLeak), 0.0f);
        setup(e);
        return play(e, 69, seconds);
    };
    const auto eight = render([](Engine& e) { e.setParam(P::layer(0, P::OrgDrawbar1 + 2), 8.0f); });
    CHECK_NEAR(zeroCrossFreq(eight, kSr, 4800, 24000), 440.0, 2.0);
    const auto sixteen = render([](Engine& e) {
        e.setParam(P::layer(0, P::OrgDrawbar1 + 2), 8.0f);
        e.setParam(P::layer(0, P::OrgDrawbar1 + 0), 8.0f);
    });
    CHECK(amp(sixteen, 220, 4800, 19200) > 0.5 * amp(sixteen, 440, 4800, 19200));
    const auto perc = render([](Engine& e) {
        e.setParam(P::layer(0, P::OrgDrawbar1 + 2), 8.0f);
        e.setParam(P::layer(0, P::OrgPerc), 2.0f);  // 3rd harmonic
    }, 1.0);
    const double early = amp(perc, 1320, 480, 4800), late = amp(perc, 1320, 33600, 4800);
    INFO("percussion (3rd): early %.4f, after 0.7 s %.5f\n", early, late);
    CHECK(early > 5.0 * late);  // the tonewheel itself has a trace of the 3rd harmonic
    const auto click = render([](Engine& e) {
        e.setParam(P::layer(0, P::OrgDrawbar1 + 2), 8.0f);
        e.setParam(P::layer(0, P::OrgClick), 1.0f);
    });
    // the click is broadband: energy far above the drawbar tones right at the onset
    CHECK(amp(click, 9000, 0, 480) > 5.0 * amp(click, 9000, 24000, 480));
}

TEST(v07_fm_pitch_env_rate_scaling_ams) {
    auto fm = [](Engine& e) {
        plain(e);
        e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
        e.setParam(P::layer(0, P::FmAlgo), 7.0f);  // additive: op 1 only audible
        for (int op = 1; op < P::kFmOps; ++op) e.setParam(P::fmOp(0, op, P::OpLevel), 0.0f);
        e.setParam(P::fmOp(0, 0, P::OpLevel), 1.0f);
        e.setParam(P::fmOp(0, 0, P::OpSustain), 1.0f);
    };
    {
        Engine e;
        e.prepare(48000);
        fm(e);
        e.setParam(P::layer(0, P::FmPegAmt), 12.0f);
        e.setParam(P::layer(0, P::FmPegTime), 0.05f);
        const auto x = play(e, 69, 1.0);
        const double f0 = zeroCrossFreq(x, kSr, 0, 1200), f1 = zeroCrossFreq(x, kSr, 24000, 48000);
        INFO("pitch env: start %.0f Hz, settled %.1f Hz\n", f0, f1);
        CHECK(f0 > 600.0);
        CHECK_NEAR(f1, 440.0, 2.0);
    }
    {  // rate scaling: decay is faster two octaves up
        auto decayLevel = [&](int key) {
            Engine e;
            e.prepare(48000);
            fm(e);
            e.setParam(P::fmOp(0, 0, P::OpSustain), 0.0f);
            e.setParam(P::fmOp(0, 0, P::OpDecay), 1.0f);
            e.setParam(P::fmOp(0, 0, P::OpRateScale), 1.0f);
            const auto x = play(e, key, 0.6);
            return rms(x, 19200, 24000) / rms(x, 480, 2400);
        };
        CHECK(decayLevel(84) < 0.5 * decayLevel(60));
    }
    {  // LFO1 amplitude modulation
        Engine e;
        e.prepare(48000);
        fm(e);
        e.setParam(P::fmOp(0, 0, P::OpAms), 1.0f);
        e.setParam(P::Lfo1Rate, 4.0f);
        const auto x = play(e, 69, 1.0);
        double lo = 1e9, hi = 0;
        for (size_t a = 9600; a + 960 <= 48000; a += 960) {
            const double r = rms(x, a, a + 960);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        INFO("AMS: level swings %.3f .. %.3f\n", lo, hi);
        CHECK(hi > 3.0 * lo);
    }
}

TEST(v07_mseg_macro_motion_sources) {
    {  // MSEG -> pitch: 0 -> 1 (+12 st) in 0.25 s, then back to 0 in 0.25 s
        Engine e;
        e.prepare(48000);
        plain(e);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        e.setParam(P::MsegPoints, 3.0f);
        e.setParam(P::msegPt(0, P::MsegLevel), 0.0f);
        e.setParam(P::msegPt(1, P::MsegTime), 0.25f);
        e.setParam(P::msegPt(1, P::MsegLevel), 1.0f);
        e.setParam(P::msegPt(2, P::MsegTime), 0.25f);
        e.setParam(P::msegPt(2, P::MsegLevel), 0.0f);
        e.setParam(P::mod(3, P::ModSrc), (float)ModSource::Mseg);
        e.setParam(P::mod(3, P::ModDst), (float)ModDest::Pitch);
        e.setParam(P::mod(3, P::ModAmt), 0.5f);
        const auto x = play(e, 57, 0.8);
        CHECK_NEAR(zeroCrossFreq(x, kSr, 11400, 12600), 440.0, 15.0);  // peak at 0.25 s
        CHECK_NEAR(zeroCrossFreq(x, kSr, 28800, 38400), 220.0, 2.0);   // back down
    }
    {  // macro 2 -> pitch
        Engine e;
        e.prepare(48000);
        plain(e);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        e.setParam(P::mod(3, P::ModSrc), (float)ModSource::Macro2);
        e.setParam(P::mod(3, P::ModDst), (float)ModDest::Pitch);
        e.setParam(P::mod(3, P::ModAmt), 0.5f);
        e.setParam(P::Macro2, 1.0f);
        CHECK_NEAR(zeroCrossFreq(play(e, 57, 0.4), kSr, 4800, 19200), 440.0, 2.0);
    }
    {  // motion lane 1 -> pitch: 1/4 steps at 120 bpm alternate 0 / +12 st
        Engine e;
        e.prepare(48000);
        plain(e);
        e.setParam(P::fxChain(P::FxTempo), 120.0f);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        e.setParam(P::motion(0, P::MotionRate), 4.0f);  // option 4 = 1/4
        e.setParam(P::motion(0, P::MotionSteps), 2.0f);
        e.setParam(P::motion(0, P::MotionValue1), 0.0f);
        e.setParam(P::motion(0, P::MotionValue1 + 1), 1.0f);
        e.setParam(P::mod(3, P::ModSrc), (float)ModSource::Motion1);
        e.setParam(P::mod(3, P::ModDst), (float)ModDest::Pitch);
        e.setParam(P::mod(3, P::ModAmt), 0.5f);
        const auto x = play(e, 57, 1.1);
        // the beat counter runs from engine start: step boundaries every 0.5 s
        const double a = zeroCrossFreq(x, kSr, 4800, 19200), b = zeroCrossFreq(x, kSr, 28800, 43200);
        INFO("motion lane: %.1f Hz then %.1f Hz\n", a, b);
        CHECK(std::fabs(std::log2(b / a)) > 0.9);  // an octave apart
    }
}

TEST(v07_step_sequencer_timing_and_threads) {
    auto run = [](int threads, bool keyMode) {
        Engine e;
        e.prepare(48000);
        e.setRenderThreads(threads);
        plain(e);
        e.setParam(P::fxChain(P::FxTempo), 120.0f);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        e.setParam(P::AmpAttack, 0.0005f);
        e.setParam(P::SeqOn, 1.0f);
        e.setParam(P::SeqMode, keyMode ? 1.0f : 0.0f);
        e.setParam(P::SeqRate, 10.0f);  // 1/16 = 0.125 s at 120 bpm
        e.setParam(P::SeqSteps, 4.0f);
        e.setParam(P::SeqGate, 0.5f);
        for (int s = 0; s < 4; ++s) {
            e.setParam(P::seqStep(s, P::SeqVel), 0.9f);
            e.setParam(P::seqStep(s, P::SeqNote), (float)(57 + 12 * (s & 1)));
        }
        if (!keyMode) e.setTransport(true);
        RenderJob job;
        job.seconds = 1.0;
        job.blockSize = 480;  // 10 ms blocks: steps fall inside blocks
        if (keyMode) {
            job.events.push_back({0.0, Event::noteOn(1, 64, 1.0f)});  // +4 st
            job.events.push_back({0.9, Event::noteOff(1, 64)});
        }
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        return L;
    };
    const auto a = run(1, false), b = run(4, false);
    // free mode: started on the next 1/16 of the running beat, then every 6000 samples
    const size_t first = onset(a, 0);
    const size_t second = onset(a, first + 3500);  // gate 0.5 step: silent before the next step
    INFO("sequencer onsets at %zu and %zu (expected 6000 apart)\n", first, second);
    CHECK(std::abs((long)(second - first) - 6000) <= 16);
    double diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = std::max(diff, (double)std::fabs(a[i] - b[i]));
    INFO("serial vs 4 render threads: max diff %.2e\n", diff);
    CHECK(diff < 1e-4);
    // alternate steps an octave apart
    CHECK_NEAR(zeroCrossFreq(a, kSr, first + 200, first + 2800), 220.0, 3.0);
    CHECK_NEAR(zeroCrossFreq(a, kSr, second + 200, second + 2800), 440.0, 5.0);
    // key mode: starts with the key, transposed by (key - C4)
    const auto k = run(1, true);
    const size_t ko = onset(k, 0);
    CHECK(ko < 32);
    CHECK_NEAR(zeroCrossFreq(k, kSr, ko + 200, ko + 2800), 220.0 * std::exp2(4.0 / 12.0), 4.0);
}

TEST(v07_half_pedal_sends_resonance_scanner) {
    {  // half pedal: a note held by a half-raised pedal fades faster than with the pedal down
        auto tail = [](float pedal) {
            Engine e;
            e.prepare(48000);
            plain(e);
            RenderJob job;
            job.seconds = 2.0;
            job.events.push_back({0.0, Event::control(EventType::Sustain, pedal)});
            job.events.push_back({0.05, Event::noteOn(1, 60, 0.8f)});
            job.events.push_back({0.2, Event::noteOff(1, 60)});
            std::vector<float> L, R;
            renderOffline(e, job, L, R);
            return rms(L, 72000, 96000);
        };
        const double full = tail(1.0f), half = tail(0.5f);
        INFO("pedal tail at 1.5 s: full %.4f, half %.5f\n", full, half);
        CHECK(full > 0.05);
        CHECK(half < 0.5 * full);
    }
    {  // send A: a reverb tail with the rack reverb off
        auto tailAfter = [](float send) {
            Engine e;
            e.prepare(48000);
            plain(e);
            e.setParam(P::SendA, send);
            const auto x = play(e, 60, 1.5, 0.2);
            return rms(x, 36000, 60000);
        };
        const double dry = tailAfter(0.0f), wet = tailAfter(0.8f);
        INFO("send A tail: %.6f (no send) vs %.5f\n", dry, wet);
        CHECK(dry < 1e-4);
        CHECK(wet > 1e-3);
    }
    {  // string resonance rings on with the pedal down (the note itself is a short pluck)
        auto ring = [](float pedal) {
            Engine e;
            e.prepare(48000);
            plain(e);
            e.setParam(P::AmpSustain, 0.0f);
            e.setParam(P::AmpDecay, 0.08f);
            e.setParam(P::fxSlot(0), (float)fx::Sympathetic);
            e.setParam(P::fxp(fx::Sympathetic, fx::sympathetic::Amount), 1.0f);
            RenderJob job;
            job.seconds = 1.5;
            job.events.push_back({0.0, Event::control(EventType::Sustain, pedal)});
            job.events.push_back({0.01, Event::noteOn(2, 60, 0.9f)});
            job.events.push_back({0.05, Event::noteOff(2, 60)});
            std::vector<float> L, R;
            renderOffline(e, job, L, R);
            return rms(L, 28800, 48000);
        };
        const double down = ring(1.0f), up = ring(0.0f);
        INFO("string resonance tail: pedal down %.5f, up %.6f\n", down, up);
        CHECK(down > 5.0 * up);
    }
    {  // scanner vibrato: the pitch wobbles
        Engine e;
        e.prepare(48000);
        plain(e);
        e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        e.setParam(P::fxSlot(0), (float)fx::Scanner);
        e.setParam(P::fxp(fx::Scanner, fx::scanner::Mode), 2.0f);  // v3
        const auto x = play(e, 69, 1.0);
        double lo = 1e9, hi = 0;
        for (size_t a = 9600; a + 1200 <= 48000; a += 1200) {
            const double f = zeroCrossFreq(x, kSr, a, a + 1200);
            lo = std::min(lo, f);
            hi = std::max(hi, f);
        }
        INFO("scanner v3: %.1f .. %.1f Hz\n", lo, hi);
        CHECK(hi - lo > 2.0);
    }
}

TEST(v07_rack_idle_keeps_late_echoes_and_wakes) {
    // the rack skips its chain after 4 s of silence; a 1.9 s echo must still
    // arrive, and a note after a long silence must get its reverb again
    Engine e;
    e.prepare(48000);
    plain(e);
    e.setParam(P::fxp(fx::Delay, fx::delay::On), 1.0f);
    e.setParam(P::fxp(fx::Delay, fx::delay::Sync), 0.0f);
    e.setParam(P::fxp(fx::Delay, fx::delay::Time), 1.9f);
    e.setParam(P::fxp(fx::Delay, fx::delay::Feedback), 0.0f);
    e.setParam(P::fxp(fx::Delay, fx::delay::Mix), 1.0f);
    RenderJob job;
    job.seconds = 14.0;
    addNote(job, 0.0, 0.05, 69, 0.9f);   // echo at 1.9 s
    addNote(job, 9.0, 0.05, 69, 0.9f);   // after > 4 s of silence: echo at 10.9 s
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    auto pk = [&](double a, double b) {
        float m = 0;
        for (size_t i = (size_t)(a * 48000); i < (size_t)(b * 48000); ++i) m = std::max(m, std::fabs(L[i]));
        return m;
    };
    INFO("echo 1 %.3f, gap %.6f, echo 2 %.3f\n", pk(1.85, 2.1), pk(3.0, 8.9), pk(10.85, 11.1));
    CHECK(pk(1.85, 2.1) > 0.05);
    CHECK(pk(3.0, 8.9) == 0.0f);   // silence is exact while idle
    CHECK(pk(10.85, 11.1) > 0.05);
}
