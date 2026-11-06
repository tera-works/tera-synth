// 8-operator FM (88 algorithms, operator waveforms, fixed frequency, key
// scaling) and the v0.6 modulation destinations / sources.
#include <cmath>
#include <set>
#include <tuple>

#include "analysis.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

// Layer A = FM with only operator 1 audible (additive algorithm, other ops off).
void singleOp(Engine& e) {
    e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
    e.setParam(P::layer(0, P::FmAlgo), 7.0f);  // "8: 1+2+3+4"
    for (int op = 1; op < P::kFmOps; ++op) e.setParam(P::fmOp(0, op, P::OpLevel), 0.0f);
    e.setParam(P::fmOp(0, 0, P::OpLevel), 1.0f);
    e.setParam(P::fmOp(0, 0, P::OpSustain), 1.0f);
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::AmpSustain, 1.0f);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setParam(P::mod(0, P::ModAmt), 0.0f);
    e.setParam(P::mod(0, P::ModVia), 0.0f);  // the default vibrato slot goes via the mod wheel
}

std::vector<float> note(Engine& e, int key, double seconds = 0.6, float vel = 0.8f) {
    RenderJob job;
    job.seconds = seconds;
    addNote(job, 0.0, seconds, key, vel);
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    return L;
}

double mean(const std::vector<float>& x, size_t a, size_t b) {
    double s = 0;
    for (size_t i = a; i < b; ++i) s += x[i];
    return s / (double)(b - a);
}

}  // namespace

TEST(fm_algorithm_tables_valid) {
    CHECK((int)fmAlgorithmNames().size() == kNumFmAlgorithms);
    CHECK(kNumFmAlgorithms == 88);
    std::set<std::tuple<std::vector<int>, int, int>> seen;
    int eightOp = 0;
    for (int a = 0; a < kNumFmAlgorithms; ++a) {
        const FmAlgorithm& al = fmAlgorithm(a);
        bool ok = al.numOps >= 4 && al.numOps <= P::kFmOps && al.carriers != 0 && al.feedbackOp >= 0 &&
                  al.feedbackOp < al.numOps && (al.carriers >> al.numOps) == 0;
        std::vector<int> mods;
        for (int i = 0; i < P::kFmOps; ++i) {
            const int m = al.modulators[i];
            mods.push_back(m);
            if (m & ((1 << (i + 1)) - 1)) ok = false;    // only higher operators modulate
            if (m >> al.numOps) ok = false;               // only used operators
            if (i >= al.numOps && m) ok = false;
        }
        if (!ok) INFO("algorithm %d (%s) invalid\n", a + 1, fmAlgorithmNames()[(size_t)a].c_str());
        CHECK(ok);
        if (a >= 16) {
            eightOp += al.numOps == 8;
            CHECK(seen.insert({mods, al.carriers, al.feedbackOp}).second);  // no duplicates among the new ones
        }
    }
    CHECK(eightOp == 72);
}

TEST(fm_8op_algorithms_render) {
    double prev = -1;
    int distinct = 0;
    for (int algo : {16, 17, 28, 33, 36, 56, 87}) {
        Engine e;
        e.prepare(48000);
        e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
        e.setParam(P::layer(0, P::FmAlgo), (float)algo);
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        const auto L = note(e, 57, 0.4);
        const double r = rms(L, 2400, 14400);
        CHECK(allFinite(L) && r > 0.005 && peak(L) <= 1.0);
        if (std::fabs(r - prev) > 1e-4) ++distinct;
        prev = r;
    }
    CHECK(distinct >= 6);
}

TEST(fm_operator_waves_fixed_and_keyscale) {
    {  // waveforms: same period, different shapes (half sine: asymmetric peaks)
        Engine e;
        e.prepare(48000);
        singleOp(e);
        const auto sine = note(e, 69);
        CHECK_NEAR(zeroCrossFreq(sine, 48000, 4800, 24000), 440.0, 2.0);
        e.setParam(P::fmOp(0, 0, P::OpWave), (float)FmWave::Half);
        const auto half = note(e, 69);
        float hi = 0, lo = 0;
        for (size_t i = 4800; i < 24000; ++i) { hi = std::max(hi, half[i]); lo = std::min(lo, half[i]); }
        INFO("half sine peaks %+.3f / %+.3f\n", hi, lo);
        CHECK(hi > 1.5f * -lo);
        double diff = 0;
        for (int w = 2; w < (int)FmWave::Count; ++w) {
            e.setParam(P::fmOp(0, 0, P::OpWave), (float)w);
            const auto x = note(e, 69);
            double d = 0;
            for (size_t i = 4800; i < 24000; ++i) d = std::max(d, (double)std::fabs(x[i] - sine[i]));
            CHECK(allFinite(x));
            diff += d > 0.05 ? 1 : 0;
        }
        CHECK(diff == (int)FmWave::Count - 2);
    }
    {  // fixed frequency: the same pitch on every key
        Engine e;
        e.prepare(48000);
        singleOp(e);
        e.setParam(P::fmOp(0, 0, P::OpFixed), 330.0f);
        CHECK_NEAR(zeroCrossFreq(note(e, 48), 48000, 4800, 24000), 330.0, 2.0);
        CHECK_NEAR(zeroCrossFreq(note(e, 84), 48000, 4800, 24000), 330.0, 2.0);
    }
    {  // key scaling -6 dB/oct: two octaves up = -12 dB
        Engine e;
        e.prepare(48000);
        singleOp(e);
        e.setParam(P::fmOp(0, 0, P::OpKeyScale), -6.0f);
        const double lo = rms(note(e, 60), 4800, 24000), hi = rms(note(e, 84), 4800, 24000);
        INFO("key scaling: %.1f dB over two octaves\n", 20.0 * std::log10(hi / lo));
        CHECK_NEAR(20.0 * std::log10(hi / lo), -12.0, 0.5);
    }
}

