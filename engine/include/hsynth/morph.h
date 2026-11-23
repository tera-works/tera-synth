#pragma once

#include <array>
#include <cstdint>

namespace hsynth {

class Json;

struct MorphRoute {
    uint16_t paramId = 0;
    float a = 0, b = 0;
    float logA = 0, logSpan = 0;
    bool exponential = false;

    float value(float position) const;
};

struct MorphProgram {
    static constexpr int kCapacity = 64;
    std::array<MorphRoute, kCapacity> routes{};
    int count = 0;

    bool assign(int id, float a, float b);
    void clear(int id);
};

bool canMorphParam(int id);
Json morphToJson(const MorphProgram& program);
bool morphFromJson(const Json& json, MorphProgram& program);

}  // namespace hsynth
