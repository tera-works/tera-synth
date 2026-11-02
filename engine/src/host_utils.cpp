#include "hsynth/host_utils.h"

#include <algorithm>

#include "hsynth/fx/ir_load.h"
#include "hsynth/tuning.h"

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

bool loadPartSamples(Engine& engine, int part, int layer, const std::string& path, std::string& error,
                     std::vector<std::string>& warnings) {
    auto set = loadSampleSet(path, error);
    if (!set) return false;
    for (auto& w : set->warnings) warnings.push_back(w);
    engine.setPartSampleSet(part, layer, std::move(set));
    engine.collectGarbage();
    return true;
}

bool loadPartWavetable(Engine& engine, int part, int layer, const std::string& path, std::string& error) {
    auto wt = loadWavetableFile(path, error);
    if (!wt) return false;
    auto set = std::make_unique<SampleSet>();
    set->name = wt->name;
    set->sourcePath = path;
    set->wavetable = std::move(wt);
    engine.setPartSampleSet(part, layer, std::move(set));
    engine.collectGarbage();
    return true;
}

bool loadLayerSamples(Engine& engine, int layer, const std::string& path, std::string& error,
                      std::vector<std::string>& warnings) {
    return loadPartSamples(engine, 0, layer, path, error, warnings);
}

bool loadLayerWavetable(Engine& engine, int layer, const std::string& path, std::string& error) {
    return loadPartWavetable(engine, 0, layer, path, error);
}

namespace {
void applySamples(Engine& engine, int part, const Preset& preset, const std::string& libraryRoot,
                  std::vector<std::string>& warnings) {
    const std::string tag = part == 0 ? std::string("layer ") : "part " + std::to_string(part + 1) + " layer ";
    for (int l = 0; l < kNumLayers; ++l) {
        const auto type = (LayerType)(int)preset.params[(size_t)P::layer(l, P::LType)];
        const std::string& rel = preset.samples[l];
        if (rel.empty()) {
            if (type == LayerType::PCM) warnings.push_back(tag + (char)('A' + l) + " is PCM but has no samples");
            engine.setPartSampleSet(part, l, nullptr);
            continue;
        }
        const std::string full = joinPath(libraryRoot, rel);
        if (engine.partSampleSetPath(part, l) == full) continue;  // already loaded
        std::string err;
        const bool ok = type == LayerType::WT ? loadPartWavetable(engine, part, l, full, err)
                                              : loadPartSamples(engine, part, l, full, err, warnings);
        if (!ok) {
            warnings.push_back(tag + (char)('A' + l) + ": " + err);
            engine.setPartSampleSet(part, l, nullptr);
        }
    }
}

void applyImpulses(Engine& engine, const std::string* impulses, const std::string& libraryRoot,
                   std::vector<std::string>& warnings) {
    for (int t = 0; t < 2; ++t) {
        const std::string& rel = impulses[t];
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
}

struct Relativizer {
    std::string root;
    explicit Relativizer(const std::string& libraryRoot) : root(libraryRoot) {
        std::replace(root.begin(), root.end(), '\\', '/');
        if (!root.empty() && root.back() != '/') root += '/';
    }
    std::string operator()(std::string path) const {
        std::replace(path.begin(), path.end(), '\\', '/');
        if (!root.empty() && path.rfind(root, 0) == 0) path = path.substr(root.size());
        return path;
    }
};
}  // namespace

bool applyPreset(Engine& engine, const Preset& preset, const std::string& libraryRoot,
                 std::vector<std::string>& warnings) {
    for (auto& w : preset.warnings) warnings.push_back(w);
    applySamples(engine, 0, preset, libraryRoot, warnings);
    applyImpulses(engine, preset.impulses, libraryRoot, warnings);
    engine.setAllParams(preset.params.data());
    engine.collectGarbage();
    return true;
}

bool applyPartPreset(Engine& engine, int part, const Preset& preset, const std::string& libraryRoot,
                     std::vector<std::string>& warnings) {
    if (part < 0 || part >= kNumParts || (int)preset.params.size() != P::kNumParams) return false;
    for (auto& w : preset.warnings) warnings.push_back(w);
    applySamples(engine, part, preset, libraryRoot, warnings);
    engine.setPartParams(part, preset.params.data());
    engine.collectGarbage();
    return true;
}

Preset capturePreset(const Engine& engine, const std::string& name, const std::string& libraryRoot) {
    return capturePartPreset(engine, 0, name, libraryRoot);
}

Preset capturePartPreset(const Engine& engine, int part, const std::string& name, const std::string& libraryRoot) {
    Preset p;
    p.name = name;
    engine.getPartParams(part, p.params.data());
    const Relativizer relative(libraryRoot);
    for (int l = 0; l < kNumLayers; ++l) p.samples[l] = relative(engine.partSampleSetPath(part, l));
    for (int t = 0; t < 2; ++t) p.impulses[t] = relative(engine.fxImpulsePath(t));
    return p;
}

bool applyMulti(Engine& engine, const MultiPreset& multi, const std::string& libraryRoot,
                std::vector<std::string>& warnings) {
    for (auto& w : multi.warnings) warnings.push_back(w);
    engine.setMultiTimbral(true);
    applyImpulses(engine, multi.global.impulses, libraryRoot, warnings);
    engine.setGlobalParams(multi.global.params.data());
    for (int k = 0; k < kNumParts; ++k) {
        applyPartPreset(engine, k, multi.parts[k], libraryRoot, warnings);
        for (int i = 0; i < Mix::Count; ++i) engine.setPartMix(k, i, multi.mix[k][i]);
    }
    engine.selectPart(multi.selectedPart);
    engine.collectGarbage();
    return true;
}

MultiPreset captureMulti(const Engine& engine, const std::string& name, const std::string& libraryRoot) {
    MultiPreset m;
    m.name = name;
    m.selectedPart = engine.selectedPart();
    engine.getAllParams(m.global.params.data());
    const Relativizer relative(libraryRoot);
    for (int t = 0; t < 2; ++t) m.global.impulses[t] = relative(engine.fxImpulsePath(t));
    for (int k = 0; k < kNumParts; ++k) {
        m.parts[k] = capturePartPreset(engine, k, "Part " + std::to_string(k + 1), libraryRoot);
        m.parts[k].impulses[0].clear();
        m.parts[k].impulses[1].clear();
        for (int i = 0; i < Mix::Count; ++i) m.mix[k][i] = engine.getPartMix(k, i);
    }
    return m;
}

bool applyScalaTuning(Engine& engine, int part, const std::string& sclPath, int root, std::string& error) {
    ScalaScale s;
    if (!loadScalaFile(sclPath, s, error)) return false;
    float off[12];
    std::string warn;
    if (!scalaToOffsets(s, root, off, warn)) { error = warn; return false; }
    for (int i = 0; i < 12; ++i) engine.setPartParam(part, P::TuneC + i, off[i]);
    error = warn;  // non-fatal note (clamped degrees), empty if none
    return true;
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
