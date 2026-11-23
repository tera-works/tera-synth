#include "hsynth/morph.h"
#include "hsynth/engine.h"
#include <algorithm>
#include "hsynth/json.h"
#include "hsynth/params.h"

#include <cmath>
#include <limits>

namespace hsynth {
static_assert(P::kPartParams <= std::numeric_limits<uint16_t>::max());

bool canMorphParam(int id) {
    if (!isPartParam(id) || id == P::Morph || (id >= P::TuneC && id <= P::TuneLast) ||
        (id >= P::PerfMorph && id <= P::MorphCurve) ||
        (id >= P::ArpOn && id <= P::ArpLatch) ||
        (id >= P::SeqOn && id <= P::SeqStepLast) ||
        (id >= P::StrumOn && id <= P::StrumJitter) ||
        (id >= P::CtlCc1 && id <= P::CtlCc4) ||
        id == P::BendRange || id == P::Unison || id == P::ChordInversion ||
        id == P::ScaleRoot || id == P::MsegPoints || id == P::MsegLoop) return false;
    if (id >= P::kLayerBase && id < P::kInsBase) {
        const int field = (id - P::kLayerBase) % P::LayerParamCount;
        if (field == P::PcmStart || field == P::PcmVelTrack || field == P::WtUnison) return false;
    }
    if (id >= P::MotionBase && id <= P::MotionLast && (id - P::MotionBase) % P::MotionLaneCount == P::MotionSteps)
        return false;
    const auto kind = paramTable()[id].kind;
    return kind == ParamKind::Float || kind == ParamKind::Int;
}

float MorphRoute::value(float position) const {
    if (position <= 0.0f) return a;
    if (position >= 1.0f) return b;
    return exponential ? std::exp2(logA + position * logSpan) : a + (b - a) * position;
}

bool MorphProgram::assign(int id, float a, float b) {
    if (!canMorphParam(id) || !std::isfinite(a) || !std::isfinite(b)) return false;
    int index = 0;
    while (index < count && routes[index].paramId != id) ++index;
    if (index == count && count == kCapacity) return false;
    a = sanitizeParam(id, a);
    b = sanitizeParam(id, b);
    const bool exp = a > 0.0f && b > 0.0f && paramTable()[id].curve == ParamCurve::Exp;
    const float logA = exp ? std::log2(a) : 0.0f;
    routes[index] = {static_cast<uint16_t>(id), a, b, logA, exp ? std::log2(b) - logA : 0.0f, exp};
    if (index == count) ++count;
    return true;
}

void MorphProgram::clear(int id) {
    for (int i = 0; i < count; ++i) {
        if (routes[i].paramId != id) continue;
        for (int j = i + 1; j < count; ++j) routes[j - 1] = routes[j];
        --count;
        break;
    }
}

Json morphToJson(const MorphProgram& program) {
    Json result = Json::object(), routes = Json::array();
    for (int i = 0; i < program.count; ++i) {
        const auto& r = program.routes[i];
        Json route = Json::object();
        route.set("param", paramTable()[r.paramId].name);
        route.set("a", r.a);
        route.set("b", r.b);
        routes.push(std::move(route));
    }
    result.set("routes", std::move(routes));
    return result;
}

bool morphFromJson(const Json& json, MorphProgram& program) {
    const auto* routes = json.find("routes");
    if (!routes || !routes->isArray() || routes->size() > MorphProgram::kCapacity) return false;
    MorphProgram parsed;
    for (size_t i = 0; i < routes->size(); ++i) {
        const auto& route = routes->at(i);
        const auto* param = route.find("param");
        const auto* a = route.find("a");
        const auto* b = route.find("b");
        if (!param || !param->isString() || !a || !a->isNumber() || !b || !b->isNumber()) return false;
        const int id = findParam(param->string());
        // Newer engines can add targets without invalidating the entire patch.
        if (!canMorphParam(id)) continue;
        if (!parsed.assign(id, static_cast<float>(a->number()), static_cast<float>(b->number()))) return false;
    }
    program = parsed;
    return true;
}

Engine::PartSoundState::PartSoundState() {
    for (auto& value : display) value.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
}

Engine::SoundBank Engine::PartSoundState::hostValue() const {
    if (middle.load(std::memory_order_acquire) & 4u)
        reader = middle.exchange(reader, std::memory_order_acq_rel) & ~4u;
    const auto host = hostToAudio.get();
    return host.serial >= outgoing[reader].serial ? host : outgoing[reader];
}

void Engine::PartSoundState::publish(const PartSoundOptions& options, bool resetMorph) {
    const auto next = serial.fetch_add(1, std::memory_order_relaxed) + 1;
    hostToAudio.update([&](SoundBank& bank) { bank = {options, next, resetMorph}; return true; });
}

void Engine::PartSoundState::publishStage() {
    outgoing[writer] = current;
    writer = middle.exchange(writer | 4u, std::memory_order_acq_rel) & ~4u;
}

void Engine::publishSound(int part, const PartSoundOptions& options, bool resetMorph) {
    sound_[part].publish(options, resetMorph);
    soundPending_.fetch_or(1u << part, std::memory_order_release);
}

Engine::PartSoundOptions Engine::partSoundOptions(int part) const {
    if (!validPart(part)) return {};
    auto& state = sound_[part];
    std::lock_guard<std::mutex> lock(state.hostMutex);
    return state.hostValue().options;
}

void Engine::setPartSoundOptions(int part, const PartSoundOptions& options) {
    if (!validPart(part)) return;
    auto& state = sound_[part];
    std::lock_guard<std::mutex> lock(state.hostMutex);
    state.haveA = false;
    publishSound(part, options, true);
}

void Engine::morphSetA(int part) {
    if (!validPart(part)) return;
    auto& state = sound_[part];
    std::lock_guard<std::mutex> lock(state.hostMutex);
    auto options = state.hostValue().options;
    state.baselineStage = state.outgoing[state.reader].serial;
    for (int i = 0; i < P::kPartParams; ++i) state.baseA[i] = morphNow(part, i);
    state.haveA = true;
    // Materialize active route values in the editable row before clearing.
    for (int i = 0; i < options.morph.count; ++i) {
        const int id = options.morph.routes[i].paramId;
        setPartParam(part, id, state.baseA[id]);
    }
    options.morph = {};
    publishSound(part, options);
}

int Engine::morphCaptureB(int part) {
    if (!validPart(part)) return 0;
    auto& state = sound_[part];
    std::lock_guard<std::mutex> lock(state.hostMutex);
    auto options = state.hostValue().options;
    if (!state.haveA || state.baselineStage != state.outgoing[state.reader].serial) return 0;
    options.morph = {};
    for (int id = 0; id < P::kPartParams && options.morph.count < MorphProgram::kCapacity; ++id) {
        const float b = getPartParam(part, id);
        if (b != state.baseA[id]) options.morph.assign(id, state.baseA[id], b);
    }
    setPartParam(part, P::PerfMorph, 1.0f);
    publishSound(part, options);
    return options.morph.count;
}

bool Engine::morphAssign(int part, int id, float a, float b) {
    if (!validPart(part)) return false;
    auto& state = sound_[part];
    std::lock_guard<std::mutex> lock(state.hostMutex);
    auto options = state.hostValue().options;
    if (!options.morph.assign(id, a, b)) return false;
    publishSound(part, options);
    return true;
}

void Engine::morphClear(int part, int id) {
    if (!validPart(part)) return;
    auto& state = sound_[part];
    std::lock_guard<std::mutex> lock(state.hostMutex);
    auto options = state.hostValue().options;
    if (id < 0) options.morph = {};
    else options.morph.clear(id);
    publishSound(part, options);
}
void Engine::morphClearAll(int part) { morphClear(part, -1); }

float Engine::morphNow(int part, int id) const {
    if (validPart(part) && isPartParam(id)) {
        const float value = sound_[part].display[id].load(std::memory_order_relaxed);
        if (!std::isnan(value)) return value;
    }
    return getPartParam(part, id);
}

bool Engine::setArpPatternJson(int part, const Json& json) {
    if (!validPart(part)) return false;
    auto& state = sound_[part];
    std::lock_guard<std::mutex> lock(state.hostMutex);
    auto options = state.hostValue().options;
    if (!arpPatternFromJson(json, options.arpPattern)) return false;
    publishSound(part, options);
    return true;
}
ArpPattern Engine::arpPattern(int part) const { return partSoundOptions(part).arpPattern; }
int Engine::arpStep(int part) const { return validPart(part) ? statArpStep_[part].load(std::memory_order_relaxed) : -1; }

bool Engine::setRhythmJson(const Json& json) {
    return rhythmConfig_.update([&](RhythmConfig& next) {
        bool fill = rhythmFill_.load(std::memory_order_relaxed);
        if (!rhythmFromJson(json, next, fill)) return false;
        if (json.find("fill")) setRhythmFill(fill);
        return true;
    });
}
Json Engine::rhythmJson() const {
    return rhythmToJson(rhythmConfig_.get(), rhythmFill_.load(std::memory_order_relaxed));
}

int Engine::physModes(int part, int layer, float* out, int capacity) const {
    if (!validPart(part) || layer < 0 || layer >= kNumLayers) return 0;
    std::array<float, P::kPartParams> values{};
    for (int field = P::PhysModel; field <= P::PhysBowPress; ++field) {
        const int id = P::layer(layer, field);
        values[id] = morphNow(part, id);
    }
    return physicalModes(values.data(), layer, out, capacity);
}

const float* Engine::basePartParams(int part) const {
    return part == 0 ? p_ : partP_.get() + static_cast<size_t>(part - 1) * P::kPartParams;
}

void Engine::invalidateEffective(int part) {
    auto& state = sound_[part];
    if (!state.current.options.morph.count) return;
    state.copied = false;
    state.evaluate = true;
    state.insertDirty = true;
}

void Engine::applySoundOptions(int part, const SoundBank& bank) {
    auto& state = sound_[part];
    const auto& prior = state.current.options.morph;
    const bool hadRoutes = prior.count != 0;
    for (int i = 0; i < prior.count; ++i)
        state.display[prior.routes[i].paramId].store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
    state.current = bank;
    state.copied = false;
    state.evaluate = true;
    state.initialize = !hadRoutes || bank.resetMorph;
    state.insertDirty = hadRoutes;
    if (bank.options.morph.count) morphParts_ |= 1u << part;
    else {
        morphParts_ &= ~(1u << part);
        parts_[part].p = parts_[part].ctx.params = basePartParams(part);
        state.insertDirty = hadRoutes;
        state.remaining = 0;
    }
}

void Engine::applyStagedSound(int part, const PartSoundOptions& options) {
    auto& state = sound_[part];
    const auto serial = state.serial.fetch_add(1, std::memory_order_relaxed) + 1;
    applySoundOptions(part, {options, serial, true});
    state.remaining = 0;
    state.publishStage();
    evaluateMorph(part, 0);
}

bool Engine::syncSoundOptions() {
    if (!soundPending_.load(std::memory_order_acquire)) return false;
    const uint32_t pending = soundPending_.exchange(0, std::memory_order_acq_rel);
    bool changed = false;
    for (int k = 0; k < kNumParts; ++k) {
        if (!(pending & (1u << k))) continue;
        auto& state = sound_[k];
        if (!state.hostToAudio.sync()) continue;
        const auto& next = state.hostToAudio.audioValue();
        if (next.serial <= state.current.serial) continue;
        applySoundOptions(k, next);
        changed = true;
    }
    return changed;
}

void Engine::evaluateMorph(int part, int frames) {
    auto& state = sound_[part];
    const auto& program = state.current.options.morph;
    if (!program.count) return;
    auto& ps = parts_[part];
    const float* base = basePartParams(part);
    float target = base[P::PerfMorph];
    switch (static_cast<int>(base[P::MorphSource])) {
        case 1: target = p_[P::SuperKnob]; break;
        case 2: target = ps.ctx.modWheel; break;
        case 3: target = ps.ctx.expression; break;
        case 4: target = ps.ctx.chanPressure; break;
    }
    if (!state.copied) {
        std::copy_n(base, P::kPartParams, state.effective.data());
        ps.p = ps.ctx.params = state.effective.data();
        state.copied = true;
        // New programs begin at their saved position. Parameter edits and
        // controller gestures on an existing program use the 15 ms ramp.
        if (state.initialize) {
            state.position = state.target = target;
            state.remaining = 0;
            state.initialize = false;
        }
    }
    if (target != state.target) {
        state.target = target;
        state.remaining = std::max(1, static_cast<int>(0.015f * sampleRate_));
        state.increment = (target - state.position) / state.remaining;
    }
    if (!state.remaining && !state.evaluate) return;
    if (frames > 0 && state.remaining) {
        const int advance = std::min(frames, state.remaining);
        state.remaining -= advance;
        state.position = state.remaining ? state.position + advance * state.increment : state.target;
    }
    float position = state.position;
    switch (static_cast<int>(base[P::MorphCurve])) {
        case 1: position *= position; break;
        case 2: position = position * (2.0f - position); break;
        case 3: position = position * position * (3.0f - 2.0f * position); break;
    }
    for (int i = 0; i < program.count; ++i) {
        const auto& route = program.routes[i];
        const float value = route.value(position);
        state.effective[route.paramId] = value;
        state.display[route.paramId].store(value, std::memory_order_relaxed);
        if (route.paramId >= P::kInsBlockBase) state.insertDirty = true;
    }
    state.evaluate = false;
}

void Engine::updateMorphedInsert(int part) {
    auto& state = sound_[part];
    if (!state.insertDirty) return;
    state.insertDirty = false;
    updateInserts(part);
}

}  // namespace hsynth
