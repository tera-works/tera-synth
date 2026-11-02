#include "hsynth/sample_set.h"

#include <algorithm>
#include <cctype>

#include "hsynth/wav.h"

namespace hsynth {

size_t SampleSet::memoryBytes() const {
    if (wavetable) return wavetable->memoryBytes();
    size_t n = 0;
    for (const auto& s : samples) n += (s->data[0].size() + s->data[1].size()) * sizeof(float);
    return n;
}

int SampleSet::findRegions(int key, int vel127, float rand01, const Region** out, int max) const {
    int n = 0;
    for (const Region& r : regions) {
        if (key < r.lokey || key > r.hikey) continue;
        if (vel127 < r.lovel || vel127 > r.hivel) continue;
        if (rand01 < r.lorand || rand01 >= r.hirand) continue;
        if (!r.sample) continue;
        out[n++] = &r;
        if (n >= max) break;
    }
    return n;
}

std::unique_ptr<Sample> sampleFromWav(const std::string& path, std::string& error) {
    WavData w;
    if (!readWav(path, w, error)) return nullptr;
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
    return set;
}

std::unique_ptr<SampleSet> loadSampleSet(const std::string& path, std::string& error) {
    const std::string ext = lowerExt(path);
    if (ext == "wav" || ext == "wave") return loadWavSampleSet(path, error);
    if (ext == "sfz") return loadSfzSampleSet(path, error);
    if (ext == "flac") {
        error = "FLAC is not supported yet (planned); convert to WAV";
        return nullptr;
    }
    error = "unsupported file type: " + path;
    return nullptr;
}

}  // namespace hsynth
