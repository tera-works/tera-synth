#include "hsynth/performance_input.h"

#include "performance_json.h"

namespace hsynth {

namespace {
float bounded(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

float interpolate(const std::array<float, PreparedPerformanceInput::kCurveSteps + 1>& table, float value) {
    if (!(value > 0.0f)) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    const float index = value * PreparedPerformanceInput::kCurveSteps;
    const int i = static_cast<int>(index);
    return table[i] + (index - i) * (table[i + 1] - table[i]);
}
}  // namespace

PerformanceInputConfig sanitizePerformanceInput(PerformanceInputConfig c) {
    c.velocityCurve = bounded(c.velocityCurve, -1.0f, 1.0f, 0.0f);
    c.velocityFixed = std::clamp(c.velocityFixed, 0, 127);
    c.aftertouchCurve = bounded(c.aftertouchCurve, -1.0f, 1.0f, 0.0f);
    c.expMin = bounded(c.expMin, 0.0f, 1.0f, 0.0f);
    c.expMax = bounded(c.expMax, 0.0f, 1.0f, 1.0f);
    for (auto& fs : c.footswitches) {
        fs.cc = std::clamp(fs.cc, -1, 119);
        fs.action = std::clamp(fs.action, 0, 10);
    }
    return c;
}

Json performanceInputToJson(const PerformanceInputConfig& c) {
    Json j = Json::object(), switches = Json::array();
    j.set("sustainInverted", c.sustainInverted);
    j.set("velocityCurve", c.velocityCurve);
    j.set("velocityFixed", c.velocityFixed);
    j.set("aftertouchCurve", c.aftertouchCurve);
    j.set("expMin", c.expMin);
    j.set("expMax", c.expMax);
    for (const auto& fs : c.footswitches) {
        Json binding = Json::object();
        binding.set("cc", fs.cc);
        binding.set("action", fs.action);
        binding.set("invert", fs.invert);
        switches.push(std::move(binding));
    }
    j.set("footswitches", std::move(switches));
    return j;
}

bool performanceInputFromJson(const Json& j, PerformanceInputConfig& config) {
    using performance_json::field;
    auto c = config;
    if (!j.isObject() || !field(j, "sustainInverted", c.sustainInverted) ||
        !field(j, "velocityCurve", c.velocityCurve, -1.0f, 1.0f) ||
        !field(j, "velocityFixed", c.velocityFixed, 0, 127) ||
        !field(j, "aftertouchCurve", c.aftertouchCurve, -1.0f, 1.0f) ||
        !field(j, "expMin", c.expMin, 0.0f, 1.0f) || !field(j, "expMax", c.expMax, 0.0f, 1.0f)) return false;
    if (const auto* switches = j.find("footswitches")) {
        if (!switches->isArray() || switches->size() > c.footswitches.size()) return false;
        for (size_t i = 0; i < switches->size(); ++i) {
            const auto& binding = switches->at(i);
            auto& fs = c.footswitches[i];
            if (!binding.isObject() || !field(binding, "cc", fs.cc, -1, 119) ||
                !field(binding, "action", fs.action, 0, 10) || !field(binding, "invert", fs.invert)) return false;
        }
    }
    config = c;
    return true;
}

void PreparedPerformanceInput::prepareCurves() {
    const float velExponent = std::exp2(1.5f * config.velocityCurve);
    const float atExponent = std::exp2(1.5f * config.aftertouchCurve);
    for (int i = 0; i <= kCurveSteps; ++i) {
        const float x = static_cast<float>(i) / kCurveSteps;
        velocity[i] = std::pow(x, velExponent);
        pressure[i] = std::pow(x, atExponent);
    }
}

float PreparedPerformanceInput::noteVelocity(float value) const {
    if (config.velocityFixed > 0) return config.velocityFixed / 127.0f;
    return config.velocityCurve == 0.0f ? value : interpolate(velocity, value);
}

float PreparedPerformanceInput::aftertouch(float value) const {
    return config.aftertouchCurve == 0.0f ? value : interpolate(pressure, value);
}

}  // namespace hsynth
