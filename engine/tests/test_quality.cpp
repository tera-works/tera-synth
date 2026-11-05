// Sound-quality regression tests: aliasing of oscillators and sample playback
// measured from engine renders (alias-to-harmonic energy, see aliasDb()).
#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

constexpr double kSr = 48000.0;
double hz(int key) { return 440.0 * std::pow(2.0, (key - 69) / 12.0); }

void plain(Engine& e) {
    e.setParam(P::FilterType, 0);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0);
    e.setParam(P::mod(0, P::ModAmt), 0);
    e.setParam(P::AmpSustain, 1);
    e.setParam(P::Limiter, 0);
}

std::vector<float> steadyNote(Engine& e, int key) {
    RenderJob job;
    job.seconds = 1.0;
    job.events.push_back({0.0, Event::noteOn(1, key, 1.0f)});
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    e.sendEvent(Event::control(EventType::AllSoundOff, 0));
    return L;
}

// Band-limited saw (harmonics up to 21 kHz) at 110 Hz, root A2, seamless loop.
std::unique_ptr<SampleSet> fullBandSaw() {
    auto set = std::make_unique<SampleSet>();
    auto s = std::make_unique<Sample>();
    s->channels = 1;
    s->sampleRate = 48000;
    s->frames = 96000;
    s->data[0].assign(96000 + Sample::kPadFrames, 0.0f);
    for (int i = 0; i < 96000; ++i) {
        double v = 0;
        for (int h = 1; h * 110.0 < 21000.0; ++h) v += std::sin(2 * M_PI * 110.0 * h * i / 48000.0) / h;
        s->data[0][(size_t)i] = (float)(0.3 * v);
    }
    Region r;
    r.sample = s.get();
    r.keycenter = 45;
    r.loopMode = LoopMode::Continuous;
    r.loopStart = 4800;
    r.loopEnd = 4800 + 48000;  // exactly 110 cycles
    set->regions.push_back(r);
    set->samples.push_back(std::move(s));
    set->prepareMips();
    return set;
}

}  // namespace

TEST(quality_wavetable_bright_frames) {
    Engine e;
    e.prepare(48000);
    plain(e);
    e.setParam(P::layer(0, P::LType), (float)LayerType::WT);
    for (float pos : {0.5f, 1.0f}) {
        e.setParam(P::layer(0, P::WtPos), pos);
        for (int key : {48, 72, 96}) {
            const double all = aliasDb(steadyNote(e, key), 9600, hz(key));
            INFO("wavetable basic pos %.1f key %d: %.1f dB\n", pos, key, all);
            CHECK(all < -55.0);  // linear reads of 4x tables gave -40 .. -50 dB
        }
    }
}

TEST(quality_va_saw_audible_alias) {
    Engine e;
    e.prepare(48000);
    plain(e);
    e.setParam(P::layer(0, P::VaMix), 0.0f);
    for (int key : {72, 96}) {
        const double lo = aliasDb(steadyNote(e, key), 9600, hz(key), kSr, 10000.0);
        INFO("VA saw key %d: alias below 10 kHz %.1f dB\n", key, lo);
        CHECK(lo < -50.0);
    }
}

TEST(quality_pcm_pitch_up_band_limited) {
    auto measure = [](Quality q, int key, double maxHz) {
        Engine e;
        e.prepare(48000);
        plain(e);
        e.setQuality(q);
        e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
        e.setSampleSet(0, fullBandSaw());
        return aliasDb(steadyNote(e, key), 9600, hz(key), kSr, maxHz);
    };
    for (int key : {52, 57, 69, 81, 88}) {  // +7 .. +43 semitones (level 3 above +36)
        const double good = measure(Quality::Normal, key, 10000.0), cubic = measure(Quality::Low, key, 10000.0);
        INFO("PCM +%d st: alias below 10 kHz %.1f dB (cubic %.1f dB)\n", key - 45, good, cubic);
        CHECK(good < -75.0);
        CHECK(good < cubic - 30.0);
    }
    // at the recorded pitch both paths are identical (cubic)
    CHECK(measure(Quality::Normal, 45, 1e9) < -85.0);
}

