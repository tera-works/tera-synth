#include <windows.h>
#include <mmsystem.h>

#include <cstdio>

#include "midi_in.h"

struct MidiIn::Impl {
    std::vector<HMIDIIN> handles;
};

namespace {
struct Sink : hsynth::EventSink {
    hsynth::Engine& engine;
    std::atomic<int>& program;
    std::atomic<int>& count;
    Sink(hsynth::Engine& e, std::atomic<int>& p, std::atomic<int>& c) : engine(e), program(p), count(c) {}
    void onEvent(const hsynth::Event& e) override {
        engine.sendEvent(e);
        count.fetch_add(1);
    }
    void onProgramChange(int, int p) override { program.store(p); }
};

void CALLBACK midiProc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    if (msg != MIM_DATA) return;  // SysEx (MIM_LONGDATA) is not needed yet
    const DWORD m = (DWORD)param1;
    uint8_t bytes[3] = {(uint8_t)(m & 0xFF), (uint8_t)((m >> 8) & 0xFF), (uint8_t)((m >> 16) & 0xFF)};
    const uint8_t status = bytes[0];
    size_t len = 3;
    if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 || status == 0xF1 || status == 0xF3) len = 2;
    else if (status >= 0xF4) len = 1;
    reinterpret_cast<MidiIn*>(instance)->onBytes(bytes, len);
}
}  // namespace

MidiIn::MidiIn(hsynth::Engine& engine) : impl_(std::make_unique<Impl>()), engine_(engine) {}

MidiIn::~MidiIn() { closeAll(); }

std::vector<std::string> MidiIn::openAll() {
    closeAll();
    std::vector<std::string> names;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSA caps{};
        if (midiInGetDevCapsA(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        HMIDIIN h = nullptr;
        if (midiInOpen(&h, i, (DWORD_PTR)&midiProc, (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            names.push_back(std::string(caps.szPname) + " (busy)");
            continue;
        }
        midiInStart(h);
        impl_->handles.push_back(h);
        names.push_back(caps.szPname);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    names_ = names;
    return names;
}

void MidiIn::closeAll() {
    for (HMIDIIN h : impl_->handles) {
        midiInStop(h);
        midiInReset(h);
        midiInClose(h);
    }
    impl_->handles.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    names_.clear();
}

std::vector<std::string> MidiIn::openPorts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return names_;
}

void MidiIn::setMpe(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    translator_.setMpeEnabled(on);
}

void MidiIn::setMultiTimbral(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    translator_.setMultiTimbral(on);
}

std::string MidiIn::lastMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
}

void MidiIn::onUmp(const uint32_t* words, size_t count) {
    if (count == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Sink sink(engine_, pendingProgram_, eventCount_);
    if (learn_) learn_->onUmp(words, count, engine_);
    translator_.processUmp(words, count, sink);
}

void MidiIn::onBytes(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    Sink sink(engine_, pendingProgram_, eventCount_);
    if (learn_) learn_->onMidi(data, len, engine_);  // learned CC -> parameter (the CC still reaches the engine)
    translator_.processMidi1(data, len, sink);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02X %02X %02X", data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0);
    last_ = buf;
}
