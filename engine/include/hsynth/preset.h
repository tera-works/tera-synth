// JSON preset format.
//
// {
//   "format": "hsynth-preset", "version": 1,
//   "name": "...", "category": "...", "author": "...", "comment": "...",
//   "samples": { "a": "factory/piano/piano.sfz", "b": "", "c": "" },
//   "impulses": { "cab": "user/ir/4x12.wav", "reverb": "" },   (optional)
//   "params": { "master.volume": 0.7, "a.type": "fm", ... }
// }
//
// Parameters are stored by name; enum values as their label. Missing params
// take their default value, unknown params are reported as warnings, so old
// presets keep loading as the engine grows.
#pragma once

#include <string>
#include <vector>

#include "hsynth/common.h"
#include "hsynth/params.h"

namespace hsynth {

struct Preset {
    std::string name = "Init";
    std::string category;
    std::string author;
    std::string comment;
    std::vector<float> params;          // kNumParams values
    std::string samples[kNumLayers];    // library-relative sample/SFZ paths
    std::string impulses[2];            // user IRs: cabinet, IR reverb (library-relative)
    std::vector<std::string> warnings;  // filled by fromJson

    Preset();
};

std::string presetToJson(const Preset& p);
bool presetFromJson(const std::string& json, Preset& out, std::string& error);
bool loadPresetFile(const std::string& path, Preset& out, std::string& error);
bool savePresetFile(const std::string& path, const Preset& p, std::string& error);

// Converts a parameter name from before the effect rack (chorus.*, delay.*,
// reverb.*, eq.low/high) into rack settings. Returns false if unknown.
bool applyLegacyParam(Preset& p, const std::string& name, float value);

// Parameter metadata as JSON (for generic editors on the host side).
std::string paramMetadataJson();

}  // namespace hsynth
