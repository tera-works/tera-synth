#include "hsynth/sample_set.h"

#include <algorithm>
#include <cmath>
#include <cctype>

#include "hsynth/dsp/resample.h"
#include "hsynth/flac.h"
#include "hsynth/wav.h"

namespace hsynth {

size_t SampleSet::memoryBytes() const {
    if (wavetable) return wavetable->memoryBytes();
    size_t n = 0;
    for (const auto& s : samples) {
        n += (s->data[0].size() + s->data[1].size()) * sizeof(float);
        for (int l = 0; l < s->mipLevels; ++l) n += (s->mip[l][0].size() + s->mip[l][1].size()) * sizeof(float);
    }
    return n;
}

void Sample::buildMips(int levels) {
    levels = std::max(0, std::min(kMaxMips, levels));
    for (int l = mipLevels; l < levels; ++l) {
        const int64_t inFrames = levelFrames(l);
        for (int c = 0; c < channels; ++c) {
            const std::vector<float>& src = l == 0 ? data[c] : mip[l - 1][c];
            resample::decimate2(src, (size_t)inFrames, mip[l][c]);
        }
        mipFrames[l] = (inFrames + 1) / 2;
    }
    mipLevels = std::max(mipLevels, levels);
}

void SampleSet::prepareMips() {
    for (auto& sp : samples) {
        Sample* s = sp.get();
        double maxSemis = -1e9;
        for (const Region& r : regions) {
            if (r.sample != s) continue;
            const double semis = (double)(r.hikey - r.keycenter) * r.pitchKeytrack / 100.0 + r.transpose + r.tune / 100.0;
            maxSemis = std::max(maxSemis, semis);
        }
        if (maxSemis < -1e8) continue;
        // +2 semitones pitch bend headroom; rate relative to the lowest common engine rate
        const double step = std::exp2((maxSemis + 2.0) / 12.0) * s->sampleRate / 44100.0;
        s->buildMips(step > 8.0 ? 3 : step > 4.0 ? 2 : step > 2.0 ? 1 : 0);
    }
}

namespace {
// position in a crossfade range [lo, hi] (callers ensure hi > lo)
float xfPos(int v, int lo, int hi) { return std::min(1.0f, std::max(0.0f, (float)(v - lo) / (float)(hi - lo))); }
float xfCurve(float t, bool power) { return power ? std::sqrt(t) : t; }
}  // namespace

float Region::crossfadeGain(int key, int vel) const {
    float g = 1.0f;
    if (xfinHiVel > xfinLoVel) g *= xfCurve(xfPos(vel, xfinLoVel, xfinHiVel), xfVelPower);
    if (xfoutHiVel > xfoutLoVel) g *= xfCurve(1.0f - xfPos(vel, xfoutLoVel, xfoutHiVel), xfVelPower);
    if (xfinHiKey > xfinLoKey) g *= xfCurve(xfPos(key, xfinLoKey, xfinHiKey), xfKeyPower);
    if (xfoutHiKey > xfoutLoKey) g *= xfCurve(1.0f - xfPos(key, xfoutLoKey, xfoutHiKey), xfKeyPower);
    return g;
}

int SampleSet::findRegions(const RegionQuery& q, const Region** out, int max) const {
    int n = 0;
    const int sw = q.keyswitch >= 0 ? q.keyswitch : swDefault;
    for (const Region& r : regions) {
        if (r.trigger != q.trigger) continue;
        if (q.key < r.lokey || q.key > r.hikey) continue;
        if (q.vel127 < r.lovel || q.vel127 > r.hivel) continue;
        if (q.rand01 < r.lorand || q.rand01 >= r.hirand) continue;
        if (r.seqLength > 1 && (int)(q.seq % (uint32_t)r.seqLength) != r.seqPosition - 1) continue;
        if (r.swLast >= 0 && r.swLast != sw) continue;
        if (r.crossfadeGain(q.key, q.vel127) <= 0.0f) continue;  // fully faded out: not started at all
        if (!r.sample) continue;
        out[n++] = &r;
        if (n >= max) break;
    }
    return n;
}

namespace {
bool endsWithFlac(const std::string& p) {
    if (p.size() < 5) return false;
    std::string e = p.substr(p.size() - 5);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".flac";
}
}  // namespace

// WAV or FLAC (by extension).
std::unique_ptr<Sample> sampleFromWav(const std::string& path, std::string& error) {
    WavData w;
    if (!(endsWithFlac(path) ? readFlac(path, w, error) : readWav(path, w, error))) return nullptr;
    auto s = std::make_unique<Sample>();
    s->path = path;
    s->channels = w.channels;
    s->sampleRate = w.sampleRate;
    s->frames = w.frames;
    s->unityNote = w.unityNote;
    if (w.loopStart >= 0) {
        s->loopStart = w.loopStart;
        s->loopEnd = w.loopEnd + 1;  // smpl end is inclusive
    }
    s->data[0] = std::move(w.left);
    s->data[0].resize((size_t)w.frames + Sample::kPadFrames, 0.0f);
    if (w.channels == 2) {
        s->data[1] = std::move(w.right);
        s->data[1].resize((size_t)w.frames + Sample::kPadFrames, 0.0f);
    }
    return s;
}

namespace {

std::string lowerExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = path.substr(dot + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

}  // namespace

std::unique_ptr<SampleSet> loadWavSampleSet(const std::string& path, std::string& error) {
    auto s = sampleFromWav(path, error);
    if (!s) return nullptr;
    auto set = std::make_unique<SampleSet>();
    set->name = baseName(path);
    set->sourcePath = path;
    Region r;
    r.sample = s.get();
    r.keycenter = s->unityNote >= 0 && s->unityNote < 128 ? s->unityNote : 60;
    if (s->loopStart >= 0) {
        r.loopMode = LoopMode::Continuous;
        r.loopStart = s->loopStart;
        r.loopEnd = s->loopEnd;
    }
    set->regions.push_back(r);
    set->samples.push_back(std::move(s));
    set->prepareMips();
    return set;
}

std::unique_ptr<SampleSet> loadSampleSet(const std::string& path, std::string& error) {
    const std::string ext = lowerExt(path);
    if (ext == "wav" || ext == "wave" || ext == "flac") return loadWavSampleSet(path, error);
    if (ext == "sfz") return loadSfzSampleSet(path, error);
    error = "unsupported file type: " + path;
    return nullptr;
}

}  // namespace hsynth
