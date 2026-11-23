#include "hsynth/dsp/master_stage.h"

#include <cmath>
#include <limits>

#include "hsynth/json.h"

namespace hsynth {
static_assert(std::atomic<unsigned>::is_always_lock_free, "MasterStage mailbox must be lock-free");
namespace {
MasterStageConfig sanitize(MasterStageConfig c) {
    const MasterStageConfig defaults;
    auto limit = [](float v, float lo, float hi, float def) {
        return std::isfinite(v) ? clampf(v, lo, hi) : def;
    };
    for (size_t i = 0; i < c.bands.size(); ++i) {
        auto& b = c.bands[i];
        b.freq = limit(b.freq, 20, 20000, defaults.bands[i].freq);
        b.gainDb = limit(b.gainDb, -18, 18, 0);
        b.q = limit(b.q, 0.25f, 12, defaults.bands[i].q);
    }
    auto& p = c.comp;
    p.thresholdDb = limit(p.thresholdDb, -48, 0, -18);
    p.ratio = limit(p.ratio, 1, 20, 2);
    p.attackMs = limit(p.attackMs, 0.2f, 100, 10);
    p.releaseMs = limit(p.releaseMs, 20, 1000, 150);
    p.kneeDb = limit(p.kneeDb, 0, 18, 6);
    p.makeupDb = limit(p.makeupDb, -12, 18, 0);
    return c;
}

bool readBool(const Json& obj, const char* key, bool& value) {
    const Json* j = obj.find(key);
    if (!j) return true;
    if (!j->isBool()) return false;
    value = j->boolean();
    return true;
}

bool readFloat(const Json& obj, const char* key, float& value) {
    const Json* j = obj.find(key);
    if (!j) return true;
    if (!j->isNumber() || !std::isfinite(j->number()) ||
        std::abs(j->number()) > std::numeric_limits<float>::max()) return false;
    value = (float)j->number();
    return true;
}
}  // namespace

Json masterStageToJson(const MasterStageConfig& config) {
    Json j = Json::object(), bands = Json::array(), comp = Json::object();
    j.set("bypass", config.bypass);
    j.set("eqEnabled", config.eqEnabled);
    j.set("compEnabled", config.compEnabled);
    for (const auto& b : config.bands) {
        Json band = Json::object();
        band.set("freq", b.freq);
        band.set("gainDb", b.gainDb);
        band.set("q", b.q);
        band.set("enabled", b.enabled);
        bands.push(std::move(band));
    }
    j.set("bands", std::move(bands));
    const auto& p = config.comp;
    comp.set("thresholdDb", p.thresholdDb);
    comp.set("ratio", p.ratio);
    comp.set("attackMs", p.attackMs);
    comp.set("releaseMs", p.releaseMs);
    comp.set("kneeDb", p.kneeDb);
    comp.set("makeupDb", p.makeupDb);
    j.set("comp", std::move(comp));
    return j;
}

bool masterStageFromJson(const Json& j, MasterStageConfig& config) {
    auto next = config;
    if (!j.isObject() || !readBool(j, "bypass", next.bypass) ||
        !readBool(j, "eqEnabled", next.eqEnabled) || !readBool(j, "compEnabled", next.compEnabled)) return false;
    if (const auto* bands = j.find("bands")) {
        if (!bands->isArray() || bands->size() != next.bands.size()) return false;
        for (size_t i = 0; i < next.bands.size(); ++i) {
            auto& b = next.bands[i];
            const Json& band = bands->at(i);
            if (!band.isObject() || !readFloat(band, "freq", b.freq) ||
                !readFloat(band, "gainDb", b.gainDb) || !readFloat(band, "q", b.q) ||
                !readBool(band, "enabled", b.enabled)) return false;
        }
    }
    if (const auto* comp = j.find("comp")) {
        auto& p = next.comp;
        if (!comp->isObject() || !readFloat(*comp, "thresholdDb", p.thresholdDb) ||
            !readFloat(*comp, "ratio", p.ratio) || !readFloat(*comp, "attackMs", p.attackMs) ||
            !readFloat(*comp, "releaseMs", p.releaseMs) || !readFloat(*comp, "kneeDb", p.kneeDb) ||
            !readFloat(*comp, "makeupDb", p.makeupDb)) return false;
    }
    config = sanitize(next);
    return true;
}

void MasterStage::setConfig(const MasterStageConfig& config) {
    std::lock_guard<std::mutex> lock(writerMutex_);
    publish(config);
}

bool MasterStage::updateConfig(const std::function<bool(MasterStageConfig&)>& edit) {
    std::lock_guard<std::mutex> lock(writerMutex_);
    auto next = hostConfig_;
    if (!edit(next)) return false;
    publish(next);
    return true;
}

void MasterStage::publish(const MasterStageConfig& config) {
    hostConfig_ = sanitize(config);
    mailbox_[writer_] = hostConfig_;
    writer_ = middle_.exchange(writer_ | kDirty, std::memory_order_acq_rel) & ~kDirty;
}

MasterStageConfig MasterStage::getConfig() const {
    std::lock_guard<std::mutex> lock(writerMutex_);
    return hostConfig_;
}

void MasterStage::prepare(float sampleRate) {
    sampleRate_ = sampleRate;
    fadeLength_ = std::max(1, (int)std::lround(0.015f * sampleRate));
    active_ = fadeRemaining_ = 0;
    paths_[0] = {};
    paths_[1] = {};
    paths_[0].configure(getConfig(), sampleRate_);
    tail_ = false;
    grDb_.store(0, std::memory_order_relaxed);
}

bool MasterStage::sync() {
    if (fadeRemaining_ || !(middle_.load(std::memory_order_acquire) & kDirty)) return false;
    reader_ = middle_.exchange(reader_, std::memory_order_acq_rel) & ~kDirty;
    auto& next = paths_[1 - active_];
    auto& old = paths_[active_];
    next = old;
    next.configure(mailbox_[reader_], sampleRate_);
    if ((next.mask | old.mask) == 0 && !next.compOn && !old.compOn) return false;
    fadeRemaining_ = fadeLength_;
    return true;
}

void MasterStage::Path::configure(const MasterStageConfig& c, float sr) {
    unsigned nextMask = 0;
    if (!c.bypass && c.eqEnabled) {
        for (size_t i = 0; i < c.bands.size(); ++i) {
            const auto& b = c.bands[i];
            if (!b.enabled || b.gainDb == 0.0f) continue;
            nextMask |= 1u << i;
            if (!(mask & (1u << i))) bands[i].reset();
            const auto type = i == 0 ? fx::Biquad::LowShelf : i == 4 ? fx::Biquad::HighShelf : fx::Biquad::Peak;
            bands[i].l.set(type, b.freq, b.q, b.gainDb, sr, true);
            bands[i].r.copyCoefs(bands[i].l);
        }
    }
    mask = nextMask;
    compOn = !c.bypass && c.compEnabled;
    comp = c.comp;
    if (!compOn) reduction = 0.0f;
    attack = std::exp(-1000.0f / (sr * comp.attackMs));
    release = std::exp(-1000.0f / (sr * comp.releaseMs));
}

void MasterStage::Path::process(float& l, float& r) {
    for (size_t i = 0; i < bands.size(); ++i)
        if (mask & (1u << i)) bands[i].process(l, r);
    if (!compOn) return;
    const float peak = std::max(std::fabs(l), std::fabs(r));
    const float over = 20.0f * std::log10(std::max(1e-12f, peak)) - comp.thresholdDb;
    const float half = comp.kneeDb * 0.5f;
    float compressed = std::max(0.0f, over);
    if (half > 0.0f && std::fabs(over) < half)
        compressed = (over + half) * (over + half) / (2.0f * comp.kneeDb);
    const float target = (1.0f / comp.ratio - 1.0f) * compressed;
    const float k = target < reduction ? attack : release;
    reduction = target + k * (reduction - target);
    if (std::fabs(reduction) < 1e-5f) reduction = 0.0f;
    const float gain = std::exp2((comp.makeupDb + reduction) * 0.166096405f);
    l *= gain;
    r *= gain;
}

bool MasterStage::Path::hasTail() const {
    if (reduction != 0.0f) return true;
    for (size_t i = 0; i < bands.size(); ++i) {
        if (!(mask & (1u << i))) continue;
        const auto& b = bands[i];
        if (std::fabs(b.l.z1) > 1e-10f || std::fabs(b.l.z2) > 1e-10f ||
            std::fabs(b.r.z1) > 1e-10f || std::fabs(b.r.z2) > 1e-10f) return true;
    }
    return false;
}

void MasterStage::process(float* l, float* r, int n) {
    if (!fadeRemaining_ && !paths_[active_].mask && !paths_[active_].compOn) return;
    for (int i = 0; i < n; ++i) {
        float a = l[i], b = r[i];
        paths_[active_].process(a, b);
        if (fadeRemaining_) {
            float c = l[i], d = r[i];
            paths_[1 - active_].process(c, d);
            const float x = 1.0f - (float)(--fadeRemaining_) / (float)fadeLength_;
            a += (c - a) * x;
            b += (d - b) * x;
            if (!fadeRemaining_) active_ = 1 - active_;
        }
        l[i] = a;
        r[i] = b;
    }
    tail_ = paths_[active_].hasTail();
    grDb_.store(paths_[active_].reduction, std::memory_order_relaxed);
}

}  // namespace hsynth
