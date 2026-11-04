// Voice-level DSP: ladder / comb / formant filters, voice FX, assignable CCs.
#include "analysis.h"
#include "hsynth/dsp/svf.h"
#include "hsynth/dsp/voice_fx.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/tuning.h"
#include <filesystem>
#include <fstream>
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

// ------------------------------------------------------------ voice modes

namespace {
std::vector<float> renderJob(Engine& e, RenderJob job, std::vector<float>* right = nullptr) {
    std::stable_sort(job.events.begin(), job.events.end(),
                     [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.time < b.time; });
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    if (right) *right = R;
    return L;
}
double freqAt(const std::vector<float>& x, double t0, double t1) {
    return zeroCrossFreq(x, kSr, (size_t)(t0 * kSr), (size_t)(t1 * kSr));
}
}  // namespace

TEST(voice_mono_last_note_priority) {
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    e.setParam(P::AmpRelease, 0.01f);
    e.setParam(P::VoiceMode, (float)VoiceMode::Mono);
    RenderJob job;
    job.seconds = 1.6;
    job.events.push_back({0.0, Event::noteOn(1, 57, 1.0f)});   // 220
    job.events.push_back({0.4, Event::noteOn(2, 69, 1.0f)});   // 440 on top
    job.events.push_back({0.8, Event::noteOff(2, 69)});        // back to 220
    job.events.push_back({1.2, Event::noteOff(1, 57)});
    const auto L = renderJob(e, job);
    CHECK_NEAR(freqAt(L, 0.1, 0.35), 220.0, 1.0);
    CHECK_NEAR(freqAt(L, 0.5, 0.75), 440.0, 1.5);
    CHECK_NEAR(freqAt(L, 0.9, 1.15), 220.0, 1.0);
    CHECK(rms(L, (size_t)(1.3 * kSr), (size_t)(1.6 * kSr)) < 1e-4);
    CHECK(e.stats().activeVoices == 0);
}

TEST(voice_legato_vs_mono_retrigger) {
    auto levelAfterChange = [](VoiceMode mode) {
        Engine e;
        e.prepare(48000);
        sinePatch(e);
        e.setParam(P::AmpAttack, 0.3f);
        e.setParam(P::VoiceMode, (float)mode);
        RenderJob job;
        job.seconds = 1.2;
        job.events.push_back({0.0, Event::noteOn(1, 57, 1.0f)});
        job.events.push_back({0.8, Event::noteOn(2, 60, 1.0f)});
        const auto L = renderJob(e, job);
        INFO("mode %d: active voices %d\n", (int)mode, e.stats().activeVoices);
        return rms(L, (size_t)(0.82 * kSr), (size_t)(0.86 * kSr)) / rms(L, (size_t)(0.7 * kSr), (size_t)(0.78 * kSr));
    };
    const double legato = levelAfterChange(VoiceMode::Legato), mono = levelAfterChange(VoiceMode::Mono);
    INFO("level right after the note change: legato %.2f, mono %.2f (relative)\n", legato, mono);
    CHECK(legato > 0.9);  // no new attack
    CHECK(mono < 0.5);    // envelope restarts
}

TEST(voice_glide) {
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    e.setParam(P::VoiceMode, (float)VoiceMode::Legato);
    e.setParam(P::Glide, 0.3f);
    RenderJob job;
    job.seconds = 1.4;
    job.events.push_back({0.0, Event::noteOn(1, 57, 1.0f)});
    job.events.push_back({0.5, Event::noteOn(2, 69, 1.0f)});
    const auto L = renderJob(e, job);
    const double mid = freqAt(L, 0.53, 0.58), end = freqAt(L, 1.0, 1.3);
    INFO("glide 220 -> 440: %.1f Hz shortly after, %.1f Hz at the end\n", mid, end);
    CHECK(freqAt(L, 0.1, 0.45) < 221.0);
    CHECK(mid > 240.0 && mid < 420.0);
    CHECK_NEAR(end, 440.0, 1.5);

    // poly portamento: a new chord note glides from the previous note
    Engine p;
    p.prepare(48000);
    sinePatch(p);
    p.setParam(P::Glide, 0.3f);
    RenderJob pj;
    pj.seconds = 1.0;
    addNote(pj, 0.0, 0.2, 57, 1.0f);
    addNote(pj, 0.3, 0.6, 69, 1.0f);
    const auto PL = renderJob(p, pj);
    const double pmid = freqAt(PL, 0.32, 0.37);
    CHECK(pmid > 240.0 && pmid < 420.0);
    // glide mode "legato": detached notes do not glide
    p.setParam(P::GlideMode, 1.0f);
    const auto QL = renderJob(p, pj);
    CHECK_NEAR(freqAt(QL, 0.32, 0.37), 440.0, 3.0);
}