TEST(mod_slot_amount_and_lfo_depth_destinations) {
    {  // slot 1 scales slot 2's amount: constant -> pitch +12 st is cancelled
        Engine e;
        e.prepare(48000);
        singleOp(e);
        e.setParam(P::mod(1, P::ModSrc), (float)ModSource::Constant);
        e.setParam(P::mod(1, P::ModDst), (float)ModDest::Pitch);
        e.setParam(P::mod(1, P::ModAmt), 0.5f);  // +12 semitones
        CHECK_NEAR(zeroCrossFreq(note(e, 69), 48000, 4800, 24000), 880.0, 3.0);
        e.setParam(P::mod(0, P::ModSrc), (float)ModSource::Constant);
        e.setParam(P::mod(0, P::ModDst), (float)ModDest::Mod2Amt);
        e.setParam(P::mod(0, P::ModAmt), -1.0f);
        CHECK_NEAR(zeroCrossFreq(note(e, 69), 48000, 4800, 24000), 440.0, 2.0);
    }
    {  // per-note pressure sets the vibrato depth (lfo1.depth)
        auto vibrato = [](float pressure) {
            Engine e;
            e.prepare(48000);
            singleOp(e);
            e.setParam(P::Lfo1Rate, 6.0f);
            e.setParam(P::mod(1, P::ModSrc), (float)ModSource::Lfo1);
            e.setParam(P::mod(1, P::ModDst), (float)ModDest::Pitch);
            e.setParam(P::mod(1, P::ModAmt), 0.04f);  // ~1 semitone
            e.setParam(P::mod(0, P::ModSrc), (float)ModSource::NotePressure);
            e.setParam(P::mod(0, P::ModDst), (float)ModDest::Lfo1Depth);
            e.setParam(P::mod(0, P::ModAmt), -1.0f);  // full pressure = no vibrato
            RenderJob job;
            job.seconds = 1.0;
            job.events.push_back({0.0, Event::noteOn(1, 69, 0.8f)});
            job.events.push_back({0.0, Event::perNote(EventType::NotePressure, 1, 69, pressure)});
            std::vector<float> L, R;
            renderOffline(e, job, L, R);
            double lo = 1e9, hi = 0;
            for (size_t a = 9600; a + 2400 <= 48000; a += 2400) {
                const double f = zeroCrossFreq(L, 48000, a, a + 2400);
                lo = std::min(lo, f);
                hi = std::max(hi, f);
            }
            return hi - lo;
        };
        const double free = vibrato(0.0f), pressed = vibrato(1.0f);
        INFO("vibrato swing: no pressure %.1f Hz, full pressure %.1f Hz\n", free, pressed);
        CHECK(free > 10.0);
        CHECK(pressed < 2.0);
    }
}

TEST(mod_release_velocity_and_fm_modpitch) {
    auto relPitch = [](float offVel) {
        Engine e;
        e.prepare(48000);
        singleOp(e);
        e.setParam(P::AmpRelease, 2.0f);
        e.setParam(P::fmOp(0, 0, P::OpRelease), 2.0f);
        e.setParam(P::mod(1, P::ModSrc), (float)ModSource::ReleaseVelocity);
        e.setParam(P::mod(1, P::ModDst), (float)ModDest::Pitch);
        e.setParam(P::mod(1, P::ModAmt), 0.5f);  // relvel 1 = +12 st
        RenderJob job;
        job.seconds = 1.0;
        job.events.push_back({0.0, Event::noteOn(1, 57, 0.8f)});
        job.events.push_back({0.3, Event::noteOff(1, 57, offVel)});
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        return zeroCrossFreq(L, 48000, 19200, 33600);
    };
    const double soft = relPitch(0.0f), hard = relPitch(1.0f);
    INFO("pitch after release: velocity 0 -> %.1f Hz, velocity 1 -> %.1f Hz\n", soft, hard);
    CHECK_NEAR(soft, 220.0, 2.0);
    CHECK_NEAR(hard, 440.0, 3.0);

    // fm.modpitch moves the modulators only: the carrier pitch stays
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
    e.setParam(P::layer(0, P::FmAlgo), 0.0f);  // 4>3>2>1
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    const auto a = note(e, 57);
    e.setParam(P::mod(1, P::ModSrc), (float)ModSource::Constant);
    e.setParam(P::mod(1, P::ModDst), (float)ModDest::FmModPitch);
    e.setParam(P::mod(1, P::ModAmt), 0.25f);  // +6 st on the modulators
    const auto b = note(e, 57);
    double diff = 0;
    for (size_t i = 4800; i < 24000; ++i) diff = std::max(diff, (double)std::fabs(a[i] - b[i]));
    CHECK(diff > 0.05);
}
