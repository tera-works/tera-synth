// General MIDI sound selection for song (SMF) playback: maps GM program
// numbers (and the drum channel) to the presets the user has, then loads the
// presets of a song's channels into the matching parts.
//
// Presets are recognised by name: SonicCell "GM2 Capital NNN" captures
// (program NNN-1), the "GM Test" sampled instruments, drum kits, and a few
// factory presets by keyword. Missing programs fall back to the nearest one
// in the same GM family, then to a related family.
#pragma once

#include <string>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/preset_index.h"
#include "hsynth/smf.h"

namespace hsynth {

const char* gmProgramName(int program);  // "Acoustic Grand Piano" ... (0..127)

class GmBank {
public:
    struct Entry {
        int program = -1;      // 0..127, -1 for drum kits
        bool drum = false;
        int quality = 0;       // higher wins among equals
        std::string file, name;
    };
    void build(const std::vector<PresetInfo>& presets);
    // Preset file for a program (or the drum kit); "" if the bank is empty.
    const Entry* pick(int program, bool drum) const;
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

// Puts the engine in multi-timbral mode and loads, for every channel the song
// uses, the preset of its first program into part = channel (channel 10 =
// drums). loaded[16] caches the preset file per part between songs. Lines
// for the user ("ch 1: Piano 1 <- Acoustic Grand Piano") go to report.
struct SongPart {
    int channel = 0;          // 0..15 (= part)
    bool drum = false;
    int program = 0;
    std::string gmName;       // "Acoustic Grand Piano" / "Drum Kit"
    std::string presetName;   // "" if nothing could be loaded
};
bool prepareSong(Engine& engine, const Song& song, const GmBank& bank, const std::string& libraryRoot,
                 std::string loaded[kNumParts], std::vector<std::string>& report, std::vector<SongPart>* parts = nullptr);

}  // namespace hsynth