TEST(voice_unison_stack) {
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    e.setParam(P::AmpRelease, 0.02f);
    e.setParam(P::Unison, 4.0f);
    e.setParam(P::UnisonDetune, 30.0f);
    e.setParam(P::UnisonSpread, 1.0f);
    RenderJob job;
    job.seconds = 1.5;
    addNote(job, 0.0, 1.0, 69, 1.0f);
    std::vector<float> R;
    RenderJob first = job;
    first.seconds = 0.5;
    const auto L0 = renderJob(e, first);
    CHECK(e.stats().activeVoices == 4);
    e.sendEvent(Event::control(EventType::AllSoundOff, 0));
    const auto L = renderJob(e, job, &R);
    double diff = 0;
    for (size_t i = 4800; i < 40000; ++i) diff += std::fabs(L[i] - R[i]);
    CHECK(diff > 10.0);                         // spread: stereo
    CHECK_NEAR(freqAt(L, 0.1, 0.9), 440.0, 6.0); // centred on the note
    CHECK(rms(L, (size_t)(1.2 * kSr), (size_t)(1.5 * kSr)) < 1e-4);
    CHECK(e.stats().activeVoices == 0);          // note-off released the whole stack
    // mono + unison: the stack moves together
    e.setParam(P::VoiceMode, (float)VoiceMode::Legato);
    RenderJob mj;
    mj.seconds = 0.6;
    mj.events.push_back({0.0, Event::noteOn(1, 57, 1.0f)});
    mj.events.push_back({0.3, Event::noteOn(2, 69, 1.0f)});
    const auto M = renderJob(e, mj);
    CHECK(e.stats().activeVoices == 4);
    CHECK_NEAR(freqAt(M, 0.35, 0.58), 440.0, 6.0);
}

TEST(voice_modes_no_stuck_notes_stress) {
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    for (int k = 0; k < 4; ++k) {
        e.setPartParam(k, P::VoiceMode, (float)(k % 3));
        e.setPartParam(k, P::Unison, (float)(1 + k));
        e.setPartParam(k, P::Glide, k % 2 ? 0.05f : 0.0f);
        e.setPartParam(k, P::AmpRelease, 0.01f);
    }
    e.setPartParam(3, P::ArpOn, 1.0f);  // part 4: arpeggiated (mono + unison 4 + arp)
    e.setPartParam(3, P::ArpOctaves, 2.0f);
    e.setMaxVoices(24);
    Rng r(9);
    std::vector<float> L(128), R(128);
    struct Held { uint32_t id; int key, ch; };
    std::vector<Held> held;
    uint32_t id = 1;
    for (int b = 0; b < 3000; ++b) {
        const float x = r.unipolar();
        if (x < 0.3f && held.size() < 12) {
            Held h{id++, 40 + (int)(r.unipolar() * 40), (int)(r.unipolar() * 4)};
            Event ev = Event::noteOn(h.id, h.key, 0.8f);
            ev.channel = (uint8_t)h.ch;
            e.sendEvent(ev);
            held.push_back(h);
        } else if (x < 0.55f && !held.empty()) {
            const size_t i = (size_t)(r.unipolar() * (float)held.size());
            Event ev = Event::noteOff(r.unipolar() < 0.3f ? 0 : held[i].id, held[i].key);  // some key-matched
            ev.channel = (uint8_t)held[i].ch;
            e.sendEvent(ev);
            held.erase(held.begin() + (long)i);
        } else if (x < 0.57f) {
            Event ev = Event::control(EventType::Sustain, r.unipolar() < 0.5f ? 1.0f : 0.0f);
            ev.channel = (uint8_t)(r.unipolar() * 4);
            e.sendEvent(ev);
        }
        e.process(L.data(), R.data(), 128);
        for (float v : L) CHECK(std::isfinite(v));
    }
    for (auto& h : held) {
        Event ev = Event::noteOff(h.id, h.key);
        ev.channel = (uint8_t)h.ch;
        e.sendEvent(ev);
    }
    for (int ch = 0; ch < 4; ++ch) {
        Event ev = Event::control(EventType::Sustain, 0.0f);
        ev.channel = (uint8_t)ch;
        e.sendEvent(ev);
    }
    for (int b = 0; b < 200; ++b) e.process(L.data(), R.data(), 128);
    INFO("voices left after all notes off: %d\n", e.stats().activeVoices);
    CHECK(e.stats().activeVoices == 0);
}

// ------------------------------------------------------- modulation matrix

