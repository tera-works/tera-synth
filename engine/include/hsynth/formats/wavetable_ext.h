// Extended wavetable importers: Surge XT / Bitwig (.wt) and Vital (.vitaltable),
// plus unified extension dispatch and UI preview sampling.
#pragma once

#include <string>

#include "hsynth/dsp/wavetable.h"

namespace hsynth {

// Load a Surge XT / Bitwig 'vawt' .wt wavetable file.
bool loadWtFile(const std::string& path, Wavetable& out, std::string& error);

// Load a Vital .vitaltable JSON wavetable file.
bool loadVitalTableFile(const std::string& path, Wavetable& out, std::string& error);

// Dispatch by extension: .wt -> loadWtFile, .vitaltable -> loadVitalTableFile,
// otherwise existing loadWavetableFile (.wav / .flac).
bool loadAnyWavetableFile(const std::string& path, Wavetable& out, std::string& error);

// Sample a normalized single-cycle preview waveform at table position pos (0..1).
void sampleWavetablePreview(const Wavetable& wt, float pos, int points, float* outSamples);

}  // namespace hsynth
