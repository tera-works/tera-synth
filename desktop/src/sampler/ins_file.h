// Reader for Cakewalk / SONAR instrument definition files (.ins), which most
// hardware synth makers publish: patch names per bank, drum kit key names.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace sampler {

struct InsPatch {
    int bank = 0;        // MSB * 128 + LSB
    int program = 0;     // 0..127
    std::string name;    // "Rich Grand"
    std::string list;    // name of the patch list (bank title)
    bool drum = false;
    std::map<int, std::string> keys;  // drum kits: key -> sound name
};

struct InsFile {
    std::map<std::string, std::map<int, std::string>> patchLists;  // .Patch Names
    std::map<std::string, std::map<int, std::string>> noteLists;   // .Note Names
    std::vector<InsPatch> patches;  // from .Instrument Definitions, in file order
};

// Appends the definitions of `path` to `out` (several files may be merged).
bool readIns(const std::string& path, InsFile& out, std::string& error);

}  // namespace sampler
