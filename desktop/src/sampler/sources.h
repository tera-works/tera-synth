#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sampler_core.h"

namespace sampler {

std::unique_ptr<Source> makeSf2Source(const std::string& path, int sampleRate, std::string& error);
std::vector<std::string> listSf2Presets(const std::string& path);

// Live hardware / software synth: MIDI out + audio capture.
struct LiveConfig {
    std::string midiOut = "0";       // index or (part of) the port name
    int channel = 0;                 // 0-based channel for tones
    int drumChannel = 9;             // 0-based channel for rhythm kits
    std::string capture = "input";   // "loopback" (what the PC plays) or "input"
    std::string captureDevice;       // (part of) the recording device name; empty = default
    int sampleRate = 48000;          // requested; the device rate is used if it differs
    bool dry = true;                 // reverb (CC91) / chorus (CC93) sends to 0
    int volume = 100;                // CC7 sent on every program change
};
std::unique_ptr<Source> makeLiveSource(const LiveConfig& cfg, std::string& error);
std::vector<std::string> listMidiOutputs();
std::vector<std::string> listCaptureDevices();
// One-line description of an open live source (devices, rates, noise floor).
std::string describeLiveSource(const Source& s);
// Record `seconds` of whatever arrives at the input (no MIDI sent).
bool captureRaw(Source& s, double seconds, Recording& out);

}  // namespace sampler
