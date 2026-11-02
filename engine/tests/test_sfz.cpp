// SFZ playback features: release triggers, exclusive groups, round robin,
// keyswitches, envelope delay / hold, loop crossfade, region filters.
#include <filesystem>
#include <fstream>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/wav.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;
namespace fs = std::filesystem;

namespace {

constexpr float kSr = 48000.0f;

std::string dirFor(const char* name) {
    fs::path p = fs::temp_directory_path() / "hsynth_tests" / name;
    fs::create_directories(p);
    return p.generic_string();
}

void wavSine(const std::string& path, float freq, double seconds, float amp = 0.5f) {
    std::vector<float> v((size_t)(seconds * kSr));
    for (size_t i = 0; i < v.size(); ++i) v[i] = amp * std::sin(kTwoPi * freq * (float)i / kSr);
    std::string err;
    writeWav(path, v, nullptr, (int)kSr, false, err);
}

// Engine with one PCM layer playing `sfz` (written to dir/t.sfz).
std::unique_ptr<Engine> pcmEngine(const std::string& dir, const std::string& sfz, std::vector<std::string>* warn = nullptr) {
    std::ofstream(dir + "/t.sfz") << sfz;
    auto e = std::make_unique<Engine>();
    e->prepare(kSr);
    e->setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    e->setParam(P::FilterType, (float)FilterType::Off);
    e->setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e->setParam(P::mod(0, P::ModAmt), 0.0f);
    e->setParam(P::AmpSustain, 1.0f);
    e->setParam(P::AmpRelease, 0.02f);
    e->setParam(P::AmpVelSens, 0.0f);
    std::string err;
    std::vector<std::string> w;
    CHECK(loadLayerSamples(*e, 0, dir + "/t.sfz", err, w));
    if (warn) *warn = w;
    for (auto& s : w) INFO("sfz warning: %s\n", s.c_str());
    return e;
}

std::vector<float> render(Engine& e, RenderJob job) {
    std::stable_sort(job.events.begin(), job.events.end(),
                     [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.time < b.time; });
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    return L;
}

double tone(const std::vector<float>& x, double f, double t0, double t1) {
    return goertzel(x, kSr, f, (size_t)(t0 * kSr), (size_t)((t1 - t0) * kSr));
}

}  // namespace

TEST(sfz_release_trigger_and_rt_decay) {
    const std::string dir = dirFor("sfz_release");
    wavSine(dir + "/sus.wav", 440, 2.0);
    wavSine(dir + "/rel.wav", 1320, 0.5);
    std::vector<std::string> warn;
    auto e = pcmEngine(dir,
                       "<region> sample=sus.wav pitch_keycenter=60 loop_mode=loop_continuous\n"
                       "<region> sample=rel.wav pitch_keycenter=60 trigger=release rt_decay=10 ampeg_release=0.05\n",
                       &warn);
    CHECK(warn.empty());
    auto run = [&](double hold) {
        RenderJob job;
        job.seconds = hold + 1.0;
        addNote(job, 0.0, hold, 60, 1.0f);
        return render(*e, job);
    };
    const auto shortNote = run(0.1), longNote = run(1.0);
    // after note-off: the release region sounds, the sustain region is gone
    const double rel = tone(shortNote, 1320, 0.15, 0.3), sus = tone(shortNote, 440, 0.15, 0.3);
    INFO("after release: 1320 %g, 440 %g\n", rel, sus);
    CHECK(rel > 0.1 && sus < 0.01 * rel);
    CHECK(tone(shortNote, 1320, 0.02, 0.09) < 1e-3);  // nothing before note-off
    // rt_decay 10 dB/s: held 0.9 s longer -> ~9 dB quieter
    const double relLong = tone(longNote, 1320, 1.05, 1.2);
    INFO("rt_decay: %.2f dB\n", 20 * std::log10(relLong / rel));
    CHECK_NEAR(20 * std::log10(relLong / rel), -9.0, 1.0);
    CHECK(e->stats().activeVoices == 0);
}

