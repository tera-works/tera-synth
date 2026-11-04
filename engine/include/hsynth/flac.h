// FLAC decoder (no external dependencies). Decodes a whole file into memory
// (loader thread only), like the WAV reader. Supports every encoding the
// reference encoder produces: 4..32 bits, 1..8 channels (downmixed to at most
// 2 like WAV), fixed / LPC / verbatim / constant subframes, all stereo
// decorrelation modes, variable block sizes. Loop points / root key are read
// from a RIFF "smpl" chunk stored in an APPLICATION "riff" block (flac
// --keep-foreign-metadata) when present.
#pragma once

#include <cstdint>
#include <string>

#include "hsynth/wav.h"

namespace hsynth {

bool readFlac(const std::string& path, WavData& out, std::string& error);
bool readFlacMemory(const uint8_t* data, size_t size, WavData& out, std::string& error);

}  // namespace hsynth