namespace {
void route(Engine& e, int slot, ModSource src, ModDest dst, float amt, ModSource via = ModSource::None) {
    e.setParam(P::mod(slot, P::ModSrc), (float)src);
    e.setParam(P::mod(slot, P::ModDst), (float)dst);
    e.setParam(P::mod(slot, P::ModAmt), amt);
    e.setParam(P::mod(slot, P::ModVia), (float)via);
}
}  // namespace

TEST(mod_constant_source_and_slot16) {
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    route(e, 15, ModSource::Constant, ModDest::Pitch, 0.5f);  // last slot: +12 st
    CHECK_NEAR(zeroCrossFreq(renderNote(e, 57, 0.5), kSr, 4800, 20000), 440.0, 1.5);
    route(e, 15, ModSource::Constant, ModDest::Pitch, 0.5f, ModSource::ModWheel);  // scaled by the wheel (0)
    e.sendEvent(Event::control(EventType::AllSoundOff, 0));
    CHECK_NEAR(zeroCrossFreq(renderNote(e, 57, 0.5), kSr, 4800, 20000), 220.0, 1.0);
}

TEST(mod_lfo3_and_env3) {
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    // env3 -> pitch: +12 st at the start, decaying to 0 (sustain 0)
    e.setParam(P::Env3Attack, 0.001f);
    e.setParam(P::Env3Decay, 0.2f);
    e.setParam(P::Env3Sustain, 0.0f);
    route(e, 9, ModSource::Env3, ModDest::Pitch, 0.5f);
    auto L = renderNote(e, 57, 1.5);
    const double early = zeroCrossFreq(L, kSr, 200, 1400), late = zeroCrossFreq(L, kSr, 48000, 70000);
    INFO("env3 pitch: early %.1f Hz, late %.1f Hz\n", early, late);
    CHECK(early > 300.0);  // averaged over the first 25 ms of the decay
    CHECK_NEAR(late, 220.0, 1.0);
    // lfo3 tempo-synced to 1/4 at fx.tempo 90 bpm -> 1.5 Hz square: +-1 semitone steps
    route(e, 9, ModSource::None, ModDest::None, 0.0f);
    e.setParam(P::fxChain(P::FxTempo), 90.0f);
    e.setParam(P::Lfo3Wave, (float)LfoWave::Square);
    e.setParam(P::Lfo3Sync, (float)fx::SyncDiv::D1_4);
    route(e, 10, ModSource::Lfo3, ModDest::Pitch, 1.0f / 24.0f);
    e.sendEvent(Event::control(EventType::AllSoundOff, 0));
    L = renderNote(e, 69, 1.5);
    // square starts high: +1 st for 1/3 s, then -1 st
    const double hi = zeroCrossFreq(L, kSr, 1000, 15000), lo = zeroCrossFreq(L, kSr, 17000, 31000);
    INFO("lfo3: %.1f / %.1f Hz\n", hi, lo);
    CHECK_NEAR(hi, 440.0 * std::pow(2.0, 1.0 / 12.0), 2.0);
    CHECK_NEAR(lo, 440.0 * std::pow(2.0, -1.0 / 12.0), 2.0);
}

TEST(mod_new_destinations) {
    // layer pan: mod wheel -> a.pan +1 moves layer A hard right
    {
        Engine e;
        e.prepare(48000);
        sinePatch(e);
        route(e, 9, ModSource::ModWheel, ModDest::LayerAPan, 1.0f);
        RenderJob job;
        job.seconds = 0.5;
        job.events.push_back({0.0, Event::control(EventType::ModWheel, 1.0f)});
        job.events.push_back({0.0, Event::noteOn(1, 69, 1.0f)});
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        CHECK(rms(L, 4800, 20000) < 1e-4 && rms(R, 4800, 20000) > 0.05);
    }
    // FM operator level: algorithm 1 (4>3>2>1); op2 level -> 0 leaves op1 as a pure sine
    {
        auto h2 = [](float amt) {
            Engine e;
            e.prepare(48000);
            sinePatch(e);
            e.setParam(P::layer(0, P::LType), (float)LayerType::FM);
            e.setParam(P::fmOp(0, 1, P::OpLevel), 1.0f);
            e.setParam(P::fmOp(0, 1, P::OpSustain), 1.0f);
            e.setParam(P::fmOp(0, 0, P::OpSustain), 1.0f);
            route(e, 9, ModSource::Constant, ModDest::FmOp2Level, amt);
            const auto L = renderNote(e, 57, 0.5);
            return goertzel(L, kSr, 440, 4800, 16000) / goertzel(L, kSr, 220, 4800, 16000);
        };
        const double full = h2(0.0f), off = h2(-1.0f);
        INFO("FM 2nd harmonic ratio: op2 full %g, op2 modulated to 0 %g\n", full, off);
        CHECK(full > 0.05);
        CHECK(off < 1e-3);
    }
    // velocity shortens the amp attack (amp.attack, negative amount)
    {
        auto levelAt20ms = [](float vel) {
            Engine e;
            e.prepare(48000);
            sinePatch(e);
            e.setParam(P::AmpAttack, 0.5f);
            route(e, 9, ModSource::Velocity, ModDest::AmpAttack, -1.0f);  // vel 1: attack / 8
            RenderJob job;
            job.seconds = 0.1;
            job.events.push_back({0.0, Event::noteOn(1, 69, vel)});
            std::vector<float> L, R;
            renderOffline(e, job, L, R);
            return rms(L, 800, 1100);
        };
        CHECK(levelAt20ms(1.0f) > 3.0 * levelAt20ms(0.1f));
    }
}