TEST(sfz_exclusive_group_choke) {
    const std::string dir = dirFor("sfz_choke");
    wavSine(dir + "/open.wav", 3000, 2.0);
    wavSine(dir + "/closed.wav", 500, 0.3);
    auto e = pcmEngine(dir,
                       "<region> sample=open.wav key=46 group=1 off_by=2 loop_mode=one_shot\n"
                       "<region> sample=closed.wav key=42 group=2 loop_mode=one_shot\n");
    RenderJob job;
    job.seconds = 1.0;
    addNote(job, 0.0, 0.05, 46, 1.0f);   // open hi-hat rings (one shot)
    addNote(job, 0.5, 0.05, 42, 1.0f);   // closed hi-hat chokes it
    const auto L = render(*e, job);
    const double before = tone(L, 3000, 0.3, 0.45), after = tone(L, 3000, 0.55, 0.7);
    INFO("open hat before %g, after choke %g\n", before, after);
    CHECK(before > 0.1);
    CHECK(after < 1e-3 * before);
    CHECK(tone(L, 500, 0.55, 0.7) > 0.1);
}

TEST(sfz_round_robin_sequence) {
    const std::string dir = dirFor("sfz_seq");
    const float f[3] = {300, 450, 675};
    for (int i = 0; i < 3; ++i) wavSine(dir + "/rr" + std::to_string(i) + ".wav", f[i], 0.5);
    auto e = pcmEngine(dir,
                       "<group> key=60 seq_length=3\n"
                       "<region> sample=rr0.wav seq_position=1\n"
                       "<region> sample=rr1.wav seq_position=2\n"
                       "<region> sample=rr2.wav seq_position=3\n");
    RenderJob job;
    job.seconds = 1.5;
    for (int i = 0; i < 6; ++i) addNote(job, 0.25 * i, 0.2, 60, 1.0f);
    const auto L = render(*e, job);
    for (int i = 0; i < 6; ++i) {
        const double t0 = 0.25 * i + 0.03, t1 = t0 + 0.15;
        const double want = tone(L, f[i % 3], t0, t1), other = tone(L, f[(i + 1) % 3], t0, t1);
        CHECK(want > 20 * other);
    }
}

TEST(sfz_keyswitch) {
    const std::string dir = dirFor("sfz_sw");
    wavSine(dir + "/legato.wav", 440, 1.0);
    wavSine(dir + "/pizz.wav", 880, 1.0);
    auto e = pcmEngine(dir,
                       "<global> sw_lokey=24 sw_hikey=25 sw_default=24\n"
                       "<region> sample=legato.wav lokey=36 hikey=96 pitch_keycenter=60 sw_last=24\n"
                       "<region> sample=pizz.wav lokey=36 hikey=96 pitch_keycenter=60 sw_last=25\n");
    RenderJob job;
    job.seconds = 1.2;
    addNote(job, 0.0, 0.3, 60, 1.0f);   // default switch: legato
    addNote(job, 0.4, 0.05, 25, 1.0f);  // switch to pizz (silent)
    addNote(job, 0.6, 0.3, 60, 1.0f);
    const auto L = render(*e, job);
    CHECK(tone(L, 440, 0.05, 0.25) > 20 * tone(L, 880, 0.05, 0.25));
    CHECK(rms(L, (size_t)(0.42 * kSr), (size_t)(0.58 * kSr)) < 1e-4);
    CHECK(tone(L, 880, 0.65, 0.85) > 20 * tone(L, 440, 0.65, 0.85));
}

TEST(sfz_delay_and_envelope_delay_hold) {
    const std::string dir = dirFor("sfz_delay");
    wavSine(dir + "/s.wav", 440, 2.0);
    auto e = pcmEngine(dir,
                       "<region> sample=s.wav key=60 delay=0.2\n"
                       "<region> sample=s.wav key=62 pitch_keycenter=62 ampeg_delay=0.1 ampeg_attack=0.001 ampeg_hold=0.3 "
                       "ampeg_decay=0.05 ampeg_sustain=10\n");
    RenderJob job;
    job.seconds = 1.0;
    addNote(job, 0.0, 0.8, 60, 1.0f);
    std::vector<float> L = render(*e, job);
    CHECK(rms(L, 0, (size_t)(0.19 * kSr)) < 1e-6);
    CHECK(rms(L, (size_t)(0.22 * kSr), (size_t)(0.4 * kSr)) > 0.1);
    job.events.clear();
    addNote(job, 0.0, 0.8, 62, 1.0f);
    L = render(*e, job);
    const double pre = rms(L, 0, (size_t)(0.09 * kSr)), held = rms(L, (size_t)(0.15 * kSr), (size_t)(0.38 * kSr)),
                 sus = rms(L, (size_t)(0.55 * kSr), (size_t)(0.75 * kSr));
    INFO("env delay %g, hold %g, sustain %g\n", pre, held, sus);
    CHECK(pre < 1e-6);
    CHECK(held > 0.13);                  // full level during attack + hold (0.354 x layer 0.8 x master 0.49)
    CHECK_NEAR(sus / held, 0.1, 0.02);   // ampeg_sustain=10 %
}

