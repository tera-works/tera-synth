#pragma once

#include <string>
#include <vector>

#include "hsynth/sample_set.h"

namespace hsynth {

struct DlsPresetInfo {
    int bank;
    int program;
    bool isDrum;
    std::string name;
    int instrumentIndex;
};

// Lists instrument headers in file order without opening or decoding their waves.
bool listDlsGigPresets(const std::string& path, std::vector<DlsPresetInfo>& out, std::string& error);

// A negative bank or program selects the first playable instrument.
// Call on the loader thread. The caller's SampleSet is replaced only on success.
bool loadDlsGigPreset(const std::string& path, int bank, int program, bool isDrum,
                      SampleSet& out, std::string& error, std::vector<std::string>& warnings);

}  // namespace hsynth