TEST(quality_pcm_sinc_gain_and_loop_seam) {
    // DC sample: the polyphase kernels have unity gain at every phase / cutoff
    auto set = std::make_unique<SampleSet>();
    auto s = std::make_unique<Sample>();
    s->channels = 1;
    s->sampleRate = 48000;
    s->frames = 48000;
    s->data[0].assign(48000 + Sample::kPadFrames, 0.5f);
    Region r;
    r.sample = s.get();
    r.keycenter = 36;
    r.loopMode = LoopMode::Continuous;
    r.loopStart = 1000;
    r.loopEnd = 47000;
    set->regions.push_back(r);
    set->samples.push_back(std::move(s));
    set->prepareMips();
    CHECK(set->samples[0]->mipLevels == 3);  // whole keyboard above key 36: up to 1/8 rate
    Engine e;
    e.prepare(48000);
    plain(e);
    e.setParam(P::AmpVelSens, 0.0f);
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    e.setSampleSet(0, std::move(set));
    for (int key : {37, 50, 61, 79, 96}) {  // sinc on level 0, 1, 2, 3 (+60: beyond the mips)
        const auto L = steadyNote(e, key);
        double lo = 1e9, hi = -1e9;
        for (size_t i = 9600; i < 48000; ++i) { lo = std::min(lo, (double)L[i]); hi = std::max(hi, (double)L[i]); }
        INFO("DC through key %d: %.5f .. %.5f\n", key, lo, hi);
        CHECK(hi - lo < 0.002 * hi);  // flat: no loop-seam or phase ripple
    }
}

TEST(quality_mips_only_where_needed) {
    // a multisample whose zone spans 3 semitones above its root needs no mips
    auto set = std::make_unique<SampleSet>();
    auto s = std::make_unique<Sample>();
    s->frames = 1000;
    s->data[0].assign(1000 + Sample::kPadFrames, 0.0f);
    Region r;
    r.sample = s.get();
    r.keycenter = 60;
    r.lokey = 58;
    r.hikey = 63;
    set->regions.push_back(r);
    set->samples.push_back(std::move(s));
    set->prepareMips();
    CHECK(set->samples[0]->mipLevels == 0);
    // ... one octave above the root (+ bend headroom) needs one level
    set->regions[0].hikey = 72;
    set->prepareMips();
    CHECK(set->samples[0]->mipLevels == 1);
    CHECK(set->samples[0]->mipFrames[0] == 500);
}

TEST(quality_high_antialiased_drive_and_saturate) {
    auto measure = [](Quality q, int key, bool saturate) {
        Engine e;
        e.prepare(48000);
        plain(e);
        e.setQuality(q);
        e.setParam(P::layer(0, P::VaMix), 0.0f);
        if (saturate) {
            e.setParam(P::layer(0, P::VaWave1), (float)VaWave::Sine);
            e.setParam(P::VfxType, (float)VfxType::Saturate);
            e.setParam(P::VfxAmount, 0.8f);
        } else {
            e.setParam(P::FilterType, (float)FilterType::LP24);
            e.setParam(P::FilterCutoff, 20000.0f);
            e.setParam(P::FilterKeytrack, 0.0f);
            e.setParam(P::FilterEnvAmt, 0.0f);
            e.setParam(P::FilterDrive, 1.0f);
        }
        return aliasDb(steadyNote(e, key), 9600, hz(key), kSr, 10000.0);
    };
    for (int key : {84, 96}) {
        const double dn = measure(Quality::Normal, key, false), dh = measure(Quality::High, key, false);
        const double sn = measure(Quality::Normal, key, true), sh = measure(Quality::High, key, true);
        INFO("key %d: drive %.1f -> %.1f dB, saturate %.1f -> %.1f dB (Normal -> High)\n", key, dn, dh, sn, sh);
        CHECK(dh < dn - 10.0);
        CHECK(sh < sn - 10.0);
    }
}
