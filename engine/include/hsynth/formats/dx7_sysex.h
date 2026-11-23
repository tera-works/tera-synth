// Yamaha DX7 SysEx (.syx) parser -> HSynth 8-Op FM presets.
//
// Supports 32-voice VMEM banks (4,104 bytes with header or 4,096 raw) and
// 1-voice VCED single dumps (163 bytes with header or 155 raw).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hsynth/preset.h"

namespace hsynth {

struct Dx7VoiceSummary {
    int index = 0;
    std::string name;
    int algorithm = 1;  // 1..32
};

// Parses DX7 SysEx VMEM or VCED bytes into HSynth FM presets.
bool parseDx7Sysex(const uint8_t* data, size_t size, std::vector<Preset>& outPresets, std::string& error,
                   std::vector<Dx7VoiceSummary>* summaries = nullptr);

// Loads and parses a .syx file from disk.
bool loadDx7SysexFile(const std::string& path, std::vector<Preset>& outPresets, std::string& error,
                     std::vector<Dx7VoiceSummary>* summaries = nullptr);

}  // namespace hsynth
