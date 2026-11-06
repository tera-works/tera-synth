// Desktop MIDI input. Windows: winmm (MIDI 1.0). Raw bytes are fed to the
// engine's portable MidiTranslator, exactly like the Android host does.
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