// ---------------------------------------------------------- 6-operator FM

namespace {
Engine* fmEngine(int algo) {
    auto* e = new Engine;
    e->prepare(48000);
    sinePatch(*e);
    e->setParam(P::layer(0, P::LType), (float)LayerType::FM);
    e->setParam(P::layer(0, P::FmAlgo), (float)algo);
    for (int op = 0; op < P::kFmOps; ++op) {
        e->setParam(P::fmOp(0, op, P::OpSustain), 1.0f);
        e->setParam(P::fmOp(0, op, P::OpVelSens), 0.0f);
    }
    return e;
}
}  // namespace

TEST(fm_six_operator_additive) {
    // algorithm 16 = DX 32: six carriers at ratios 1..6
    std::unique_ptr<Engine> e(fmEngine(15));
    for (int op = 0; op < 6; ++op) {
        e->setParam(P::fmOp(0, op, P::OpRatio), (float)(op + 1));
        e->setParam(P::fmOp(0, op, P::OpLevel), 1.0f);
    }
    const auto L = renderNote(*e, 45, 0.6);  // 110 Hz
    for (int h = 1; h <= 6; ++h) {
        const double a = goertzel(L, kSr, 110.0 * h, 4800, 19200);
        INFO("harmonic %d: %g\n", h, a);
        CHECK(a > 0.02);
    }
    CHECK(goertzel(L, kSr, 770, 4800, 19200) < 1e-3);
}

TEST(fm_six_operator_stack_and_four_op_unchanged) {
    // DX 1 (algorithm 9): 6>5>4>3 + 2>1; only op 6 -> ... chain modulates carrier 3
    std::unique_ptr<Engine> e(fmEngine(8));
    for (int op = 0; op < 6; ++op) e->setParam(P::fmOp(0, op, P::OpLevel), op == 0 || op == 2 ? 1.0f : 0.0f);
    e->setParam(P::fmOp(0, 2, P::OpRatio), 2.0f);
    auto L = renderNote(*e, 45, 0.6);
    CHECK(goertzel(L, kSr, 110, 4800, 19200) > 0.05 && goertzel(L, kSr, 220, 4800, 19200) > 0.05);
    CHECK(goertzel(L, kSr, 330, 4800, 19200) < 1e-3);  // no modulation yet: two pure carriers
    e->setParam(P::fmOp(0, 5, P::OpLevel), 1.0f);      // op6 (top) -> op5 level 0 -> nothing reaches op3
    e->sendEvent(Event::control(EventType::AllSoundOff, 0));
    L = renderNote(*e, 45, 0.6);
    CHECK(goertzel(L, kSr, 330, 4800, 19200) < 1e-3);
    e->setParam(P::fmOp(0, 4, P::OpLevel), 1.0f);
    e->setParam(P::fmOp(0, 3, P::OpLevel), 1.0f);      // full stack: sidebands around op3
    e->sendEvent(Event::control(EventType::AllSoundOff, 0));
    L = renderNote(*e, 45, 0.6);
    CHECK(goertzel(L, kSr, 330, 4800, 19200) > 0.01);

    // a 4-operator algorithm ignores operators 5 and 6 entirely
    auto render4 = [](float op5level) {
        std::unique_ptr<Engine> f(fmEngine(0));
        f->setParam(P::fmOp(0, 4, P::OpLevel), op5level);
        f->setParam(P::fmOp(0, 5, P::OpRatio), 7.0f);
        return renderNote(*f, 57, 0.4);
    };
    const auto a = render4(0.0f), b = render4(1.0f);
    double diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = std::max(diff, (double)std::fabs(a[i] - b[i]));
    CHECK(diff == 0.0);
}

TEST(fm_all_algorithms_finite) {
    for (int algo = 0; algo < 16; ++algo) {
        std::unique_ptr<Engine> e(fmEngine(algo));
        e->setParam(P::layer(0, P::FmFeedback), 1.0f);
        e->setParam(P::layer(0, P::FmIndex), 1.0f);
        const auto L = renderNote(*e, 60, 0.3);
        CHECK(allFinite(L) && peak(L) < 2.0 && rms(L, 2400, 14000) > 0.01);
    }
}

