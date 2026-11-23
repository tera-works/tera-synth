// Decent Sampler (.dspreset) XML instrument parser.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hsynth/sample_set.h"

namespace hsynth {

using OggDecodeFn =
    std::function<bool(const uint8_t* oggData, size_t oggBytes, std::vector<float>& outMono, int& outRate)>;

// Loads a Decent Sampler .dspreset file into SampleSet, decoding WAV, FLAC,
// and optionally OGG samples (when oggDecode is provided).
bool loadDecentSamplerPreset(const std::string& dspresetPath, SampleSet& out, std::string& error,
                             std::vector<std::string>& warnings, OggDecodeFn oggDecode = nullptr);

}  // namespace hsynth
