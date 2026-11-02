#include "sampler_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "hsynth/fileio.h"
#include "hsynth/fx/convolver.h"
#include "hsynth/fx/dsp.h"
#include "hsynth/params.h"
#include "hsynth/preset.h"
#include "hsynth/wav.h"

namespace fs = std::filesystem;

namespace sampler {

namespace {

// normalised squared error of the waveform match at the loop point
constexpr double kMaxLoopError = 0.25;
// crossfade length used when the waveform match is not clean
constexpr double kLongCrossfadeSec = 0.3;

struct Take {
    int note = 60, velocity = 100;
    Recording rec;
    bool stereo = true;
    int64_t loopStart = -1, loopEnd = -1;  // exclusive end
    float peak = 0;
};

float peakOf(const Recording& r) {
    float p = 0;
    for (float v : r.left) p = std::max(p, std::fabs(v));
    for (float v : r.right) p = std::max(p, std::fabs(v));
    return p;
}

double rms(const std::vector<float>& x, size_t a, size_t b) {
    b = std::min(b, x.size());
    if (a >= b) return 0;
    double s = 0;
    for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / (double)(b - a));
}

// Remove lead-in silence (keeping a few samples before the onset) and the
// silent tail, then apply a short fade-out.
void trim(Recording& r, double silenceDb, int sampleRate) {
    const float pk = peakOf(r);
    if (pk <= 0) return;
    const float thr = std::max(1e-6f, pk * (float)std::pow(10.0, silenceDb / 20.0));
    const float onsetThr = std::max(1e-6f, pk * 0.003f);  // -50 dB: robust against noise before the note
    const size_t n = r.left.size();
    auto amp = [&](size_t i) { return std::max(std::fabs(r.left[i]), r.right.empty() ? 0.0f : std::fabs(r.right[i])); };
    size_t start = 0;
    while (start < n && amp(start) < onsetThr) ++start;
    start = start > 16 ? start - 16 : 0;
    size_t end = n;
    while (end > start && amp(end - 1) < thr) --end;
    const size_t fade = std::min<size_t>((size_t)(0.02 * sampleRate), end - start);
    end = std::min(n, end + fade);
    auto cut = [&](std::vector<float>& v) {
        if (v.empty()) return;
        v = std::vector<float>(v.begin() + (std::ptrdiff_t)start, v.begin() + (std::ptrdiff_t)end);
        for (size_t i = 0; i < fade && i < v.size(); ++i) v[v.size() - 1 - i] *= (float)i / (float)fade;
        // 1 ms fade-in removes any click from cutting into the noise floor
        const size_t fin = std::min<size_t>((size_t)(0.001 * sampleRate), v.size());
        for (size_t i = 0; i < fin; ++i) v[i] *= (float)i / (float)fin;
    };
    cut(r.left);
    cut(r.right);
}

bool isMono(const Recording& r) {
    if (r.right.empty()) return true;
    double diff = 0, sum = 0;
    for (size_t i = 0; i < r.left.size(); ++i) {
        const double d = (double)r.left[i] - r.right[i];
        diff += d * d;
        sum += (double)r.left[i] * r.left[i];
    }
    return diff <= sum * 1e-6;  // identical channels (centre-panned mono source)
}

// Find a loop inside the sustain part: compare the waveform around candidate
// start/end points and choose the best match, then crossfade the region in
// front of the loop end with the region in front of the loop start so the
// jump is seamless. Returns false when no acceptable loop exists.
bool findLoop(Take& t, double holdSec, int sr) {
    const auto& L = t.rec.left;
    const auto& R = t.rec.right.empty() ? t.rec.left : t.rec.right;
    const size_t n = L.size();
    const size_t holdEnd = std::min(n, (size_t)(holdSec * sr));
    if (holdEnd < (size_t)sr) return false;
    // sustain check: level late in the hold vs earlier
    const double early = rms(L, (size_t)(0.35 * holdEnd), (size_t)(0.5 * holdEnd));
    const double late = rms(L, (size_t)(0.75 * holdEnd), (size_t)(0.9 * holdEnd));
    if (std::getenv("HSYNTH_SAMPLER_DEBUG")) std::printf("    sustain n%d v%d: late/early %.2f\n", t.note, t.velocity, early > 0 ? late / early : 0.0);
    if (early <= 0 || late / early < 0.55) return false;

    const int half = 384;
    const size_t maxXf = (size_t)(kLongCrossfadeSec * sr);
    const size_t endCenter = (size_t)(0.88 * holdEnd);
    const size_t startLo = std::max<size_t>(maxXf + (size_t)half, (size_t)(0.40 * holdEnd));
    const size_t startHi = (size_t)(0.60 * holdEnd);
    auto cost = [&](size_t s, size_t e) {
        double c = 0;
        for (int i = -half; i < half; i += 2) {
            const double dl = (double)L[s + i] - L[e + i];
            const double dr = (double)R[s + i] - R[e + i];
            c += dl * dl + dr * dr;
        }
        return c;
    };
    double best = 1e300;
    size_t bestS = 0, bestE = 0;
    // end candidates: rising zero crossings near endCenter
    for (size_t e = endCenter - (size_t)(0.01 * sr); e < endCenter + (size_t)(0.01 * sr) && e + half < n; ++e) {
        if (!(L[e - 1] < 0 && L[e] >= 0)) continue;
        for (size_t s = startLo; s < startHi; ++s) {
            if (!(L[s - 1] < 0 && L[s] >= 0)) continue;
            const double c = cost(s, e);
            if (c < best) { best = c; bestS = s; bestE = e; }
        }
    }
    if (bestE == 0) return false;
    // match quality relative to the signal energy in the window
    double energy = 0;
    for (int i = -half; i < half; i += 2) energy += (double)L[bestE + i] * L[bestE + i] + (double)R[bestE + i] * R[bestE + i];
    if (energy <= 0) return false;
    const double err = best / energy;
    if (std::getenv("HSYNTH_SAMPLER_DEBUG")) std::printf("    loop n%d v%d: match error %.3f\n", t.note, t.velocity, err);
    // Periodic sounds (organ, flute, brass) splice cleanly with a short linear
    // crossfade. Ensembles / choirs / pads are not periodic: hide the seam with
    // a long equal-power crossfade instead (signals are mostly uncorrelated).
    const bool clean = err <= kMaxLoopError;
    const size_t xf = clean ? (size_t)(0.04 * sr) : maxXf;
    auto blend = [&](std::vector<float>& v) {
        if (v.empty()) return;
        for (size_t i = 0; i < xf; ++i) {
            const double a = (double)i / (double)xf;
            const double gIn = clean ? a : std::sin(a * 1.5707963), gOut = clean ? 1.0 - a : std::cos(a * 1.5707963);
            const size_t de = bestE - xf + i, ds = bestS - xf + i;
            v[de] = (float)(v[de] * gOut + v[ds] * gIn);
        }
    };
    blend(t.rec.left);
    blend(t.rec.right);
    t.loopStart = (int64_t)bestS;
    t.loopEnd = (int64_t)bestE;
    return true;
}

std::string noteName(int n) {
    static const char* names[12] = {"c", "cs", "d", "ds", "e", "f", "fs", "g", "gs", "a", "as", "b"};
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

// Halve the sample rate with the engine's linear-phase half-band FIR.
void decimate2(std::vector<float>& x) {
    if (x.empty()) return;
    hsynth::fx::HalfBand hb;
    hb.design();
    std::vector<float> y(x.size() / 2);
    for (size_t k = 0; k < y.size(); ++k) y[k] = hb.down(x[2 * k], x[2 * k + 1]);
    x.swap(y);
}

// Level of the 50 ms window ending at i (linear RMS, both channels).
double windowRms(const Recording& r, size_t end, size_t len) {
    const size_t a = end > len ? end - len : 0;
    double s = 0;
    for (size_t i = a; i < end && i < r.left.size(); ++i) {
        s += (double)r.left[i] * r.left[i];
        if (!r.right.empty()) s += (double)r.right[i] * r.right[i];
    }
    return std::sqrt(s / (double)std::max<size_t>(1, end - a) / (r.right.empty() ? 1.0 : 2.0));
}

size_t onsetOf(const Recording& r, float thr) {
    for (size_t i = 0; i < r.left.size(); ++i)
        if (std::fabs(r.left[i]) > thr || (!r.right.empty() && std::fabs(r.right[i]) > thr)) return i;
    return r.left.size();
}

// Third-octave band levels (dB, mean removed) of ~170 ms after the onset:
// a level-independent fingerprint of the timbre.
std::vector<double> spectrumShape(const Recording& r, size_t onset, int sr) {
    const int n = 8192;
    std::vector<float> re((size_t)n, 0.0f), im((size_t)n, 0.0f);
    const size_t start = onset + (size_t)(0.01 * sr);
    for (int i = 0; i < n && start + (size_t)i < r.left.size(); ++i) {
        const float w = 0.5f - 0.5f * std::cos(6.2831853f * (float)i / (float)(n - 1));
        const float v = r.right.empty() ? r.left[start + (size_t)i] : 0.5f * (r.left[start + (size_t)i] + r.right[start + (size_t)i]);
        re[(size_t)i] = v * w;
    }
    hsynth::fx::Fft fft;
    fft.init(n);
    fft.forward(re.data(), im.data());
    std::vector<double> bands;
    const double top = std::min(16000.0, sr / 2.2);
    for (double f = 100.0; f * 1.26 < top; f *= 1.26) {
        const int a = (int)(f / sr * n), b = std::max(a + 1, (int)(f * 1.26 / sr * n));
        double e = 0;
        for (int k = a; k < b; ++k) e += (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
        bands.push_back(10.0 * std::log10(e + 1e-20));
    }
    double mean = 0;
    for (double v : bands) mean += v;
    mean /= std::max<size_t>(1, bands.size());
    for (double& v : bands) v -= mean;
    return bands;
}

double holdForNote(const InstrumentSpec& spec, int note) {
    if (!spec.keyScaledHold) return spec.holdSec;
    // low notes ring longer: double the hold every two octaves down
    return std::clamp(spec.holdSec * std::pow(2.0, (60 - note) / 24.0), 0.4, 15.0);
}

}  // namespace

bool probeInstrument(Source& src, InstrumentSpec& spec, ProbeReport& rep, std::string& error) {
    const int sr = src.sampleRate();
    if (!src.selectProgram(spec.bank, spec.program, spec.drum, error)) return false;
    const float floorPeak = std::max(src.noisePeak() * 4.0f, 3e-5f);

    // 1. find an audible note (sustain / decay / release behaviour)
    std::vector<int> candidates;
    if (spec.drum) {
        for (int k : {38, 36, 40, 42}) if (std::find(spec.keys.begin(), spec.keys.end(), k) != spec.keys.end()) candidates.push_back(k);
        if (!spec.keys.empty()) candidates.push_back(spec.keys[spec.keys.size() / 2]);
    } else {
        candidates = {60, 48, 72, 36, 84, 96, 24};
    }
    Recording r;
    const double probeHold = spec.drum ? 0.2 : 2.5;
    rep = ProbeReport{};
    rep.silent = true;
    for (int note : candidates) {
        NoteRequest q;
        q.note = note;
        q.velocity = 100;
        q.holdSec = probeHold;
        q.maxReleaseSec = 6.0;
        if (!src.record(q, r, error)) return false;
        if (peakOf(r) > floorPeak) { rep.silent = false; rep.probeNote = note; break; }
    }
    if (rep.silent) { rep.summary = "silent"; return true; }

    const float pk = peakOf(r);
    const size_t onset = onsetOf(r, std::max(floorPeak, pk * 0.003f));
    const size_t win = (size_t)(0.05 * sr);
    const size_t holdEnd = std::min(r.left.size(), onset + (size_t)(probeHold * sr));
    auto dbAt = [&](size_t i) { return 20.0 * std::log10(windowRms(r, i, win) + 1e-12); };
    const double peakDb = 20.0 * std::log10(pk + 1e-12);
    if (!spec.drum) {
        const double early = windowRms(r, onset + (size_t)(0.5 * probeHold * sr), win);
        const double late = windowRms(r, onset + (size_t)(0.9 * probeHold * sr), win);
        rep.sustained = early > 0 && late / early > 0.5;
        // decay: extrapolate the level slope between 0.3 s and the end of the hold to -50 dB
        const double d0 = dbAt(onset + (size_t)(0.3 * sr)), d1 = dbAt(holdEnd);
        const double slope = (d0 - d1) / std::max(0.1, probeHold - 0.3);  // dB per second (> 0 = falling)
        const double below = peakDb - 3.0 - d0;                             // rms vs peak offset
        rep.decaySec = slope > 0.5 ? 0.3 + std::max(0.0, 50.0 - below) / slope : 30.0;
    }
    // release: time after note-off until the level is 50 dB under the level at note-off
    {
        const double atOff = dbAt(holdEnd);
        size_t i = holdEnd;
        while (i + win < r.left.size() && dbAt(i + win) > atOff - 50.0) i += win / 2;
        rep.releaseSec = (double)(i - holdEnd) / sr;
    }

    // 2. velocity: level and timbre of a soft and a hard note
    Recording soft, hard;
    for (int pass = 0; pass < 3; ++pass) {
        NoteRequest q;
        q.note = rep.probeNote;
        q.holdSec = spec.drum ? 0.2 : 0.6;
        q.maxReleaseSec = 0.6;
        q.velocity = 127;
        if (!src.record(q, hard, error)) return false;
        if (!hard.clipped) break;
        if (!src.reduceLevel()) break;  // clips at the lowest level: keep, warn below
    }
    {
        NoteRequest q;
        q.note = rep.probeNote;
        q.holdSec = spec.drum ? 0.2 : 0.6;
        q.maxReleaseSec = 0.6;
        q.velocity = 32;
        if (!src.record(q, soft, error)) return false;
    }
    const float ph = peakOf(hard), ps = peakOf(soft);
    rep.levelDb = 20.0 * std::log10((ph + 1e-9) / (ps + 1e-9));
    if (ps > floorPeak && ph > floorPeak) {
        const auto a = spectrumShape(soft, onsetOf(soft, ps * 0.01f), sr);
        const auto b = spectrumShape(hard, onsetOf(hard, ph * 0.01f), sr);
        double d = 0;
        for (size_t k = 0; k < a.size(); ++k) d += (a[k] - b[k]) * (a[k] - b[k]);
        rep.timbreDb = std::sqrt(d / std::max<size_t>(1, a.size()));
    }

    // 3. plan
    if (rep.timbreDb < 1.5) spec.velocities = {100};
    else if (rep.timbreDb < 4.0) spec.velocities = {64, 120};
    else spec.velocities = {40, 85, 125};
    // SFZ amp_veltrack 100 % = -24 dB at velocity 32. With several recorded
    // layers most of the dynamics are in the samples already.
    const double vt = std::clamp(rep.levelDb / 24.0 * 100.0, 0.0, 100.0);
    spec.ampVeltrack = (int)std::lround(spec.velocities.size() == 1 ? vt : vt * 0.3);
    if (spec.drum) {
        spec.holdSec = 0.2;
        spec.maxReleaseSec = 8.0;
        spec.oneShot = true;
        spec.loop = InstrumentSpec::Loop::Off;
    } else if (rep.sustained) {
        spec.holdSec = 3.0;
        spec.keyScaledHold = false;
        spec.stopBelowDb = 0;
        spec.loop = InstrumentSpec::Loop::Auto;
        spec.maxReleaseSec = std::clamp(rep.releaseSec * 1.3 + 0.3, 0.4, 8.0);
        spec.presetRelease = (float)std::clamp(rep.releaseSec * 0.3, 0.02, 3.0);
    } else {
        spec.holdSec = std::clamp(rep.decaySec * 1.2, 0.6, 12.0);
        spec.keyScaledHold = true;
        spec.stopBelowDb = 55.0;
        spec.loop = InstrumentSpec::Loop::Off;
        spec.maxReleaseSec = std::clamp(rep.releaseSec * 1.3 + 0.3, 0.4, 6.0);
        spec.presetRelease = (float)std::clamp(rep.releaseSec * 0.3, 0.02, 2.0);
    }
    char b[256];
    std::snprintf(b, sizeof b, "%s, probe %s, decay %.1fs, release %.1fs, vel: level %.1f dB timbre %.1f dB -> %zu layer(s)%s",
                  spec.drum ? "kit" : rep.sustained ? "sustained" : "decaying", noteName(rep.probeNote).c_str(), rep.decaySec,
                  rep.releaseSec, rep.levelDb, rep.timbreDb, spec.velocities.size(), hard.clipped ? ", CLIPS at lowest level" : "");
    rep.summary = b;
    return true;
}

bool sampleInstrument(Source& src, const InstrumentSpec& spec, const Options& opt, std::string& error, InstrumentResult* result) {
    const int captureRate = src.sampleRate();
    const int dec = opt.decimate == 2 ? 2 : 1;
    const int sr = captureRate / dec;

    std::vector<int> notes = spec.keys;
    if (notes.empty())
        for (int n = spec.lowNote; n <= spec.highNote; n += spec.step) notes.push_back(n);
    std::vector<int> vels = spec.velocities;
    std::sort(vels.begin(), vels.end());

    const fs::path dir = hsynth::utf8Path(opt.outDir) / hsynth::utf8Path(spec.name);
    const fs::path sdir = dir / "samples";
    const std::string leaf = hsynth::utf8String(dir.filename());  // spec.name may contain sub folders

    std::vector<Take> takes;
    int loops = 0, clipped = 0;
    for (int attempt = 0;; ++attempt) {
        if (!src.selectProgram(spec.bank, spec.program, spec.drum, error)) return false;
        takes.clear();
        loops = clipped = 0;
        bool restart = false;
        for (size_t ni = 0; ni < notes.size() && !restart; ++ni) {
            if (opt.stopRequested && opt.stopRequested()) {
                if (opt.verbose) std::printf("\n");
                error = kStoppedError;
                return false;
            }
            const int note = notes[ni];
            for (int vel : vels) {
                Take t;
                t.note = note;
                t.velocity = vel;
                NoteRequest q;
                q.note = note;
                q.velocity = vel;
                q.holdSec = holdForNote(spec, note);
                q.maxReleaseSec = spec.maxReleaseSec;
                q.stopBelowDb = spec.stopBelowDb;
                if (!src.record(q, t.rec, error)) return false;
                if (t.rec.clipped) {
                    // level must stay the same for the whole instrument: start over quieter
                    if (attempt < 2 && src.reduceLevel()) {
                        if (opt.verbose) std::printf("\n    %s v%d clipped: restarting %s 6 dB lower\n", noteName(note).c_str(), vel, spec.name.c_str());
                        restart = true;
                        break;
                    }
                    ++clipped;
                }
                if (dec == 2) {
                    decimate2(t.rec.left);
                    decimate2(t.rec.right);
                }
                trim(t.rec, opt.silenceDb, sr);
                t.peak = peakOf(t.rec);
                if (t.peak < std::max(src.noisePeak() * 4.0f, 1e-5f) || t.rec.left.size() < 64)
                    continue;  // silent note (outside the patch's key range)
                t.stereo = opt.keepStereo && !isMono(t.rec);
                if (!t.stereo) t.rec.right.clear();
                const bool tryLoop = !spec.oneShot && spec.loop != InstrumentSpec::Loop::Off;
                if (tryLoop && findLoop(t, q.holdSec, sr)) ++loops;
                else if (spec.loop == InstrumentSpec::Loop::On && opt.verbose)
                    std::printf("    %s v%d: no clean loop found, plays without loop\n", noteName(note).c_str(), vel);
                takes.push_back(std::move(t));
            }
            if (opt.verbose && !restart) {
                std::printf("\r  %-28s %3zu/%zu keys, %zu samples", spec.name.c_str(), ni + 1, notes.size(), takes.size());
                std::fflush(stdout);
            }
        }
        if (!restart) break;
    }
    if (opt.verbose) std::printf("\n");
    if (takes.empty()) { error = "no audio recorded for " + spec.name; return false; }
    fs::create_directories(sdir);

    // one gain for the whole instrument keeps the velocity layers' dynamics
    float maxPeak = 0;
    for (auto& t : takes) maxPeak = std::max(maxPeak, t.peak);
    const float gain = (float)std::pow(10.0, opt.targetPeakDb / 20.0) / maxPeak;

    std::string sfz;
    sfz += "// " + spec.title + " - sampled by hsynth_sampler (test use only, do not distribute)\n";
    sfz += "<control> default_path=samples/\n";
    sfz += "<global> amp_veltrack=" + std::to_string(spec.ampVeltrack) + (spec.oneShot ? " loop_mode=one_shot" : "") + "\n";
    size_t bytes = 0;
    double seconds = 0;
    for (size_t k = 0; k < takes.size(); ++k) {
        Take& t = takes[k];
        for (auto& v : t.rec.left) v *= gain;
        for (auto& v : t.rec.right) v *= gain;
        const std::string file = noteName(t.note) + "_v" + std::to_string(t.velocity) + ".wav";
        std::string err;
        if (!hsynth::writeWav(hsynth::utf8String(sdir / file), t.rec.left, t.stereo ? &t.rec.right : nullptr, sr, false, err,
                              t.note, t.loopStart, t.loopEnd >= 0 ? t.loopEnd - 1 : -1)) {
            error = err;
            return false;
        }
        bytes += t.rec.left.size() * (t.stereo ? 4 : 2);
        seconds += (double)t.rec.left.size() / sr;

        // key range: halfway to the neighbouring sampled notes
        const size_t ni = (size_t)(std::find(notes.begin(), notes.end(), t.note) - notes.begin());
        int lo, hi;
        if (!spec.keys.empty()) {
            lo = hi = t.note;
        } else {
            lo = ni == 0 ? 0 : (notes[ni - 1] + t.note) / 2 + 1;
            hi = ni + 1 == notes.size() ? 127 : (t.note + notes[ni + 1]) / 2;
        }
        const size_t vi = (size_t)(std::find(vels.begin(), vels.end(), t.velocity) - vels.begin());
        const int lovel = vi == 0 ? 1 : (vels[vi - 1] + t.velocity) / 2 + 1;
        const int hivel = vi + 1 == vels.size() ? 127 : (t.velocity + vels[vi + 1]) / 2;
        if (vi == 0 && ni < spec.keyNames.size() && !spec.keyNames[ni].empty()) sfz += "// " + spec.keyNames[ni] + "\n";
        char line[512];
        std::snprintf(line, sizeof line, "<region> sample=%s lokey=%d hikey=%d pitch_keycenter=%d lovel=%d hivel=%d", file.c_str(),
                      lo, hi, t.note, lovel, hivel);
        sfz += line;
        if (t.loopStart >= 0)
            sfz += " loop_mode=loop_sustain loop_start=" + std::to_string(t.loopStart) + " loop_end=" + std::to_string(t.loopEnd - 1);
        else if (!spec.oneShot)
            sfz += " loop_mode=no_loop";
        sfz += "\n";
    }
    {
        std::ofstream f(dir / hsynth::utf8Path(leaf + ".sfz"), std::ios::binary);
        f << sfz;
    }
    const float gainDb = 20.0f * std::log10(gain);
    if (opt.verbose)
        std::printf("  -> %zu samples (%d looped%s), %.1f MB, gain %+.1f dB\n", takes.size(), loops,
                    clipped ? (", " + std::to_string(clipped) + " CLIPPED").c_str() : "", bytes / 1048576.0, gainDb);
    if (result) {
        result->samples = (int)takes.size();
        result->loops = loops;
        result->clipped = clipped;
        result->seconds = seconds;
        result->megabytes = bytes / 1048576.0;
        result->gainDb = gainDb;
    }

    if (!opt.presetDir.empty()) {
        hsynth::Preset p;
        p.name = opt.presetPrefix + spec.title;
        p.category = spec.category;
        p.author = "hsynth_sampler";
        p.comment = "Sampled for testing only - not for distribution";
        fs::path sfzPath = dir / hsynth::utf8Path(leaf + ".sfz");
        fs::path rel = opt.libraryRoot.empty() ? sfzPath : fs::relative(sfzPath, hsynth::utf8Path(opt.libraryRoot));
        p.samples[0] = hsynth::utf8String(rel);
        auto set = [&](const char* name, float v) { p.params[(size_t)hsynth::findParam(name)] = v; };
        set("a.type", (float)hsynth::LayerType::PCM);
        set("filter.type", (float)hsynth::FilterType::Off);
        set("amp.attack", 0.001f);
        set("amp.sustain", 1.0f);
        set("amp.release", spec.presetRelease);
        set("amp.velsens", 0.0f);
        hsynth::applyLegacyParam(p, "reverb.mix", spec.presetReverb);
        set("mod2.amt", 0.0f);  // default timbre->cutoff route is irrelevant with the filter off
        fs::create_directories(hsynth::utf8Path(opt.presetDir));
        std::string err;
        std::string file = opt.presetPrefix + spec.title;
        for (auto& c : file)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        hsynth::savePresetFile(hsynth::utf8String(hsynth::utf8Path(opt.presetDir) / (file + ".json")), p, err);
    }
    return true;
}

std::vector<InstrumentSpec> gmTestSet() {
    using L = InstrumentSpec::Loop;
    auto mk = [](std::string name, std::string title, int prog, int lo, int hi, std::vector<int> vels, double hold, L loop,
                 float release) {
        InstrumentSpec s;
        s.name = std::move(name);
        s.title = std::move(title);
        s.program = prog;
        s.lowNote = lo;
        s.highNote = hi;
        s.velocities = std::move(vels);
        s.holdSec = hold;
        s.loop = loop;
        s.presetRelease = release;
        return s;
    };
    std::vector<InstrumentSpec> v = {
        mk("grand_piano", "Grand Piano", 0, 21, 108, {40, 80, 120}, 6.0, L::Off, 0.5f),
        mk("epiano", "E.Piano", 4, 28, 103, {50, 110}, 4.0, L::Off, 0.4f),
        mk("vibraphone", "Vibraphone", 11, 53, 89, {50, 110}, 4.0, L::Off, 0.6f),
        mk("organ", "Drawbar Organ", 16, 36, 96, {100}, 3.0, L::On, 0.08f),
        mk("nylon_guitar", "Nylon Guitar", 24, 40, 88, {50, 110}, 4.0, L::Off, 0.3f),
        mk("finger_bass", "Finger Bass", 33, 28, 67, {50, 110}, 3.0, L::Off, 0.2f),
        mk("harp", "Harp", 46, 24, 103, {50, 110}, 4.0, L::Off, 0.8f),
        mk("strings", "String Ensemble", 48, 36, 96, {60, 115}, 4.0, L::On, 0.6f),
        mk("choir", "Choir Aahs", 52, 48, 84, {90}, 4.0, L::On, 0.6f),
        mk("trumpet", "Trumpet", 56, 52, 84, {60, 115}, 3.0, L::On, 0.15f),
        mk("brass", "Brass Section", 61, 40, 84, {60, 115}, 3.0, L::On, 0.2f),
        mk("alto_sax", "Alto Sax", 65, 49, 80, {60, 115}, 3.0, L::On, 0.15f),
        mk("flute", "Flute", 73, 60, 96, {60, 110}, 3.0, L::On, 0.2f),
        mk("warm_pad", "Warm Pad", 89, 36, 96, {100}, 4.0, L::On, 1.0f),
    };
    InstrumentSpec drums;
    drums.name = "gm_drums";
    drums.title = "Standard Drum Kit";
    drums.drum = true;
    drums.program = 0;
    for (int k = 35; k <= 81; ++k) drums.keys.push_back(k);
    drums.velocities = {60, 115};
    drums.holdSec = 0.3;
    drums.maxReleaseSec = 3.0;
    drums.oneShot = true;
    drums.loop = L::Off;
    drums.presetRelease = 0.05f;
    drums.presetReverb = 0.08f;
    v.push_back(drums);
    return v;
}

}  // namespace sampler