// ------------------------------------------------- tuning / velocity curve


TEST(tuning_master_and_scale) {
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    e.setParam(P::MasterTune, 100.0f);
    CHECK_NEAR(zeroCrossFreq(renderNote(e, 69, 0.5), kSr, 4800, 20000), 440.0 * std::pow(2.0, 1.0 / 12.0), 1.5);
    CHECK(!isPartParam(P::MasterTune) && isPartParam(P::TuneC + 3));
    e.setParam(P::MasterTune, 0.0f);
    e.setParam(P::TuneC + 9, -50.0f);  // A a quarter tone flat
    e.sendEvent(Event::control(EventType::AllSoundOff, 0));
    CHECK_NEAR(zeroCrossFreq(renderNote(e, 69, 0.5), kSr, 4800, 20000), 440.0 * std::pow(2.0, -0.5 / 12.0), 1.5);
    e.sendEvent(Event::control(EventType::AllSoundOff, 0));
    CHECK_NEAR(zeroCrossFreq(renderNote(e, 72, 0.5), kSr, 4800, 20000), 523.25, 1.5);  // C unchanged
}

TEST(tuning_scala_import) {
    const std::string ji =
        "! just.scl\n!\n5-limit just intonation\n 12\n!\n16/15\n9/8\n6/5\n5/4\n4/3\n45/32\n3/2\n8/5\n5/3\n9/5\n15/8\n2/1\n";
    ScalaScale s;
    std::string err;
    CHECK(parseScala(ji, s, err));
    CHECK(s.description == "5-limit just intonation" && s.cents.size() == 12);
    float off[12];
    CHECK(scalaToOffsets(s, 0, off, err));
    CHECK_NEAR(off[4], 1200.0 * std::log2(1.25) - 400.0, 0.01);  // E: -13.69 ct
    CHECK_NEAR(off[7], 1200.0 * std::log2(1.5) - 700.0, 0.01);   // G: +1.96 ct
    CHECK(off[0] == 0.0f);
    // root on D: degree 4 (major third) lands on F#
    CHECK(scalaToOffsets(s, 2, off, err));
    CHECK_NEAR(off[6], 1200.0 * std::log2(1.25) - 400.0, 0.01);
    // cents notation, comments after the value, and a non-12 scale
    ScalaScale c;
    CHECK(parseScala("cents\n3\n400.0 major third\n700.0\n1200.0\n", c, err) && c.cents[1] == 700.0);
    CHECK(!scalaToOffsets(c, 0, off, err));
    CHECK(!parseScala("x\n4\n100.0\n", c, err));
    // through the engine: E4 in just intonation from C = 261.63 * 5/4
    const std::string path = (std::filesystem::temp_directory_path() / "hsynth_tests" / "just.scl").generic_string();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path) << ji;
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    CHECK(applyScalaTuning(e, 0, path, 0, err));
    CHECK_NEAR(zeroCrossFreq(renderNote(e, 64, 0.5), kSr, 4800, 20000), 261.6256 * 1.25, 1.0);
}

TEST(velocity_curve) {
    auto level = [](float curve, float vel) {
        Engine e;
        e.prepare(48000);
        sinePatch(e);
        e.setParam(P::AmpVelSens, 1.0f);
        e.setParam(P::VelCurve, curve);
        RenderJob job;
        job.seconds = 0.3;
        job.events.push_back({0.0, Event::noteOn(1, 69, vel)});
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        return rms(L, 4800, 12000);
    };
    const double lin = level(0.0f, 0.5f), hard = level(1.0f, 0.5f), soft = level(-1.0f, 0.5f), full = level(1.0f, 1.0f);
    INFO("velocity 0.5: linear %g, hard %g, soft %g; velocity 1 hard %g\n", lin, hard, soft, full);
    CHECK(hard < 0.5 * lin);
    CHECK(soft > 1.5 * lin);
    CHECK_NEAR(full, level(0.0f, 1.0f), 1e-6);  // full velocity unaffected
}

// ---------------------------------------------------------- arpeggiator

namespace {
Engine* arpEngine(ArpMode mode, int octaves = 1) {
    auto* e = new Engine;
    e->prepare(48000);
    sinePatch(*e);
    e->setParam(P::AmpAttack, 0.001f);
    e->setParam(P::AmpRelease, 0.005f);
    e->setParam(P::ArpOn, 1.0f);
    e->setParam(P::ArpMode, (float)mode);
    e->setParam(P::ArpOctaves, (float)octaves);
    e->setParam(P::fxChain(P::FxTempo), 120.0f);  // 1/16 = 0.125 s
    return e;
}
// frequency of step k (0.125 s steps, gate 0.5)
double stepFreq(const std::vector<float>& L, int k, double step = 0.125) {
    return freqAt(L, k * step + 0.008, k * step + 0.055);
}
}  // namespace

