#pragma once

#include <algorithm>
#include <cmath>

#include "hsynth/json.h"

namespace hsynth::performance_json {

inline bool field(const Json& json, const char* key, bool& out) {
    const Json* v = json.find(key);
    if (!v) return true;
    if (!v->isBool()) return false;
    out = v->boolean();
    return true;
}

inline bool field(const Json& json, const char* key, float& out, float min, float max) {
    const Json* v = json.find(key);
    if (!v) return true;
    if (!v->isNumber() || !std::isfinite(v->number())) return false;
    out = static_cast<float>(std::clamp<double>(v->number(), min, max));
    return true;
}

template <typename Int>
bool field(const Json& json, const char* key, Int& out, int min, int max) {
    const Json* v = json.find(key);
    if (!v) return true;
    const double n = v->number();
    if (!v->isNumber() || !std::isfinite(n) || n != std::trunc(n)) return false;
    out = static_cast<Int>(std::clamp<double>(n, min, max));
    return true;
}

}  // namespace hsynth::performance_json
