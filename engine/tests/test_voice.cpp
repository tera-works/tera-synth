// Voice-level DSP: ladder / comb / formant filters, voice FX, assignable CCs.
#include "analysis.h"
#include "hsynth/dsp/svf.h"
#include "hsynth/dsp/voice_fx.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

constexpr float kSr = 48000.0f;

std::vector<float> noise(size_t n, uint32_t seed = 7) {
    Rng r(seed);
    std::vector<float> v(n);
    for (auto& x : v) x = 0.25f * r.bipolar();
    return v;
}

std::vector<float> sineWave(float f, size_t n, float amp = 0.5f) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = amp * std::sin(kTwoPi * f * (float)i / kSr);
    return v;
}

struct Filt {
    std::vector<float> comb = std::vector<float>(Comb::kSize, 0.0f);
    VoiceFilter f;
    Filt() { f.setCombBuffer(comb.data()); f.reset(); }
    std::vector<float> run(FilterType t, float cutoff, float res, const std::vector<float>& in, float morph = 0.0f,
                           float drive = 0.0f) {
        f.set(t, cutoff, res, drive, kSr, morph);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i) out[i] = f.process(in[i]);
        return out;
    }
};

// level of a sine at f through the filter (steady state), relative to input
double sineGain(FilterType t, float cutoff, float res, float f, float morph = 0.0f) {
    Filt flt;
    const auto in = sineWave(f, 24000);
    const auto out = flt.run(t, cutoff, res, in, morph);
    return rms(out, 12000, 24000) / rms(in, 12000, 24000);
}

void sinePatch(Engine& e) {
    e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
    e.setParam(P::layer(0, P::VaMix), 0.0f);
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setParam(P::mod(0, P::ModAmt), 0.0f);
    e.setParam(P::AmpSustain, 1.0f);
    e.setParam(P::AmpVelSens, 0.0f);
}

std::vector<float> renderNote(Engine& e, int key, double seconds, const std::vector<ScheduledEvent>& extra = {}) {
    RenderJob job;
    job.seconds = seconds;
    job.events = extra;
    job.events.push_back({0.0, Event::noteOn(1, key, 1.0f)});
    std::stable_sort(job.events.begin(), job.events.end(),
                     [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.time < b.time; });
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    return L;
}

double band(const std::vector<float>& x, double f, size_t from = 9600, size_t n = 24000) {
    return goertzel(x, kSr, f, from, n);
}

}  // namespace

TEST(filter_ladder_response_and_self_oscillation) {
    const double pass = sineGain(FilterType::Ladder, 1000, 0.0f, 100);
    const double stop = sineGain(FilterType::Ladder, 1000, 0.0f, 8000);
    INFO("ladder: 100 Hz %.3f, 8 kHz %.6f\n", pass, stop);
    CHECK(pass > 0.6 && pass < 1.6);
    CHECK(stop < 0.003);  // 4 poles: ~ -70 dB three octaves above cutoff
    // resonance peak at cutoff
    CHECK(sineGain(FilterType::Ladder, 1000, 0.8f, 1000) > 2.0 * sineGain(FilterType::Ladder, 1000, 0.0f, 1000));
    // self-oscillation at full resonance, pitch close to the cutoff
    Filt flt;
    std::vector<float> in(48000, 0.0f);
    in[0] = 0.5f;
    const auto out = flt.run(FilterType::Ladder, 880, 1.0f, in);
    CHECK(allFinite(out));
    CHECK(rms(out, 24000, 48000) > 0.05);
    CHECK_NEAR(zeroCrossFreq(out, kSr, 24000, 48000), 880.0, 90.0);
}

TEST(filter_comb_peaks) {
    // comb+: peaks at multiples of 500 Hz, notches in between
    const double pPeak = sineGain(FilterType::CombPlus, 500, 0.9f, 1000);
    const double pNotch = sineGain(FilterType::CombPlus, 500, 0.9f, 1250);
    // comb-: peaks at odd multiples of 250 Hz
    const double mPeak = sineGain(FilterType::CombMinus, 500, 0.9f, 750);
    const double mNotch = sineGain(FilterType::CombMinus, 500, 0.9f, 1000);
    INFO("comb+: %g vs %g, comb-: %g vs %g\n", pPeak, pNotch, mPeak, mNotch);
    CHECK(pPeak > 4.0 * pNotch);
    CHECK(mPeak > 4.0 * mNotch);
    Filt flt;
    const auto out = flt.run(FilterType::CombPlus, 500, 1.0f, noise(48000));
    CHECK(allFinite(out) && peak(out) < 4.0);
}