TEST(arp_up_down_updown_octaves) {
    const double C = 261.63, E = 329.63, G = 392.0;
    auto run = [](ArpMode mode, int oct, std::initializer_list<int> keys, double seconds) {
        std::unique_ptr<Engine> e(arpEngine(mode, oct));
        RenderJob job;
        job.seconds = seconds;
        uint32_t id = 1;
        for (int k : keys) job.events.push_back({0.0, Event::noteOn(id++, k, 1.0f)});
        return renderJob(*e, job);
    };
    auto L = run(ArpMode::Up, 1, {64, 60, 67}, 1.0);
    const double up[] = {C, E, G, C, E, G};
    for (int k = 0; k < 6; ++k) CHECK_NEAR(stepFreq(L, k), up[k], 2.0);
    // gate 0.5: silence in the second half of each step
    CHECK(rms(L, (size_t)(0.09 * kSr), (size_t)(0.12 * kSr)) < 1e-3);
    L = run(ArpMode::Down, 1, {60, 64, 67}, 1.0);
    const double down[] = {G, E, C, G};
    for (int k = 0; k < 4; ++k) CHECK_NEAR(stepFreq(L, k), down[k], 2.0);
    L = run(ArpMode::UpDown, 1, {60, 64, 67}, 1.0);
    const double ud[] = {C, E, G, E, C, E};
    for (int k = 0; k < 6; ++k) CHECK_NEAR(stepFreq(L, k), ud[k], 2.0);
    L = run(ArpMode::Up, 2, {60}, 1.0);
    for (int k = 0; k < 4; ++k) CHECK_NEAR(stepFreq(L, k), k % 2 ? 2 * C : C, 4.0);
    L = run(ArpMode::Order, 1, {67, 60}, 0.6);
    CHECK_NEAR(stepFreq(L, 0), G, 2.0);
    CHECK_NEAR(stepFreq(L, 1), C, 2.0);
}

TEST(arp_release_latch_and_chord) {
    std::unique_ptr<Engine> e(arpEngine(ArpMode::Up));
    RenderJob job;
    job.seconds = 1.0;
    job.events.push_back({0.0, Event::noteOn(1, 60, 1.0f)});
    job.events.push_back({0.3, Event::noteOff(1, 60)});
    auto L = renderJob(*e, job);
    CHECK(rms(L, (size_t)(0.5 * kSr), (size_t)(1.0 * kSr)) < 1e-4);  // stops with the key
    CHECK(e->stats().activeVoices == 0);
    // latch: keeps going after release; a new chord replaces the latched one
    e->setParam(P::ArpLatch, 1.0f);
    job.events.push_back({0.55, Event::noteOn(2, 67, 1.0f)});
    job.events.push_back({0.6, Event::noteOff(2, 67)});
    L = renderJob(*e, job);
    CHECK_NEAR(stepFreq(L, 3), 261.63, 2.0);  // 0.375 s: still C after the key-up at 0.3
    CHECK_NEAR(stepFreq(L, 6), 392.0, 2.0);   // 0.75 s: the new latched note G
    // switching the arp off stops it without hanging notes
    e->setParam(P::ArpOn, 0.0f);
    std::vector<float> l(128), r(128);
    for (int b = 0; b < 40; ++b) e->process(l.data(), r.data(), 128);
    CHECK(e->stats().activeVoices == 0);
    // chord mode: every step plays all held notes
    std::unique_ptr<Engine> c(arpEngine(ArpMode::Chord));
    RenderJob cj;
    cj.seconds = 0.15;  // inside the second step's gate
    for (int k : {60, 64, 67}) cj.events.push_back({0.0, Event::noteOn((uint32_t)k, k, 1.0f)});
    renderJob(*c, cj);
    CHECK(c->stats().activeVoices == 3);
}

