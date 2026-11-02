#include "hsynth/host_utils.h"

#include <algorithm>

#include "hsynth/fx/ir_load.h"

namespace hsynth {

namespace {
bool isAbsolute(const std::string& p) {
    return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
}
std::string joinPath(const std::string& root, const std::string& rel) {
    if (root.empty() || isAbsolute(rel)) return rel;
    const char last = root.back();
    return (last == '/' || last == '\\') ? root + rel : root + "/" + rel;
}
}  // namespace

bool loadLayerSamples(Engine& engine, int layer, const std::string& path, std::string& error,
                      std::vector<std::string>& warnings) {
    auto set = loadSampleSet(path, error);
    if (!set) return false;
    for (auto& w : set->warnings) warnings.push_back(w);
    engine.setSampleSet(layer, std::move(set));
    engine.collectGarbage();
    return true;
}

bool loadLayerWavetable(Engine& engine, int layer, const std::string& path, std::string& error) {
    auto wt = loadWavetableFile(path, error);
    if (!wt) return false;
    auto set = std::make_unique<SampleSet>();
    set->name = wt->name;
    set->sourcePath = path;
    set->wavetable = std::move(wt);
    engine.setSampleSet(layer, std::move(set));
    engine.collectGarbage();
    return true;
}

bool applyPreset(Engine& engine, const Preset& preset, const std::string& libraryRoot,
                 std::vector<std::string>& warnings) {
    for (auto& w : preset.warnings) warnings.push_back(w);
    for (int l = 0; l < kNumLayers; ++l) {
        const auto type = (LayerType)(int)preset.params[(size_t)P::layer(l, P::LType)];
        const std::string& rel = preset.samples[l];
        if (rel.empty()) {
            if (type == LayerType::PCM) warnings.push_back(std::string("layer ") + (char)('A' + l) + " is PCM but has no samples");
            engine.setSampleSet(l, nullptr);
            continue;
        }
        const std::string full = joinPath(libraryRoot, rel);
        if (engine.sampleSetPath(l) == full) continue;  // already loaded
        std::string err;
        const bool ok = type == LayerType::WT ? loadLayerWavetable(engine, l, full, err)
                                              : loadLayerSamples(engine, l, full, err, warnings);
        if (!ok) {
            warnings.push_back(std::string("layer ") + (char)('A' + l) + ": " + err);
            engine.setSampleSet(l, nullptr);
        }
    }
    for (int t = 0; t < 2; ++t) {
        const std::string& rel = preset.impulses[t];
        if (rel.empty()) {
            engine.setFxImpulse(t, nullptr);
            continue;
        }
        const std::string full = joinPath(libraryRoot, rel);
        if (engine.fxImpulsePath(t) == full) continue;  // already loaded
        std::string err;
        auto ir = fx::loadImpulseResponse(full, engine.sampleRate(), t, err);
        if (!ir) warnings.push_back(std::string(t == 0 ? "cabinet IR: " : "reverb IR: ") + err);
        engine.setFxImpulse(t, std::move(ir));
    }
    engine.setAllParams(preset.params.data());
    engine.collectGarbage();
    return true;
}

Preset capturePreset(const Engine& engine, const std::string& name, const std::string& libraryRoot) {
    Preset p;
    p.name = name;
    engine.getAllParams(p.params.data());
    std::string root = libraryRoot;
    std::replace(root.begin(), root.end(), '\\', '/');
    if (!root.empty() && root.back() != '/') root += '/';
    auto relative = [&](std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (!root.empty() && path.rfind(root, 0) == 0) path = path.substr(root.size());
        return path;
    };
    for (int l = 0; l < kNumLayers; ++l) p.samples[l] = relative(engine.sampleSetPath(l));
    for (int t = 0; t < 2; ++t) p.impulses[t] = relative(engine.fxImpulsePath(t));
    return p;
}

void addNote(RenderJob& job, double time, double duration, int key, float velocity, uint32_t id) {
    if (id == 0) id = (uint32_t)(job.events.size() + 1000);
    job.events.push_back({time, Event::noteOn(id, key, velocity)});
    job.events.push_back({time + duration, Event::noteOff(id, key)});
    std::stable_sort(job.events.begin(), job.events.end(),
                     [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.time < b.time; });
}

void renderOffline(Engine& engine, const RenderJob& job, std::vector<float>& left, std::vector<float>& right) {
    const int64_t total = (int64_t)(job.seconds * job.sampleRate);
    left.assign((size_t)total, 0.0f);
    right.assign((size_t)total, 0.0f);
    size_t next = 0;
    for (int64_t pos = 0; pos < total; pos += job.blockSize) {
        const double t = (double)(pos + job.blockSize) / job.sampleRate;
        while (next < job.events.size() && job.events[next].time < t) engine.sendEvent(job.events[next++].event);
        const int n = (int)std::min<int64_t>(job.blockSize, total - pos);
        engine.process(left.data() + pos, right.data() + pos, n);
        engine.collectGarbage();
    }
}

}  // namespace hsynth