TEST(filter_formant_vowels) {
    // 'a' (morph 0): F1 800 Hz, F2 1150; 'i' (morph 0.5): F1 270, F2 2140
    const double a800 = sineGain(FilterType::Formant, 1000, 0.6f, 800, 0.0f);
    const double a2140 = sineGain(FilterType::Formant, 1000, 0.6f, 2140, 0.0f);
    const double i800 = sineGain(FilterType::Formant, 1000, 0.6f, 800, 0.5f);
    const double i2140 = sineGain(FilterType::Formant, 1000, 0.6f, 2140, 0.5f);
    INFO("formant a: 800 %.3f 2140 %.3f | i: 800 %.3f 2140 %.3f\n", a800, a2140, i800, i2140);
    CHECK(a800 > 3.0 * i800);
    CHECK(i2140 > 2.0 * a2140);
    // cutoff shifts the formants: at 2 kHz 'a' has F1 at 1600 Hz
    CHECK(sineGain(FilterType::Formant, 2000, 0.6f, 1600, 0.0f) > 2.0 * sineGain(FilterType::Formant, 2000, 0.6f, 800, 0.0f));
}

TEST(filter_new_types_stable_under_modulation) {
    const FilterType types[] = {FilterType::Ladder, FilterType::CombPlus, FilterType::CombMinus, FilterType::Formant};
    for (FilterType t : types) {
        Filt flt;
        Rng r(3);
        const auto in = noise(48000 * 2);
        double pk = 0;
        bool finite = true;
        for (size_t off = 0; off < in.size(); off += 16) {
            flt.f.set(t, 20.0f + 19000.0f * r.unipolar() * r.unipolar(), r.unipolar(), r.unipolar(), kSr, r.unipolar());
            for (size_t i = off; i < off + 16; ++i) {
                const float y = flt.f.process(in[i] * 4.0f);
                finite = finite && std::isfinite(y);
                pk = std::max(pk, (double)std::fabs(y));
            }
        }
        INFO("filter type %d peak %.2f\n", (int)t, pk);
        CHECK(finite);
        CHECK(pk < 30.0);
    }
}

TEST(filter_comb_in_engine_stereo_switch) {
    // a layer with pan switches the voice to stereo mid-note: the comb's delay
    // memory is copied to the right channel without clicks or aliasing buffers
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    e.setParam(P::FilterType, (float)FilterType::CombPlus);
    e.setParam(P::FilterCutoff, 220.0f);
    e.setParam(P::FilterKeytrack, 0.0f);
    e.setParam(P::FilterEnvAmt, 0.0f);
    e.setParam(P::FilterVelAmt, 0.0f);
    e.setParam(P::FilterReso, 0.8f);
    e.setParam(P::mod(3, P::ModSrc), (float)ModSource::ModWheel);
    e.setParam(P::mod(3, P::ModDst), (float)ModDest::Pan);
    e.setParam(P::mod(3, P::ModAmt), 0.5f);
    const auto L = renderNote(e, 57, 1.0, {{0.5, Event::control(EventType::ModWheel, 1.0f)}});
    CHECK(allFinite(L));
    CHECK(rms(L, 4800, 24000) > 0.01);
    double maxStep = 0;
    for (size_t i = 24000; i < 26000; ++i) maxStep = std::max(maxStep, (double)std::fabs(L[i] - L[i - 1]));
    CHECK(maxStep < 0.2);
}