TEST(arp_swing_and_clock_sync) {
    std::unique_ptr<Engine> e(arpEngine(ArpMode::Up));
    e->setParam(P::ArpSwing, 0.4f);  // odd steps 0.05 s late
    RenderJob job;
    job.seconds = 0.6;
    job.events.push_back({0.0, Event::noteOn(1, 60, 1.0f)});
    job.events.push_back({0.0, Event::noteOn(2, 72, 1.0f)});
    auto L = renderJob(*e, job);
    CHECK(rms(L, (size_t)(0.13 * kSr), (size_t)(0.17 * kSr)) < 1e-3);  // step 1 has not started yet
    CHECK_NEAR(freqAt(L, 0.18, 0.22), 523.25, 3.0);

    // MIDI clock at 100 bpm: 1/16 = 0.15 s
    std::unique_ptr<Engine> c(arpEngine(ArpMode::Up));
    c->setParam(P::fxChain(P::FxClockSync), 1.0f);
    std::vector<float> out, l(128), r(128);
    double nextTick = 0.0;
    const double tick = 60.0 / (100.0 * 24.0);
    for (int b = 0; b < (int)(3.0 * 48000 / 128); ++b) {
        const double t = b * 128.0 / 48000.0;
        while (nextTick < t + 128.0 / 48000.0) {
            c->sendEvent(Event::control(EventType::Clock, 0.0f, (int)ClockMsg::Tick));
            nextTick += tick;
        }
        if (b == (int)(1.0 * 48000 / 128)) {
            c->sendEvent(Event::noteOn(1, 60, 1.0f));
            c->sendEvent(Event::noteOn(2, 72, 1.0f));
        }
        c->process(l.data(), r.data(), 128);
        out.insert(out.end(), l.begin(), l.end());
    }
    // find the note onsets after 1 s and check their spacing
    std::vector<double> onsets;
    bool on = false;
    for (size_t i = 48000; i < out.size(); i += 48) {
        double s = 0;
        for (size_t j = i; j < i + 48; ++j) s += std::fabs(out[j]);
        const bool now = s / 48 > 0.01;
        if (now && !on) onsets.push_back((double)i / 48000.0);
        on = now;
    }
    CHECK(onsets.size() >= 8);
    for (size_t k = 2; k + 1 < onsets.size() && k < 10; ++k) CHECK_NEAR(onsets[k + 1] - onsets[k], 0.15, 0.004);
}

TEST(arp_multi_parts_independent) {
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    for (int k = 0; k < 2; ++k) {
        e.setPartParam(k, P::layer(0, P::VaWave1), (float)VaWave::Sine);
        e.setPartParam(k, P::layer(0, P::VaMix), 0.0f);
        e.setPartParam(k, P::FilterType, 0.0f);
        e.setPartParam(k, P::AmpSustain, 1.0f);
        e.setPartParam(k, P::AmpRelease, 0.005f);
        e.setPartParam(k, P::mod(0, P::ModAmt), 0.0f);
    }
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setPartParam(1, P::ArpOn, 1.0f);  // part 2 arpeggiates, part 1 plays normally
    Event a = Event::noteOn(1, 48, 1.0f), b = Event::noteOn(2, 72, 1.0f);
    a.channel = 0;
    b.channel = 1;
    e.sendEvent(a);
    e.sendEvent(b);
    std::vector<float> l(128), r(128);
    int maxP1 = 0, minP1 = 9;
    for (int i = 0; i < 300; ++i) {
        e.process(l.data(), r.data(), 128);
        if (i > 10) {
            maxP1 = std::max(maxP1, e.partActiveVoices(1));
            minP1 = std::min(minP1, e.partActiveVoices(1));
        }
        if (i > 10) CHECK(e.partActiveVoices(0) == 1);
    }
    CHECK(maxP1 == 1 && minP1 == 0);  // part 2 gates on and off
}

// ------------------------------------------- regressions from code review

TEST(review_poly_notes_released_after_switch_to_mono) {
    Engine e;
    e.prepare(48000);
    sinePatch(e);
    e.setParam(P::AmpRelease, 0.01f);
    e.sendEvent(Event::noteOn(1, 60, 1.0f));
    e.sendEvent(Event::noteOn(2, 64, 1.0f));
    std::vector<float> l(128), r(128);
    for (int b = 0; b < 20; ++b) e.process(l.data(), r.data(), 128);
    e.setParam(P::VoiceMode, (float)VoiceMode::Mono);
    e.sendEvent(Event::noteOff(1, 60));
    e.sendEvent(Event::noteOff(2, 64));
    for (int b = 0; b < 100; ++b) e.process(l.data(), r.data(), 128);
    CHECK(e.stats().activeVoices == 0);
}

TEST(review_omni_note_off_after_select_part) {
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    e.setPartParam(0, P::AmpRelease, 0.01f);
    e.sendEvent(Event::noteOn(1, 60, 1.0f));  // omni -> part 1
    std::vector<float> l(128), r(128);
    for (int b = 0; b < 10; ++b) e.process(l.data(), r.data(), 128);
    e.selectPart(3);
    e.sendEvent(Event::noteOff(1, 60));
    for (int b = 0; b < 100; ++b) e.process(l.data(), r.data(), 128);
    CHECK(e.stats().activeVoices == 0);
}

