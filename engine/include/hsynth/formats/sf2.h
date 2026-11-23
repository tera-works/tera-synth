// SoundFont 2 (.sf2) and SoundFont 3 (.sf3) parser.
//
// Parses RIFF 'sfbk' containers (INFO, sdta, pdta), resolves preset and
// instrument global + local zones, links stereo sample pairs, and optionally
// decodes SF3 Ogg Vorbis samples via a caller-supplied callback.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hsynth/sample_set.h"

namespace hsynth {

struct Sf2PresetInfo {
    int bank = 0;
    int program = 0;
    std::string name;
    int presetIndex = 0;
};

using OggDecodeFn =
    std::function<bool(const uint8_t* oggData, size_t oggBytes, std::vector<float>& outMono, int& outRate)>;

// Enumerates all non-EOP presets in an SF2/SF3 file.
bool listSf2Presets(const std::string& path, std::vector<Sf2PresetInfo>& out, std::string& error);

// Loads a single preset (by bank & program, or the first valid preset if
// bank < 0 || program < 0) into SampleSet.
bool loadSf2Preset(const std::string& path, int bank, int program, SampleSet& out, std::string& error,
                   std::vector<std::string>& warnings, OggDecodeFn oggDecode = nullptr);

}  // namespace hsynth