TEST(voice_fx_types) {
    auto render = [](VfxType t, float amount, float ratio = 2.0f) {
        Engine e;
        e.prepare(48000);
        sinePatch(e);
        e.setParam(P::VfxType, (float)t);
        e.setParam(P::VfxAmount, amount);
        e.setParam(P::VfxRatio, ratio);
        return renderNote(e, 69, 0.8);
    };
    const auto clean = render(VfxType::Off, 0.0f);
    const auto sat = render(VfxType::Saturate, 0.8f);
    const auto fold = render(VfxType::Fold, 0.8f);
    const auto crush = render(VfxType::Crush, 0.9f);
    const auto ring = render(VfxType::Ring, 1.0f, 1.5f);
    for (auto* v : {&sat, &fold, &crush, &ring}) CHECK(allFinite(*v) && peak(*v) < 1.5);
    const double h3clean = band(clean, 1320) / band(clean, 440);
    const double h3sat = band(sat, 1320) / band(sat, 440);
    const double h3fold = band(fold, 1320) / band(fold, 440);
    INFO("3rd harmonic ratio: clean %g, saturate %g, fold %g\n", h3clean, h3sat, h3fold);
    CHECK(h3clean < 1e-3);
    CHECK(h3sat > 0.05);
    CHECK(h3fold > 0.05);
    // crush: error energy (signal minus the clean sine)
    double err = 0;
    for (size_t i = 9600; i < 33600; ++i) err += (crush[i] - clean[i]) * (crush[i] - clean[i]);
    CHECK(std::sqrt(err / 24000) > 0.01);
    // ring at 1.5x: carrier 660 Hz -> sidebands 220 and 1100 Hz, no 440
    INFO("ring: 220 %g, 1100 %g, 440 %g\n", band(ring, 220), band(ring, 1100), band(ring, 440));
    CHECK(band(ring, 220) > 10.0 * band(ring, 440));
    CHECK(band(ring, 1100) > 10.0 * band(ring, 440));
}

TEST(voice_fx_per_note_amount) {
    // note pressure drives vfx.amount: only the pressed key distorts
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    e.setParam(P::VfxType, (float)VfxType::Saturate);
    e.setParam(P::VfxAmount, 0.0f);
    e.setParam(P::mod(3, P::ModSrc), (float)ModSource::NotePressure);
    e.setParam(P::mod(3, P::ModDst), (float)ModDest::VfxAmount);
    e.setParam(P::mod(3, P::ModAmt), 1.0f);
    RenderJob job;
    job.seconds = 1.0;
    job.events.push_back({0.0, Event::noteOn(1, 57, 1.0f)});   // 220 Hz
    job.events.push_back({0.0, Event::noteOn(2, 69, 1.0f)});   // 440 Hz
    job.events.push_back({0.05, Event::perNote(EventType::NotePressure, 2, 69, 1.0f)});
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    const double h220 = band(L, 660) / band(L, 220);   // 3rd harmonic of the unpressed note
    const double h440 = band(L, 1320) / band(L, 440);  // 3rd harmonic of the pressed note
    INFO("3rd harmonic: unpressed %g, pressed %g\n", h220, h440);
    CHECK(h440 > 0.05);
    CHECK(h220 < 0.01);
}

TEST(assignable_cc_mod_source) {
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    e.setParam(P::CtlCc2, 21.0f);
    e.setParam(P::mod(3, P::ModSrc), (float)ModSource::Cc2);
    e.setParam(P::mod(3, P::ModDst), (float)ModDest::Pitch);
    e.setParam(P::mod(3, P::ModAmt), 0.5f);  // +12 semitones at full controller
    const auto L = renderNote(e, 69, 1.0, {{0.5, Event::control(EventType::Controller, 1.0f, 21)},
                                           {0.3, Event::control(EventType::Controller, 1.0f, 22)}});
    CHECK_NEAR(zeroCrossFreq(L, kSr, 4800, 23000), 440.0, 1.0);
    CHECK_NEAR(zeroCrossFreq(L, kSr, 28000, 47000), 880.0, 2.0);
    // CC1 is delivered as ModWheel: assigning CC 1 still works
    e.setParam(P::CtlCc2, 1.0f);
    e.sendEvent(Event::control(EventType::AllSoundOff, 0));
    const auto M = renderNote(e, 69, 1.0, {{0.5, Event::control(EventType::ModWheel, 1.0f)}});
    CHECK_NEAR(zeroCrossFreq(M, kSr, 28000, 47000), 880.0, 2.0);
}
