// Helpers shared by hosts (Android app, offline tools, tests). Blocking:
// call from a loader / UI thread, never from the audio callback.
#pragma once

#include <string>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/preset.h"

namespace hsynth {

// Part 0 + global: apply parameters and load the preset's sample sets (paths are relative to
// libraryRoot unless absolute). Returns false only if nothing could be applied;
// non-fatal problems are appended to warnings.
bool applyPreset(Engine& engine, const Preset& preset, const std::string& libraryRoot,
                 std::vector<std::string>& warnings);

// Snapshot the engine state into a preset. Sample paths are made relative to
// libraryRoot when they are inside it.
Preset capturePreset(const Engine& engine, const std::string& name, const std::string& libraryRoot);

// ---- multi-timbral (see engine.h) ----
// Part parameters + sample sets of `preset` into part N (0..15). The effect
// rack, impulses, master volume and limiter of the preset are ignored.
bool applyPartPreset(Engine& engine, int part, const Preset& preset, const std::string& libraryRoot,
                     std::vector<std::string>& warnings);
// Part N as a regular preset (global params / impulses are the current global ones).
Preset capturePartPreset(const Engine& engine, int part, const std::string& name, const std::string& libraryRoot);
// Switches the engine to multi-timbral mode and loads all 16 parts, their mix
// settings, the rack / master parameters and the impulses.
bool applyMulti(Engine& engine, const MultiPreset& multi, const std::string& libraryRoot,
                std::vector<std::string>& warnings);
MultiPreset captureMulti(const Engine& engine, const std::string& name, const std::string& libraryRoot);
bool loadPartSamples(Engine& engine, int part, int layer, const std::string& path, std::string& error,
                     std::vector<std::string>& warnings);
bool loadPartWavetable(Engine& engine, int part, int layer, const std::string& path, std::string& error);

// Load a WAV/SFZ file into a layer. Returns false on failure (error set);
// warnings receives non-fatal SFZ messages.
bool loadLayerSamples(Engine& engine, int layer, const std::string& path, std::string& error,
                      std::vector<std::string>& warnings);

// Load a WAV wavetable as the layer's "user" table (layer type WT).
bool loadLayerWavetable(Engine& engine, int layer, const std::string& path, std::string& error);

// Offline rendering ---------------------------------------------------------

struct ScheduledEvent {
    double time = 0;  // seconds
    Event event;
};

struct RenderJob {
    float sampleRate = 48000.0f;
    int blockSize = 128;
    double seconds = 2.0;
    std::vector<ScheduledEvent> events;  // must be sorted by time
};

// Renders job into left/right (resized). Events are quantised to block starts.
void renderOffline(Engine& engine, const RenderJob& job, std::vector<float>& left, std::vector<float>& right);

// Helper to schedule a note.
void addNote(RenderJob& job, double time, double duration, int key, float velocity, uint32_t id = 0);

}  // namespace hsynth