TEST(review_panic_stops_latched_arp) {
    std::unique_ptr<Engine> e(arpEngine(ArpMode::Up));
    e->setParam(P::ArpLatch, 1.0f);
    e->sendEvent(Event::noteOn(1, 60, 1.0f));
    e->sendEvent(Event::noteOff(1, 60));
    std::vector<float> l(128), r(128);
    for (int b = 0; b < 100; ++b) e->process(l.data(), r.data(), 128);
    e->sendEvent(Event::control(EventType::AllNotesOff, 0));
    int maxActive = 0;
    for (int b = 0; b < 400; ++b) {
        e->process(l.data(), r.data(), 128);
        if (b > 20) maxActive = std::max(maxActive, e->stats().activeVoices);
    }
    CHECK(maxActive == 0);
    // latch switched off with a latched chord and no keys down: the arp stops
    e->sendEvent(Event::noteOn(2, 64, 1.0f));
    e->sendEvent(Event::noteOff(2, 64));
    for (int b = 0; b < 50; ++b) e->process(l.data(), r.data(), 128);
    e->setParam(P::ArpLatch, 0.0f);
    maxActive = 0;
    for (int b = 0; b < 400; ++b) {
        e->process(l.data(), r.data(), 128);
        if (b > 40) maxActive = std::max(maxActive, e->stats().activeVoices);  // after the current gate (62.5 ms)
    }
    CHECK(maxActive == 0);
    // arp.rate has no "off" option
    CHECK(paramTable()[(size_t)P::ArpRate].options.front() == "1/1");
}

TEST(review_arp_follows_clock_start_and_stop) {
    std::unique_ptr<Engine> e(arpEngine(ArpMode::Up));
    e->setParam(P::ArpLatch, 1.0f);
    e->setParam(P::fxChain(P::FxClockSync), 1.0f);
    std::vector<float> l(128), r(128);
    const double tick = 60.0 / (120.0 * 24.0);
    double nextTick = 0.0;
    int onsets = 0;
    bool on = false;
    auto run = [&](double from, double to, bool countOnsets) {
        int count = 0;
        for (double t = from; t < to; t += 128.0 / 48000.0) {
            while (nextTick < t + 128.0 / 48000.0) {
                e->sendEvent(Event::control(EventType::Clock, 0.0f, (int)ClockMsg::Tick));
                nextTick += tick;
            }
            e->process(l.data(), r.data(), 128);
            const bool now = e->stats().activeVoices > 0;
            if (countOnsets && now && !on) ++count;
            on = now;
        }
        return count;
    };
    run(0.0, 1.0, false);
    e->sendEvent(Event::noteOn(1, 60, 1.0f));
    e->sendEvent(Event::noteOff(1, 60));
    onsets = run(1.0, 3.0, true);
    INFO("arp onsets before Start: %d\n", onsets);
    CHECK(onsets >= 12);
    e->sendEvent(Event::control(EventType::Clock, 0.0f, (int)ClockMsg::Start));  // beat jumps back to 0
    onsets = run(3.0, 5.0, true);
    INFO("arp onsets after Start: %d\n", onsets);
    CHECK(onsets >= 12);
    e->sendEvent(Event::control(EventType::Clock, 0.0f, (int)ClockMsg::Stop));  // ticks keep coming
    run(5.0, 5.2, false);
    CHECK(e->stats().activeVoices == 0);  // no hanging arp notes while stopped
    CHECK(run(5.2, 6.0, true) == 0);
    e->sendEvent(Event::control(EventType::Clock, 0.0f, (int)ClockMsg::Continue));
    CHECK(run(6.0, 7.0, true) >= 5);
}

TEST(review_mono_glide_source_with_unison) {
    auto startFreq = [](int unison) {
        Engine e;
        e.prepare(48000);
        sinePatch(e);
        e.setParam(P::VoiceMode, (float)VoiceMode::Mono);
        e.setParam(P::Glide, 1.0f);
        e.setParam(P::Unison, (float)unison);
        e.setParam(P::UnisonDetune, 100.0f);
        e.setParam(P::UnisonSpread, 0.0f);
        RenderJob job;
        job.seconds = 1.3;
        job.events.push_back({0.0, Event::noteOn(1, 57, 1.0f)});
        job.events.push_back({1.2, Event::noteOn(2, 69, 1.0f)});
        const auto L = renderJob(e, job);
        return freqAt(L, 1.205, 1.225);  // just after the new note: still near the old pitch
    };
    const double one = startFreq(1), two = startFreq(2);
    INFO("glide start: unison 1 %.1f Hz, unison 2 %.1f Hz\n", one, two);
    CHECK(std::fabs(12.0 * std::log2(two / one)) < 0.25);  // was half a semitone sharp
}
