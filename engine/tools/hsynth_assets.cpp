// Generates the self-made demo content: test waveforms, multisampled
// instruments with SFZ mappings, and factory presets. No third-party audio.
//
// usage: hsynth_assets <output-dir>
//   writes <out>/library/factory/... and <out>/presets/*.json
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hsynth/common.h"
#include "hsynth/fx/fx_params.h"
#include "hsynth/preset.h"
#include "hsynth/wav.h"

using namespace hsynth;
namespace fs = std::filesystem;

namespace {

constexpr int kSr = 48000;
const double kTau = 6.283185307179586;

void writeOrDie(const std::string& path, const std::vector<float>& l, const std::vector<float>* r = nullptr,
                int unity = -1, int64_t ls = -1, int64_t le = -1) {
    std::string err;
    if (!writeWav(path, l, r, kSr, false, err, unity, ls, le)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        std::exit(1);
    }
    std::printf("  %s\n", path.c_str());
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
    std::printf("  %s\n", path.c_str());
}

void normalize(std::vector<float>& v, float target) {
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    if (p > 0) for (float& x : v) x *= target / p;
}

double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// ---------------------------------------------------------------- test waves

void testWaves(const std::string& dir) {
    fs::create_directories(dir);
    std::vector<float> v(kSr * 2);
    for (size_t i = 0; i < v.size(); ++i) v[i] = 0.5f * (float)std::sin(kTau * 1000.0 * (double)i / kSr);
    writeOrDie(dir + "/sine_1khz.wav", v);
    // logarithmic sine sweep 20 Hz - 20 kHz, 5 s
    v.assign(kSr * 5, 0.0f);
    {
        const double f0 = 20, f1 = 20000, T = 5.0, k = std::log(f1 / f0);
        for (size_t i = 0; i < v.size(); ++i) {
            const double t = (double)i / kSr;
            v[i] = 0.5f * (float)std::sin(kTau * f0 * T / k * (std::exp(t / T * k) - 1.0));
        }
    }
    writeOrDie(dir + "/sweep_20_20k.wav", v);
    // impulse train (1 per second)
    v.assign(kSr * 3, 0.0f);
    for (int s = 0; s < 3; ++s) v[(size_t)(s * kSr)] = 0.9f;
    writeOrDie(dir + "/impulses.wav", v);
    // white noise burst
    Rng rng(99);
    v.assign(kSr, 0.0f);
    for (auto& x : v) x = 0.3f * rng.bipolar();
    writeOrDie(dir + "/noise_1s.wav", v);
    // stereo channel identification: L = 440 Hz, R = 880 Hz
    std::vector<float> l(kSr), r(kSr);
    for (int i = 0; i < kSr; ++i) {
        l[(size_t)i] = 0.4f * (float)std::sin(kTau * 440.0 * i / kSr);
        r[(size_t)i] = 0.4f * (float)std::sin(kTau * 880.0 * i / kSr);
    }
    writeOrDie(dir + "/stereo_440L_880R.wav", l, &r);
    // single-cycle-loop sine at C4 with smpl loop (for loop testing)
    const int period = 4800;  // 100 cycles of 1000 Hz? use 261.63 approx: build exact-integer loop
    v.assign((size_t)period * 2, 0.0f);
    for (size_t i = 0; i < v.size(); ++i) v[i] = 0.5f * (float)std::sin(kTau * 260.0 * (double)i / kSr);  // 260 Hz: 26 cycles per 4800
    writeOrDie(dir + "/loop_260hz.wav", v, nullptr, 60, period, period * 2 - 1);
}

// ---------------------------------------------------------- glass piano

std::vector<float> pianoTone(int note, bool loud, double seconds) {
    const double f = noteHz(note);
    std::vector<float> v((size_t)(seconds * kSr));
    const int partials = loud ? 14 : 7;
    for (int k = 1; k <= partials; ++k) {
        const double inharm = 1.0 + 0.0004 * k * k;  // slight stiffness
        const double fk = f * k * inharm;
        if (fk > kSr * 0.45) break;
        const double amp = (loud ? 1.0 : 0.7) / std::pow((double)k, loud ? 1.0 : 1.6);
        const double decay = 1.2 + 2.5 / k + (60 - note) * 0.02;
        for (size_t i = 0; i < v.size(); ++i) {
            const double t = (double)i / kSr;
            v[i] += (float)(amp * std::exp(-t / std::max(0.2, decay) * k * 0.6) * std::sin(kTau * fk * t + k));
        }
    }
    // hammer transient
    Rng rng((uint32_t)note * 7 + (loud ? 1 : 2));
    for (size_t i = 0; i < 400; ++i) v[i] += (float)((loud ? 0.25 : 0.08) * rng.bipolar() * std::exp(-(double)i / 80.0));
    // short fade-out at the end
    for (size_t i = 0; i < 2400; ++i) v[v.size() - 1 - i] *= (float)i / 2400.0f;
    normalize(v, loud ? 0.9f : 0.5f);
    return v;
}

void glassPiano(const std::string& dir) {
    fs::create_directories(dir + "/samples");
    const int notes[] = {36, 48, 60, 72, 84};
    std::string sfz =
        "// Glass Piano - synthesized multisample for HSynth (self-made, CC0)\n"
        "<control> default_path=samples/\n"
        "<global> ampeg_release=0.6 amp_veltrack=60\n";
    for (int i = 0; i < 5; ++i) {
        const int n = notes[i];
        const int lo = i == 0 ? 0 : n - 6, hi = i == 4 ? 127 : n + 5;
        for (int loud = 0; loud < 2; ++loud) {
            const std::string name = "gp_" + std::to_string(n) + (loud ? "_f" : "_p") + ".wav";
            writeOrDie(dir + "/samples/" + name, pianoTone(n, loud != 0, n < 60 ? 3.0 : 2.0), nullptr, n);
            sfz += "<region> sample=" + name + " lokey=" + std::to_string(lo) + " hikey=" + std::to_string(hi) +
                   " pitch_keycenter=" + std::to_string(n) + (loud ? " lovel=80" : " hivel=79") + "\n";
        }
    }
    writeText(dir + "/glass_piano.sfz", sfz);
}

// -------------------------------------------------------------- loop pad

void loopPad(const std::string& dir) {
    fs::create_directories(dir);
    // 1-second loop; every partial frequency is an integer number of Hz so the
    // loop is seamless. Fundamental 131 Hz ~ C3 (-3.6 cents) -> tune in SFZ.
    const int loopLen = kSr;
    const int attack = kSr / 2;
    std::vector<float> l((size_t)(attack + loopLen)), r((size_t)(attack + loopLen));
    for (int k = 1; k <= 24; ++k) {
        const int f = 131 * k;
        if (f > 16000) break;
        const double amp = 1.0 / k;
        for (int d = -1; d <= 1; ++d) {  // three detuned copies (+-1 Hz)
            const double fk = f + d;
            const double ph = k * 1.3 + d * 2.1;
            for (size_t i = 0; i < l.size(); ++i) {
                const double t = (double)i / kSr;
                const double s = amp * std::sin(kTau * fk * t + ph);
                const double pan = 0.5 + 0.35 * d;
                l[i] += (float)(s * (1 - pan));
                r[i] += (float)(s * pan);
            }
        }
    }
    for (int i = 0; i < attack; ++i) {
        const float g = (float)i / attack;
        l[(size_t)i] *= g;
        r[(size_t)i] *= g;
    }
    float p = 0;
    for (size_t i = 0; i < l.size(); ++i) p = std::max(p, std::max(std::fabs(l[i]), std::fabs(r[i])));
    for (size_t i = 0; i < l.size(); ++i) { l[i] *= 0.6f / p; r[i] *= 0.6f / p; }
    writeOrDie(dir + "/pad_c3_loop.wav", l, &r, 48, attack, attack + loopLen - 1);
    writeText(dir + "/loop_pad.sfz",
              "// Loop Pad - additive stereo pad with seamless loop (self-made, CC0)\n"
              "<region> sample=pad_c3_loop.wav pitch_keycenter=48 tune=-4 loop_mode=loop_continuous\n");
}

// ------------------------------------------------------------- drum kit

void drumKit(const std::string& dir) {
    fs::create_directories(dir);
    Rng rng(1234);
    std::vector<float> kick((size_t)(kSr * 0.6));
    double ph = 0;
    for (size_t i = 0; i < kick.size(); ++i) {
        const double t = (double)i / kSr;
        const double f = 45 + 120 * std::exp(-t * 30);
        ph += kTau * f / kSr;
        kick[i] = (float)(std::sin(ph) * std::exp(-t * 6) + 0.3 * rng.bipolar() * std::exp(-t * 300));
    }
    normalize(kick, 0.95f);
    writeOrDie(dir + "/kick.wav", kick);

    std::vector<float> snare((size_t)(kSr * 0.4));
    float lp = 0;
    for (size_t i = 0; i < snare.size(); ++i) {
        const double t = (double)i / kSr;
        lp += 0.5f * (rng.bipolar() - lp);
        snare[i] = (float)(0.6 * std::sin(kTau * 190 * t) * std::exp(-t * 25) + lp * std::exp(-t * 12));
    }
    normalize(snare, 0.85f);
    writeOrDie(dir + "/snare.wav", snare);

    std::vector<float> hat((size_t)(kSr * 0.15));
    float prev = 0;
    for (size_t i = 0; i < hat.size(); ++i) {
        const float n = rng.bipolar();
        const float hp = n - prev;  // crude high-pass
        prev = n;
        hat[i] = (float)(hp * std::exp(-(double)i / kSr * 40));
    }
    normalize(hat, 0.6f);
    writeOrDie(dir + "/hat.wav", hat);

    writeText(dir + "/drum_kit.sfz",
              "// Synth Drum Kit (self-made, CC0)\n"
              "<group> loop_mode=one_shot amp_veltrack=80\n"
              "<region> sample=kick.wav key=36\n"
              "<region> sample=snare.wav key=38\n"
              "<region> sample=snare.wav key=40 tune=150\n"
              "<region> sample=hat.wav key=42 group=1\n"
              "<region> sample=hat.wav key=46 tune=-200 volume=-3 group=2 off_by=1\n");  // closed hat chokes the open one
}

// ---------------------------------------------------------------- presets

struct PB {
    Preset p;
    explicit PB(const std::string& name, const std::string& cat) { p.name = name; p.category = cat; p.author = "HSynth factory"; }
    PB& set(const std::string& n, float v) {
        const int id = findParam(n);
        if (id < 0 && applyLegacyParam(p, n, v)) return *this;  // older factory recipes
        if (id < 0) { std::fprintf(stderr, "unknown param %s\n", n.c_str()); std::exit(1); }
        p.params[(size_t)id] = sanitizeParam(id, v);
        return *this;
    }
    PB& e(const std::string& n, const std::string& label) {
        const int id = findParam(n);
        const auto& opts = paramTable()[(size_t)id].options;
        for (size_t i = 0; i < opts.size(); ++i)
            if (opts[i] == label) { p.params[(size_t)id] = (float)i; return *this; }
        std::fprintf(stderr, "bad enum %s=%s\n", n.c_str(), label.c_str());
        std::exit(1);
    }
    PB& mod(int slot, const std::string& src, const std::string& dst, float amt, const std::string& via = "none") {
        const std::string s = "mod" + std::to_string(slot);
        e(s + ".src", src); e(s + ".dst", dst); e(s + ".via", via);
        return set(s + ".amt", amt);
    }
    PB& sample(int layer, const std::string& path) { p.samples[layer] = path; return *this; }
    // effect rack: blocks in slot order (remaining slots empty)
    PB& chain(std::initializer_list<const char*> blocks) {
        int s = 1;
        for (const char* b : blocks) e("fx.slot" + std::to_string(s++), b);
        for (; s <= fx::kFxSlots; ++s) e("fx.slot" + std::to_string(s), "none");
        for (const char* b : blocks) set(std::string("fx.") + b + ".on", 1.0f);
        return *this;
    }
    PB& fx(const std::string& n, float v) { return set("fx." + n, v); }
    PB& fxe(const std::string& n, const std::string& label) { return e("fx." + n, label); }
    PB& fxmod(int slot, const std::string& src, const std::string& dst, float amt) {
        const std::string s = "fxmod" + std::to_string(slot);
        e(s + ".src", src); e(s + ".dst", dst);
        return set(s + ".amt", amt);
    }
    PB& comment(const std::string& c) { p.comment = c; return *this; }
};

void writePreset(const std::string& dir, const std::string& file, const PB& b) {
    std::string err;
    if (!savePresetFile(dir + "/" + file, b.p, err)) { std::fprintf(stderr, "%s\n", err.c_str()); std::exit(1); }
    std::printf("  %s/%s\n", dir.c_str(), file.c_str());
}

void presets(const std::string& dir) {
    fs::create_directories(dir);
    writePreset(dir, "00_init.json", PB("Init VA", "Init").comment("Default patch: one saw layer"));

    writePreset(dir, "01_va_brass.json",
                PB("VA Brass", "VA").e("a.va.wave1", "saw").e("a.va.wave2", "saw").set("a.va.detune", 9)
                    .set("filter.cutoff", 700).set("filter.reso", 0.2f).set("filter.envamt", 0.55f)
                    .set("env2.attack", 0.06f).set("env2.decay", 0.6f).set("env2.sustain", 0.4f)
                    .set("amp.attack", 0.03f).set("amp.sustain", 0.85f).set("amp.release", 0.25f)
                    .set("reverb.mix", 0.2f));

    writePreset(dir, "02_va_pad.json",
                PB("Warm VA Pad", "VA").e("a.va.wave1", "saw").e("a.va.wave2", "pulse").set("a.va.detune", 14)
                    .set("a.va.osc2semi", 12).set("a.va.mix", 0.35f).set("filter.cutoff", 1500).set("filter.envamt", 0.1f)
                    .set("amp.attack", 0.8f).set("amp.release", 1.6f).set("amp.sustain", 1.0f)
                    .set("chorus.mix", 0.6f).set("reverb.mix", 0.4f).set("reverb.size", 0.8f)
                    .mod(4, "lfo2", "cutoff", 0.15f).mod(5, "lfo1", "va.pw", 0.3f));

    writePreset(dir, "03_square_lead.json",
                PB("PWM Lead", "VA").e("a.va.wave1", "pulse").e("a.va.wave2", "square").set("a.va.osc2semi", -12)
                    .set("a.va.mix", 0.3f).set("filter.cutoff", 3000).set("filter.reso", 0.35f).set("filter.drive", 0.3f)
                    .set("amp.release", 0.15f).set("delay.mix", 0.25f).set("delay.time", 0.375f)
                    .mod(4, "lfo2", "va.pw", 0.5f).set("lfo2.rate", 0.8f).set("master.bendrange", 7));

    writePreset(dir, "04_fm_epiano.json",
                PB("FM E.Piano", "FM").e("a.type", "fm").e("a.fm.algo", "4: 4>3 + 2>1").set("a.fm.index", 0.35f)
                    .set("a.fm.op1.ratio", 1).set("a.fm.op1.decay", 3.0f).set("a.fm.op1.sustain", 0.0f).set("a.fm.op1.release", 0.4f)
                    .set("a.fm.op2.ratio", 1).set("a.fm.op2.level", 0.55f).set("a.fm.op2.decay", 1.6f).set("a.fm.op2.sustain", 0.1f).set("a.fm.op2.velsens", 0.9f)
                    .set("a.fm.op3.ratio", 1).set("a.fm.op3.level", 0.5f).set("a.fm.op3.decay", 2.0f).set("a.fm.op3.sustain", 0.0f)
                    .set("a.fm.op4.ratio", 14).set("a.fm.op4.level", 0.35f).set("a.fm.op4.decay", 0.25f).set("a.fm.op4.sustain", 0.0f).set("a.fm.op4.velsens", 1.0f)
                    .e("filter.type", "off").set("amp.sustain", 1.0f).set("amp.release", 0.4f)
                    .set("chorus.mix", 0.4f).set("reverb.mix", 0.2f).mod(4, "lfo2", "pan", 0.3f).set("lfo2.rate", 3.0f));

    writePreset(dir, "05_fm_bell.json",
                PB("FM Bell", "FM").e("a.type", "fm").e("a.fm.algo", "3: 4>3>1 + 2>1").set("a.fm.index", 0.45f)
                    .set("a.fm.op1.ratio", 1).set("a.fm.op1.decay", 6.0f).set("a.fm.op1.sustain", 0.0f).set("a.fm.op1.release", 2.0f)
                    .set("a.fm.op2.ratio", 3).set("a.fm.op2.detune", 7).set("a.fm.op2.level", 0.6f).set("a.fm.op2.decay", 4.0f).set("a.fm.op2.sustain", 0.0f)
                    .set("a.fm.op3.ratio", 7).set("a.fm.op3.level", 0.3f).set("a.fm.op3.decay", 1.5f).set("a.fm.op3.sustain", 0.0f)
                    .set("a.fm.op4.ratio", 2).set("a.fm.op4.level", 0.2f)
                    .e("filter.type", "off").set("amp.sustain", 1.0f).set("amp.release", 2.0f).set("reverb.mix", 0.35f).set("reverb.size", 0.85f));

    writePreset(dir, "06_fm_bass.json",
                PB("FM Bass", "FM").e("a.type", "fm").e("a.fm.algo", "1: 4>3>2>1").set("a.fm.index", 0.5f).set("a.fm.feedback", 0.4f)
                    .set("a.fm.op1.ratio", 1).set("a.fm.op1.sustain", 0.9f)
                    .set("a.fm.op2.ratio", 1).set("a.fm.op2.level", 0.7f).set("a.fm.op2.decay", 0.35f).set("a.fm.op2.sustain", 0.2f)
                    .set("a.fm.op3.ratio", 2).set("a.fm.op3.level", 0.2f).set("a.fm.op4.ratio", 1).set("a.fm.op4.level", 0.3f)
                    .set("filter.cutoff", 2500).set("filter.envamt", 0.2f).set("amp.release", 0.08f).set("reverb.mix", 0.0f));

    writePreset(dir, "07_glass_piano.json",
                PB("Glass Piano (PCM)", "PCM").e("a.type", "pcm").sample(0, "factory/glass_piano/glass_piano.sfz")
                    .e("filter.type", "lp12").set("filter.cutoff", 9000).set("filter.velamt", 0.4f).set("filter.envamt", 0)
                    .set("amp.velsens", 0.0f).set("amp.sustain", 1.0f).set("amp.release", 0.5f).set("reverb.mix", 0.25f));

    writePreset(dir, "08_hybrid_piano_fm.json",
                PB("Hybrid Piano + FM Tine", "Hybrid").e("a.type", "pcm").sample(0, "factory/glass_piano/glass_piano.sfz")
                    .e("b.type", "fm").set("b.level", 0.6f).e("b.fm.algo", "4: 4>3 + 2>1").set("b.fm.index", 0.4f)
                    .set("b.fm.op1.decay", 2.5f).set("b.fm.op1.sustain", 0.0f).set("b.fm.op2.ratio", 14).set("b.fm.op2.level", 0.4f)
                    .set("b.fm.op2.decay", 0.3f).set("b.fm.op2.sustain", 0.0f).set("b.fm.op3.ratio", 1).set("b.fm.op3.level", 0.4f)
                    .set("b.fm.op3.decay", 1.5f).set("b.fm.op3.sustain", 0.0f).set("b.fm.op4.ratio", 1).set("b.fm.op4.level", 0.5f)
                    .set("mix.morph", -0.5f).mod(4, "modwheel", "morph", 1.0f).mod(5, "notepressure", "fm.index", 0.5f)
                    .e("filter.type", "off").set("amp.velsens", 0.2f).set("amp.sustain", 1.0f).set("amp.release", 0.5f)
                    .set("chorus.mix", 0.3f).set("reverb.mix", 0.25f)
                    .comment("Mod wheel crossfades PCM piano -> FM tine; per-note pressure brightens the FM layer"));

    writePreset(dir, "09_hybrid_pad.json",
                PB("Hybrid Loop Pad", "Hybrid").e("a.type", "pcm").sample(0, "factory/loop_pad/loop_pad.sfz")
                    .e("b.type", "va").e("b.va.wave1", "saw").e("b.va.wave2", "saw").set("b.va.detune", 18).set("b.level", 0.4f)
                    .set("filter.cutoff", 2500).set("filter.envamt", 0.0f).set("amp.attack", 0.6f).set("amp.release", 1.5f)
                    .set("amp.sustain", 1.0f).set("chorus.mix", 0.4f).set("reverb.mix", 0.45f).set("reverb.size", 0.85f)
                    .mod(4, "lfo2", "morph", 0.4f).set("lfo2.rate", 0.15f).mod(5, "notetimbre", "cutoff", 0.5f));

    writePreset(dir, "10_drum_kit.json",
                PB("Synth Drum Kit (PCM)", "PCM").e("a.type", "pcm").sample(0, "factory/drum_kit/drum_kit.sfz")
                    .e("filter.type", "off").set("amp.velsens", 0.0f).set("amp.sustain", 1.0f).set("amp.release", 0.05f)
                    .set("reverb.mix", 0.1f).mod(1, "none", "none", 0.0f).comment("Keys 36 kick, 38/40 snare, 42/46 hats"));

    writePreset(dir, "11_expressive_lead.json",
                PB("Expressive Per-Note Lead", "Expressive").e("a.va.wave1", "saw").e("a.va.wave2", "square").set("a.va.mix", 0.4f)
                    .e("b.type", "fm").set("b.level", 0.4f).set("b.fm.index", 0.2f).set("b.fm.op2.ratio", 2)
                    .set("filter.cutoff", 900).set("filter.reso", 0.4f).set("filter.envamt", 0.1f)
                    .mod(2, "notetimbre", "cutoff", 0.6f).mod(3, "notepressure", "amp", 0.6f)
                    .mod(4, "notepressure", "fm.index", 0.8f).mod(5, "notex", "pan", 0.5f)
                    .mod(6, "lfo1", "pitch", 0.015f, "notepressure").set("master.bendrange", 12)
                    .set("amp.sustain", 0.9f).set("amp.release", 0.3f).set("delay.mix", 0.2f).set("reverb.mix", 0.25f)
                    .comment("Per-note: Y=timbre->cutoff, pressure->level/FM index/vibrato, X->pan"));

    writePreset(dir, "12_test_sine.json",
                PB("Test Sine", "Test").e("a.va.wave1", "sine").set("a.va.mix", 0.0f).e("filter.type", "off")
                    .set("amp.sustain", 1.0f).set("amp.velsens", 0.0f).set("reverb.mix", 0.0f).mod(1, "none", "none", 0)
                    .mod(2, "none", "none", 0).mod(3, "none", "none", 0).comment("Pure sine for latency / tuning checks"));

    // ---- effect rack showcases
    writePreset(dir, "13_amp_lead.json",
                PB("Amp Stack Lead", "FX").e("a.va.wave1", "saw").e("a.va.wave2", "square").set("a.va.osc2semi", 12).set("a.va.mix", 0.3f)
                    .set("filter.cutoff", 5000).set("filter.envamt", 0.1f).set("amp.release", 0.25f).set("master.bendrange", 12)
                    .chain({"comp", "drive", "amp", "cab", "delay", "reverb"})
                    .fx("comp.thresh", -22).fx("comp.ratio", 3).fxe("drive.type", "overdrive").fx("drive.amount", 0.35f).fx("drive.tight", 120)
                    .fxe("amp.model", "lead").fx("amp.gain", 6.5f).fx("amp.middle", 6).fxe("cab.model", "4x12 british")
                    .fxe("delay.type", "analog").fxe("delay.sync", "1/8.").fx("delay.feedback", 0.35f).fx("delay.mix", 0.22f)
                    .fxe("reverb.type", "plate").fx("reverb.decay", 1.8f).fx("reverb.mix", 0.12f)
                    .fxmod(1, "modwheel", "fx.drive.amount", 0.5f)
                    .comment("Compressor > overdrive > lead amp > 4x12 cab > dotted-eighth analog delay. Mod wheel adds drive."));

    writePreset(dir, "14_ensemble_strings.json",
                PB("Ensemble Strings", "FX").e("a.va.wave1", "saw").e("a.va.wave2", "saw").set("a.va.detune", 12)
                    .set("filter.cutoff", 3500).set("filter.envamt", 0.05f).set("amp.attack", 0.35f).set("amp.release", 1.2f).set("amp.sustain", 1.0f)
                    .chain({"chorus", "peq", "reverb"})
                    .fxe("chorus.mode", "ensemble").fx("chorus.mix", 0.65f).fx("chorus.rate", 0.5f).fx("chorus.depth", 0.6f)
                    .fx("peq.lowcut", 90).fx("peq.p2.freq", 350).fx("peq.p2.gain", -3).fx("peq.hs.gain", 2.5f)
                    .fxe("reverb.type", "hall").fx("reverb.size", 0.8f).fx("reverb.decay", 4.0f).fx("reverb.mix", 0.25f)
                    .comment("String-machine ensemble chorus (3-phase BBD style) into a large hall"));

    writePreset(dir, "15_rotary_organ.json",
                PB("Rotary Organ", "FX").e("a.type", "fm").e("a.fm.algo", "8: 1+2+3+4").set("a.fm.index", 0.0f)
                    .set("a.fm.op1.ratio", 1).set("a.fm.op1.sustain", 1.0f).set("a.fm.op1.attack", 0.003f)
                    .set("a.fm.op2.ratio", 2).set("a.fm.op2.level", 0.7f).set("a.fm.op2.sustain", 1.0f).set("a.fm.op2.velsens", 0)
                    .set("a.fm.op3.ratio", 3).set("a.fm.op3.level", 0.5f).set("a.fm.op3.sustain", 1.0f).set("a.fm.op3.velsens", 0)
                    .set("a.fm.op4.ratio", 4).set("a.fm.op4.level", 0.45f).set("a.fm.op4.sustain", 1.0f).set("a.fm.op4.velsens", 0)
                    .e("filter.type", "off").set("amp.attack", 0.002f).set("amp.sustain", 1.0f).set("amp.release", 0.05f).set("amp.velsens", 0)
                    .mod(1, "none", "none", 0).mod(2, "none", "none", 0).mod(3, "none", "none", 0)
                    .chain({"drive", "rotary", "reverb"})
                    .fxe("drive.type", "tube").fx("drive.amount", 0.25f).fx("drive.mix", 0.6f)
                    .fx("rotary.accel", 1.2f).fx("rotary.drive", 0.25f)
                    .fxe("reverb.type", "room").fx("reverb.mix", 0.12f)
                    .fxmod(1, "modwheel", "fx.rotary.fast", 1.0f)
                    .comment("Drawbar-style additive FM through a rotary speaker. Mod wheel: slow / fast."));

    writePreset(dir, "16_trance_gate.json",
                PB("Trance Gate Pad", "FX").e("a.va.wave1", "saw").e("a.va.wave2", "saw").set("a.va.detune", 16).set("a.va.osc2semi", 12)
                    .set("filter.cutoff", 2200).set("filter.envamt", 0.0f).set("amp.sustain", 1.0f).set("amp.release", 0.6f)
                    .set("fx.tempo", 138)
                    .chain({"slicer", "delay", "reverb"})
                    .fxe("slicer.pattern", "trance 1").fxe("slicer.division", "1/16").fx("slicer.duty", 0.55f).fx("slicer.smooth", 0.15f)
                    .fxe("delay.mode", "pingpong").fxe("delay.sync", "1/8.").fx("delay.feedback", 0.4f).fx("delay.mix", 0.25f)
                    .fxe("reverb.type", "hall").fx("reverb.mix", 0.18f)
                    .fxmod(1, "modwheel", "fx.slicer.depth", -0.8f)
                    .comment("138 bpm 16-step gate with ping-pong dotted-eighth delay. Mod wheel opens the gate."));

    writePreset(dir, "17_shimmer_pad.json",
                PB("Shimmer Pad", "FX").e("a.va.wave1", "pulse").e("a.va.wave2", "saw").set("a.va.detune", 9).set("a.va.mix", 0.4f)
                    .set("filter.cutoff", 1800).set("filter.envamt", 0.0f).set("amp.attack", 1.2f).set("amp.release", 2.5f).set("amp.sustain", 1.0f)
                    .chain({"chorus", "reverb"})
                    .fxe("chorus.mode", "dimension").fx("chorus.mix", 0.5f)
                    .fxe("reverb.type", "shimmer").fx("reverb.size", 0.9f).fx("reverb.decay", 8.0f).fx("reverb.shimmer", 0.6f)
                    .fx("reverb.mix", 0.45f).fx("reverb.predelay", 40)
                    .comment("Octave-up shimmer reverb tail over a dimension-chorused pad"));

    writePreset(dir, "18_lofi_keys.json",
                PB("Lo-Fi Keys", "FX").e("a.type", "fm").e("a.fm.algo", "4: 4>3 + 2>1").set("a.fm.index", 0.3f)
                    .set("a.fm.op1.decay", 2.5f).set("a.fm.op1.sustain", 0.0f).set("a.fm.op2.level", 0.5f).set("a.fm.op2.decay", 1.4f)
                    .set("a.fm.op2.sustain", 0.1f).set("a.fm.op4.ratio", 14).set("a.fm.op4.level", 0.25f).set("a.fm.op4.decay", 0.2f).set("a.fm.op4.sustain", 0)
                    .e("filter.type", "off").set("amp.sustain", 1.0f).set("amp.release", 0.4f)
                    .chain({"lofi", "crush", "comp", "reverb"})
                    .fx("lofi.wow", 0.45f).fx("lofi.flutter", 0.3f).fx("lofi.saturation", 0.4f).fx("lofi.hiss", 0.08f).fx("lofi.crackle", 0.2f)
                    .fx("lofi.bandwidth", 6000).fx("crush.bits", 11).fx("crush.rate", 24000).fx("crush.mix", 0.5f)
                    .fx("comp.thresh", -24).fx("comp.ratio", 3).fxe("reverb.type", "room").fx("reverb.mix", 0.2f)
                    .comment("Tape wow / flutter, vinyl crackle and a gentle bit crush on an FM e-piano"));

    writePreset(dir, "19_auto_wah_clav.json",
                PB("Auto-Wah Clav", "FX").e("a.va.wave1", "pulse").e("a.va.wave2", "pulse").set("a.va.pw", 0.2f).set("a.va.osc2semi", 12)
                    .set("a.va.mix", 0.3f).e("filter.type", "hp").set("filter.cutoff", 150).set("amp.decay", 0.5f).set("amp.sustain", 0.25f)
                    .set("amp.release", 0.08f)
                    .chain({"comp", "wah", "phaser", "reverb"})
                    .fx("comp.thresh", -26).fx("comp.ratio", 6).fx("comp.attack", 2)
                    .fxe("wah.mode", "auto").fx("wah.sens", 0.75f).fx("wah.reso", 0.7f)
                    .fxe("phaser.stages", "4").fx("phaser.mix", 0.3f).fx("phaser.rate", 0.3f)
                    .fxe("reverb.type", "room").fx("reverb.mix", 0.1f)
                    .comment("Envelope-following wah: play harder for a wider sweep"));

    writePreset(dir, "20_vocoder.json",
                PB("Vocoder Choir", "FX").e("a.va.wave1", "saw").e("a.va.wave2", "saw").set("a.va.detune", 10).set("a.va.noise", 0.08f)
                    .e("filter.type", "off").set("amp.sustain", 1.0f).set("amp.release", 0.3f)
                    .chain({"vocoder", "chorus", "reverb"})
                    .fx("vocoder.bands", 20).fx("vocoder.unvoiced", 0.3f).fx("chorus.mix", 0.35f).fx("reverb.mix", 0.15f)
                    .comment("Needs an audio input (microphone): hold chords and speak. Desktop: Audio > Input."));

    writePreset(dir, "21_jet_flanger.json",
                PB("Jet Flanger Strings", "FX").set("master.volume", 0.55f).e("a.va.wave1", "saw").e("a.va.wave2", "saw").set("a.va.detune", 6)
                    .set("filter.cutoff", 6000).set("filter.envamt", 0.0f).set("amp.attack", 0.05f).set("amp.release", 0.9f).set("amp.sustain", 1.0f)
                    .chain({"flanger", "phaser", "delay", "reverb"})
                    .fx("flanger.rate", 0.12f).fx("flanger.feedback", 0.75f).fx("flanger.depth", 0.9f).fx("flanger.thruzero", 1)
                    .fxe("phaser.stages", "8").fx("phaser.mix", 0.25f).fx("phaser.rate", 0.07f)
                    .fxe("delay.type", "tape").fx("delay.time", 0.42f).fx("delay.mix", 0.18f)
                    .fxe("reverb.type", "hall").fx("reverb.mix", 0.2f)
                    .comment("Thru-zero flanger sweep layered with a slow 8-stage phaser"));

    writePreset(dir, "22_harmonizer_stutter.json",
                PB("Harmonizer Stutter Lead", "FX").e("a.va.wave1", "square").e("a.va.wave2", "saw").set("a.va.mix", 0.4f)
                    .set("filter.cutoff", 4000).set("amp.release", 0.2f).set("fx.tempo", 124)
                    .chain({"pitch", "stutter", "delay", "reverb"})
                    .fx("pitch.semi1", 7).fx("pitch.pan1", -0.5f).fx("pitch.semi2", 12).fx("pitch.level2", 0.6f).fx("pitch.pan2", 0.5f)
                    .fx("pitch.mix", 0.45f).fxe("stutter.division", "1/16")
                    .fxe("delay.mode", "pingpong").fxe("delay.sync", "1/4").fx("delay.mix", 0.2f).fx("reverb.mix", 0.15f)
                    .fxmod(1, "modwheel", "fx.stutter.active", 1.0f)
                    .comment("Fifth + octave harmonizer. Push the mod wheel to beat-repeat 1/16 slices."));

    // ---- wavetable layer
    writePreset(dir, "23_wt_morph_pad.json",
                PB("WT Morph Pad", "WT").set("master.volume", 0.55f).e("a.type", "wt").e("a.wt.table", "basic").set("a.wt.unison", 5).set("a.wt.detune", 14)
                    .set("a.wt.spread", 0.8f).set("a.wt.posenv", 0.6f).set("filter.cutoff", 3500).set("filter.envamt", 0.1f)
                    .set("env2.attack", 2.5f).set("env2.sustain", 0.5f).set("amp.attack", 0.6f).set("amp.release", 1.8f).set("amp.sustain", 1.0f)
                    .mod(4, "lfo2", "wt.pos", 0.15f).set("lfo2.rate", 0.2f)
                    .chain({"chorus", "reverb"}).fx("chorus.mix", 0.35f).fxe("reverb.type", "hall").fx("reverb.mix", 0.3f).fx("reverb.decay", 4.5f)
                    .comment("5-voice unison wavetable: Env2 sweeps sine > saw > square, LFO2 keeps it moving"));
    writePreset(dir, "24_wt_formant_lead.json",
                PB("WT Formant Lead", "WT").e("a.type", "wt").e("a.wt.table", "formant").set("a.wt.pos", 0.1f).set("a.wt.unison", 2)
                    .set("a.wt.detune", 8).e("filter.type", "lp12").set("filter.cutoff", 6000).set("amp.release", 0.25f)
                    .set("master.bendrange", 12).mod(4, "modwheel", "wt.pos", 0.8f).mod(5, "notetimbre", "wt.pos", 0.5f)
                    .chain({"drive", "delay", "reverb"}).fxe("drive.type", "tube").fx("drive.amount", 0.2f)
                    .fxe("delay.sync", "1/8.").fx("delay.mix", 0.2f).fx("reverb.mix", 0.12f)
                    .comment("Mod wheel / per-note Y morph the vowel a-e-i-o-u"));
    writePreset(dir, "25_wt_sync_bass.json",
                PB("WT Sync Bass", "WT").e("a.type", "wt").e("a.wt.table", "digital").set("a.wt.pos", 0.3f).e("a.wt.warp", "sync")
                    .set("a.wt.warpamt", 0.0f).mod(4, "env2", "wt.warp", 0.6f).set("env2.decay", 0.35f).set("env2.sustain", 0.0f)
                    .set("filter.cutoff", 1800).set("filter.reso", 0.3f).set("filter.envamt", 0.35f).set("amp.release", 0.1f)
                    .chain({"comp", "drive"}).fx("comp.thresh", -20).fxe("drive.type", "soft").fx("drive.amount", 0.3f)
                    .comment("Envelope-swept hard-sync warp on the digital table"));

    // ---- v0.4: filters, voice FX, voice modes, 6-op FM, part inserts
    writePreset(dir, "26_ladder_bass.json",
                PB("Ladder Mono Bass", "Bass").e("a.va.wave1", "saw").e("a.va.wave2", "square").set("a.va.osc2semi", -12)
                    .set("a.va.mix", 0.4f).e("filter.type", "ladder").set("filter.cutoff", 350).set("filter.reso", 0.55f)
                    .set("filter.drive", 0.35f).set("filter.envamt", 0.45f).set("filter.keytrack", 0.3f).set("env2.decay", 0.25f)
                    .set("env2.sustain", 0.0f).set("amp.release", 0.08f).e("voice.mode", "legato").set("voice.glide", 0.06f)
                    .e("voice.glidemode", "legato").mod(1, "none", "none", 0).mod(4, "velocity", "cutoff", 0.25f)
                    .set("reverb.mix", 0.0f)
                    .comment("4-pole ladder with drive; legato notes glide, detached notes retrigger nothing but the pitch"));
    writePreset(dir, "27_supersaw.json",
                PB("Supersaw Unison", "VA").set("master.volume", 0.5f).e("a.va.wave1", "saw").e("a.va.wave2", "saw")
                    .set("a.va.detune", 11).set("voice.unison", 5).set("voice.unidetune", 38).set("voice.unispread", 1.0f)
                    .set("filter.cutoff", 6000).set("filter.envamt", 0.05f).set("amp.attack", 0.01f).set("amp.release", 0.6f)
                    .set("amp.sustain", 1.0f).mod(4, "modwheel", "unison.detune", 1.0f)
                    .chain({"chorus", "delay", "reverb"}).fx("chorus.mix", 0.25f).fxe("delay.sync", "1/8.").fx("delay.mix", 0.18f)
                    .fxe("reverb.type", "hall").fx("reverb.mix", 0.25f)
                    .comment("5 voices per note x 2 oscillators. Mod wheel widens the unison detune"));
    writePreset(dir, "28_formant_choir.json",
                PB("Formant Choir", "VA").e("a.va.wave1", "saw").e("a.va.wave2", "pulse").set("a.va.detune", 8).set("a.va.noise", 0.04f)
                    .e("filter.type", "formant").set("filter.cutoff", 1000).set("filter.reso", 0.7f).set("filter.keytrack", 0.25f)
                    .set("filter.envamt", 0.0f).set("filter.velamt", 0.0f).set("filter.morph", 0.1f)
                    .set("amp.attack", 0.5f).set("amp.release", 1.2f).set("amp.sustain", 1.0f)
                    .set("lfo3.rate", 0.15f).mod(4, "lfo3", "filter.morph", 0.25f).mod(5, "modwheel", "filter.morph", 0.8f)
                    .set("voice.unison", 3).set("voice.unidetune", 18).set("voice.unispread", 0.7f)
                    .chain({"chorus", "reverb"}).fxe("chorus.mode", "ensemble").fx("chorus.mix", 0.4f).fxe("reverb.type", "hall")
                    .fx("reverb.mix", 0.35f).fx("reverb.decay", 4.0f)
                    .comment("Vowel filter a-e-i-o-u: a per-voice LFO3 drifts the vowel, the mod wheel sweeps it"));
    writePreset(dir, "29_comb_pluck.json",
                PB("Comb Pluck", "VA").e("a.va.wave1", "saw").set("a.va.mix", 0.0f).set("a.va.noise", 0.6f)
                    .e("filter.type", "comb+").set("filter.cutoff", 261.6f).set("filter.keytrack", 1.0f).set("filter.reso", 0.93f)
                    .set("filter.envamt", 0.0f).set("filter.velamt", 0.0f).set("amp.attack", 0.001f).set("amp.decay", 1.5f)
                    .set("amp.sustain", 0.0f).set("amp.release", 0.6f).mod(1, "none", "none", 0)
                    .mod(4, "env3", "a.level", -1.0f).set("env3.attack", 0.0005f).set("env3.decay", 0.02f).set("env3.sustain", 1.0f)
                    .mod(5, "velocity", "resonance", 0.06f).chain({"reverb"}).fxe("reverb.type", "room").fx("reverb.mix", 0.2f)
                    .comment("Key-tracked comb filter excited by a short noise burst (Env3 gates the noise): plucked string"));
    writePreset(dir, "30_fm6_epiano.json",
                PB("FM6 Tine E.Piano", "FM").e("a.type", "fm").e("a.fm.algo", "11: 6op 6>5 + 4>3 + 2>1").set("a.fm.index", 0.35f)
                    .set("a.fm.op1.ratio", 1).set("a.fm.op1.decay", 3.5f).set("a.fm.op1.sustain", 0.0f).set("a.fm.op1.release", 0.4f)
                    .set("a.fm.op2.ratio", 1).set("a.fm.op2.level", 0.5f).set("a.fm.op2.decay", 1.8f).set("a.fm.op2.sustain", 0.05f)
                    .set("a.fm.op2.velsens", 0.9f)
                    .set("a.fm.op3.ratio", 1).set("a.fm.op3.level", 0.7f).set("a.fm.op3.decay", 2.5f).set("a.fm.op3.sustain", 0.0f)
                    .set("a.fm.op4.ratio", 14).set("a.fm.op4.level", 0.3f).set("a.fm.op4.decay", 0.2f).set("a.fm.op4.sustain", 0.0f)
                    .set("a.fm.op4.velsens", 1.0f)
                    .set("a.fm.op5.ratio", 1).set("a.fm.op5.detune", 4).set("a.fm.op5.level", 0.5f).set("a.fm.op5.decay", 3.0f)
                    .set("a.fm.op5.sustain", 0.0f).set("a.fm.op6.ratio", 1).set("a.fm.op6.level", 0.35f).set("a.fm.op6.decay", 1.0f)
                    .set("a.fm.op6.sustain", 0.1f).e("filter.type", "off").set("amp.sustain", 1.0f).set("amp.release", 0.4f)
                    .mod(4, "notepressure", "fm.op4.level", 1.0f).mod(5, "lfo2", "pan", 0.25f).set("lfo2.rate", 3.5f)
                    .chain({"chorus", "reverb"}).fx("chorus.mix", 0.35f).fx("reverb.mix", 0.2f)
                    .comment("Three operator pairs (6-op algorithm): body, tine (ratio 14) and a detuned layer. Pressure adds tine"));
    writePreset(dir, "31_fm6_brass.json",
                PB("FM6 Brass", "FM").e("a.type", "fm").e("a.fm.algo", "14: 6op 6>(5,4,3) + 2>1").set("a.fm.index", 0.45f)
                    .set("a.fm.feedback", 0.3f).set("a.fm.op1.sustain", 0.9f).set("a.fm.op2.level", 0.6f).set("a.fm.op2.attack", 0.08f)
                    .set("a.fm.op2.sustain", 0.6f).set("a.fm.op3.sustain", 0.9f).set("a.fm.op3.detune", -5).set("a.fm.op4.ratio", 1)
                    .set("a.fm.op4.detune", 5).set("a.fm.op4.sustain", 0.9f).set("a.fm.op5.sustain", 0.9f).set("a.fm.op5.level", 0.5f)
                    .set("a.fm.op6.level", 0.55f).set("a.fm.op6.attack", 0.1f).set("a.fm.op6.sustain", 0.5f)
                    .e("filter.type", "lp12").set("filter.cutoff", 5000).set("amp.attack", 0.04f).set("amp.release", 0.25f)
                    .mod(4, "velocity", "amp.attack", -0.5f).mod(5, "modwheel", "fm.op6.level", 0.8f)
                    .chain({"comp", "reverb"}).fx("comp.thresh", -18).fx("reverb.mix", 0.2f)
                    .comment("Op 6 drives three carriers; harder playing = faster attack, mod wheel = brassier"));
    writePreset(dir, "32_ring_lead.json",
                PB("Ring Mod Lead", "VA").e("a.va.wave1", "saw").e("a.va.wave2", "sine").set("a.va.mix", 0.3f)
                    .e("vfx.type", "ring").set("vfx.amount", 0.0f).set("vfx.ratio", 1.5f).set("filter.cutoff", 4500)
                    .set("amp.release", 0.2f).set("master.bendrange", 12).e("voice.mode", "legato").set("voice.glide", 0.08f)
                    .mod(4, "notepressure", "vfx.amount", 1.0f).mod(5, "modwheel", "vfx.amount", 0.7f)
                    .chain({"delay", "reverb"}).fxe("delay.sync", "1/8.").fx("delay.mix", 0.2f).fx("reverb.mix", 0.15f)
                    .comment("Per-voice ring modulator at 1.5x the note: pressure / mod wheel bring it in"));
    writePreset(dir, "33_pressure_drive.json",
                PB("Pressure Drive Keys", "Expressive").e("a.va.wave1", "triangle").e("a.va.wave2", "square").set("a.va.mix", 0.25f)
                    .e("vfx.type", "saturate").set("vfx.amount", 0.0f).e("filter.type", "lp12").set("filter.cutoff", 2500)
                    .set("amp.decay", 1.2f).set("amp.sustain", 0.5f).set("amp.release", 0.3f)
                    .mod(4, "notepressure", "vfx.amount", 1.0f).mod(5, "notepressure", "cutoff", 0.4f)
                    .mod(6, "notetimbre", "vfx.mix", -0.6f)
                    .chain({"chorus", "reverb"}).fx("chorus.mix", 0.25f).fx("reverb.mix", 0.18f)
                    .comment("Each key saturates on its own pressure (voice FX), so chords stay clean except the pressed note"));
    writePreset(dir, "34_insert_strip.json",
                PB("Part Insert Strip", "FX").e("a.va.wave1", "pulse").e("a.va.wave2", "saw").set("a.va.detune", 7)
                    .set("filter.cutoff", 2800).set("amp.sustain", 0.8f).set("amp.release", 0.4f)
                    .e("ins.slot1", "comp").set("ins.comp.thresh", -20).set("ins.comp.ratio", 4)
                    .e("ins.slot2", "phaser").set("ins.phaser.mix", 0.4f).set("ins.phaser.rate", 0.25f)
                    .e("ins.slot3", "stereo").set("ins.stereo.width", 1.5f)
                    .chain({"reverb"}).fx("reverb.mix", 0.2f)
                    .comment("Part inserts (comp > phaser > stereo width) travel with the patch, before the shared rack"));
    writePreset(dir, "35_sync_wobble.json",
                PB("Tempo Wobble Bass", "Bass").e("a.va.wave1", "saw").e("a.va.wave2", "square").set("a.va.osc2semi", -12)
                    .e("filter.type", "ladder").set("filter.cutoff", 300).set("filter.reso", 0.5f).set("filter.envamt", 0.0f)
                    .set("amp.release", 0.1f).set("fx.tempo", 128).e("lfo3.sync", "1/8T").e("lfo3.wave", "triangle")
                    .mod(4, "lfo3", "cutoff", 0.45f, "modwheel").e("voice.mode", "mono").mod(1, "none", "none", 0)
                    .set("fx.clocksync", 1)
                    .chain({"drive", "comp"}).fxe("drive.type", "fuzz").fx("drive.amount", 0.3f).fx("drive.mix", 0.5f)
                    .comment("LFO3 synced to 1/8T of the tempo (follows MIDI clock); the mod wheel sets the wobble depth"));
}

// A GM-style multi: piano, bass, strings, lead and drums on their usual channels.
void multis(const std::string& presetDir, const std::string& dir) {
    fs::create_directories(dir);
    MultiPreset m;
    m.name = "Demo Band (multi)";
    m.comment = "Ch1 piano, ch2 bass, ch3 strings, ch4 lead, ch10 drums. Load as a multi (multi-timbral mode).";
    auto load = [&](int part, const char* file) {
        std::string err;
        if (!loadPresetFile(presetDir + "/" + file, m.parts[part], err)) { std::fprintf(stderr, "%s\n", err.c_str()); std::exit(1); }
    };
    load(0, "07_glass_piano.json");
    load(1, "06_fm_bass.json");
    load(2, "14_ensemble_strings.json");
    load(3, "11_expressive_lead.json");
    load(9, "10_drum_kit.json");
    // global rack: plate reverb on the send, master limiter
    PB rack("rack", "");
    rack.chain({"reverb"}).fxe("reverb.type", "plate").fx("reverb.mix", 0.18f);
    m.global = rack.p;
    m.mix[0][Mix::Volume] = 0.8f;
    m.mix[1][Mix::Volume] = 0.85f;
    m.mix[1][Mix::FxSend] = 0.2f;
    m.mix[2][Mix::Volume] = 0.6f;
    m.mix[2][Mix::Pan] = -0.3f;
    m.mix[3][Mix::Volume] = 0.7f;
    m.mix[3][Mix::Pan] = 0.3f;
    m.mix[9][Mix::Volume] = 0.85f;
    m.mix[9][Mix::FxSend] = 0.3f;
    std::string err;
    if (!saveMultiFile(dir + "/01_demo_band.json", m, err)) { std::fprintf(stderr, "%s\n", err.c_str()); std::exit(1); }
    std::printf("  %s/01_demo_band.json\n", dir.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: hsynth_assets <output-dir>\n");
        return 2;
    }
    const std::string out = argv[1];
    const std::string lib = out + "/library/factory";
    std::printf("Generating demo assets into %s\n", out.c_str());
    testWaves(lib + "/test_waves");
    glassPiano(lib + "/glass_piano");
    loopPad(lib + "/loop_pad");
    drumKit(lib + "/drum_kit");
    presets(out + "/presets");
    multis(out + "/presets", out + "/multis");
    writeText(lib + "/README.txt",
              "All audio in this folder was generated by tools/hsynth_assets.cpp.\n"
              "No third-party recordings are included. Released as CC0.\n");
    return 0;
}
