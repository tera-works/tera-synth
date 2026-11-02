// Sample / preset library on disk for desktop hosts (same layout as Android):
//
//   <data>/library/factory/...   demo content (synced from the repository assets)
//   <data>/library/user/...      imported WAV files and SFZ folders
//   <data>/presets/factory/      factory presets
//   <data>/presets/user/         user presets
//
// Preset sample paths are relative to <data>/library, so presets move freely
// between Windows, Linux and Android.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// UTF-8 conversions (paths are UTF-8 everywhere in the app).
std::string pathToUtf8(const fs::path& p);
fs::path utf8ToPath(const std::string& s);

class HostLibrary {
public:
    struct Entry {
        fs::path file;
        std::string title;
        bool user = false;
    };

    HostLibrary(fs::path dataDir, fs::path factorySource);

    // Copy factory content from the repository (overwrites, so regenerated
    // assets take effect). Returns an error message or "".
    std::string syncFactory();

    fs::path libraryRoot() const { return data_ / "library"; }
    fs::path userPresetDir() const { return data_ / "presets" / "user"; }
    fs::path dataDir() const { return data_; }

    std::vector<Entry> presets() const;
    std::vector<Entry> sampleSources() const;

    // Copy a WAV into library/user/wav. Throws std::runtime_error.
    fs::path importWav(const fs::path& src);
    // Copies an impulse-response WAV into library/user/ir (unless it is
    // already inside the library) and returns the path to load.
    fs::path importIr(const fs::path& src);
    // Copy an SFZ instrument folder (sfz/wav/txt files, keeping relative paths).
    std::vector<fs::path> importFolder(const fs::path& src, int& filesCopied);
    // Copy a preset JSON into presets/user.
    fs::path importPreset(const fs::path& src);

    fs::path userPresetFile(const std::string& name) const;
    static std::string safeName(const std::string& name);

private:
    fs::path data_;
    fs::path factorySrc_;
};
