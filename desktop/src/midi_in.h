// Desktop MIDI input. Windows: winmm (MIDI 1.0); macOS / iOS: CoreMIDI with the
// MIDI 2.0 protocol (Universal MIDI Packets; MIDI 1.0 devices are converted by
// CoreMIDI). Everything goes to the engine's portable MidiTranslator, like on Android.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/midi.h"
#include "hsynth/midi_learn.h"

class MidiIn {
public:
    explicit MidiIn(hsynth::Engine& engine);
    ~MidiIn();

    // (Re)open all available input ports. Returns the port names.
    std::vector<std::string> openAll();
    void closeAll();
    std::vector<std::string> openPorts() const;

    void setMpe(bool on);
    void setMultiTimbral(bool on);
    void setLearn(hsynth::MidiLearn* l) { learn_ = l; }
    int takeProgramChange() { return pendingProgram_.exchange(-1); }
    int eventCount() const { return eventCount_.load(); }
    std::string lastMessage() const;

    // Called from the driver thread with one complete MIDI message.
    void onBytes(const uint8_t* data, size_t len);
    // ... or with Universal MIDI Packets (words in host order).
    void onUmp(const uint32_t* words, size_t count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    hsynth::Engine& engine_;
    hsynth::MidiTranslator translator_;
    mutable std::mutex mutex_;
    std::atomic<int> pendingProgram_{-1};
    std::atomic<int> eventCount_{0};
    std::string last_;
    std::vector<std::string> names_;
    hsynth::MidiLearn* learn_ = nullptr;
};