TEST(sfz_loop_crossfade_smooths_seam) {
    const std::string dir = dirFor("sfz_xfade");
    // 441.3 Hz in a loop of 10000 frames: the loop seam is a hard jump
    wavSine(dir + "/s.wav", 441.3f, 1.0);
    auto maxJump = [&](const std::string& extra) {
        auto e = pcmEngine(dir, "<region> sample=s.wav pitch_keycenter=60 loop_mode=loop_continuous loop_start=20000 "
                                "loop_end=29999 " + extra + "\n");
        RenderJob job;
        job.seconds = 2.0;
        addNote(job, 0.0, 1.9, 60, 1.0f);
        const auto L = render(*e, job);
        double m = 0;
        for (size_t i = 33000; i < 90000; ++i) {
            // second difference: a smooth sine has |d2| ~ (2 pi f / sr)^2 * amp
            const double d2 = L[i] - 2 * L[i - 1] + L[i - 2];
            m = std::max(m, std::fabs(d2));
        }
        return m;
    };
    const double hard = maxJump(""), smooth = maxJump("loop_crossfade=0.05");
    INFO("max second difference: no crossfade %g, crossfade %g\n", hard, smooth);
    CHECK(smooth < 0.3 * hard);
}

TEST(sfz_region_filter_and_amp_keytrack) {
    const std::string dir = dirFor("sfz_filter");
    Rng r(5);
    std::vector<float> noise(48000);
    for (auto& x : noise) x = 0.3f * r.bipolar();
    std::string err;
    writeWav(dir + "/n.wav", noise, nullptr, 48000, false, err);
    auto e = pcmEngine(dir,
                       "<region> sample=n.wav key=60\n"
                       "<region> sample=n.wav key=61 pitch_keycenter=61 cutoff=500 fil_type=lpf_2p\n"
                       "<region> sample=n.wav key=62 pitch_keycenter=62 cutoff=500 fil_type=hpf_2p\n"
                       "<region> sample=n.wav key=72 pitch_keycenter=72 amp_keytrack=-0.5 amp_keycenter=60\n");
    auto bandRms = [&](int key, double f) {
        RenderJob job;
        job.seconds = 0.6;
        addNote(job, 0.0, 0.5, key, 1.0f);
        const auto L = render(*e, job);
        double s = 0;
        for (double k = 0.8; k <= 1.25; k += 0.05) s += tone(L, f * k, 0.05, 0.45);
        return s;
    };
    const double dryHi = bandRms(60, 6000), lpHi = bandRms(61, 6000), hpLo = bandRms(62, 100), dryLo = bandRms(60, 100);
    INFO("lpf: 6 kHz %g -> %g, hpf: 100 Hz %g -> %g\n", dryHi, lpHi, dryLo, hpLo);
    CHECK(lpHi < 0.05 * dryHi);
    CHECK(hpLo < 0.1 * dryLo);
    // amp_keytrack -0.5 dB/key, 12 keys above the centre -> -6 dB
    RenderJob job;
    job.seconds = 0.6;
    addNote(job, 0.0, 0.5, 72, 1.0f);
    const auto k72 = render(*e, job);
    job.events.clear();
    addNote(job, 0.0, 0.5, 60, 1.0f);
    const auto k60 = render(*e, job);
    CHECK_NEAR(20 * std::log10(rms(k72, 2400, 21600) / rms(k60, 2400, 21600)), -6.0, 0.3);
}
