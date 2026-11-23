#pragma once

#include <memory>
#include <string>

#include "hsynth/json.h"

namespace hsynth {

class Engine;

// Control-thread API. Compilation and synchronous inference may block on a
// device driver. Only the resulting parameter writes reach the audio engine.
class NpuMorph {
public:
    static constexpr int kAnchors = 16;
    static constexpr int kMaxParams = 64;

    NpuMorph();
    ~NpuMorph();
    NpuMorph(const NpuMorph&) = delete;
    NpuMorph& operator=(const NpuMorph&) = delete;

    bool captureAnchor(const Engine& engine, int part, int slot, float x, float y, const std::string& name);
    Json loadPresetAnchor(int slot, float x, float y, const std::string& path, const std::string& libraryRoot);
    void removeAnchor(int slot);
    void clearAnchors();
    Json compile();
    Json status() const;
    Json anchorsJson() const;
    float evaluate(Engine& engine, int part, float x, float y);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hsynth
