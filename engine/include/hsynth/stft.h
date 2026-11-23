#pragma once

#include <string>

#include "hsynth/json.h"

namespace hsynth {

// Worker-side file APIs. Magnitudes are tightly packed IEEE float32 LE in
// [frame][bin][channel] order. A nonpositive frame limit extracts the whole file.
Json stftExtractMag(const std::string& wavPath, const std::string& outMagFloatPath, int maxFrames);
Json stftReconstructStems(const std::string& wavPath, const std::string maskPaths[4], const std::string& outDir);
Json separateStemsFastSpectral(const std::string& wavPath, const std::string& outDir);

}  // namespace hsynth
