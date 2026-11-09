#include <windows.h>
#include <mmsystem.h>

#include <cstdio>

#include "midi_in.h"

namespace {
std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 1 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace

namespace {
// One open input: SysEx arrives in buffers handed to the driver (MIM_LONGDATA).
struct Port {
    static constexpr int kBuffers = 8;
    static constexpr DWORD kBufferBytes = 1024;
    MidiIn* self = nullptr;
    HMIDIIN handle = nullptr;
    std::string name;
    MIDIHDR hdr[kBuffers] = {};
    char data[kBuffers][kBufferBytes] = {};
    std::vector<uint8_t> sysex;  // the message being received
    std::atomic<bool> closing{false};
};
}  // namespace

struct MidiIn::Impl {
    std::vector<std::unique_ptr<Port>> ports;
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

void CALLBACK midiProc(HMIDIIN h, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    auto* port = reinterpret_cast<Port*>(instance);
    if (msg == MIM_LONGDATA) {  // a SysEx buffer is full / the message ended
        auto* hdr = reinterpret_cast<MIDIHDR*>(param1);
        const auto* b = reinterpret_cast<const uint8_t*>(hdr->lpData);
        for (DWORD i = 0; i < hdr->dwBytesRecorded; ++i) {
            const uint8_t c = b[i];
            if (c == 0xF0) port->sysex.clear();
            if (c == 0xF0 || !port->sysex.empty()) port->sysex.push_back(c);
            if (c == 0xF7 && !port->sysex.empty()) {
                port->self->onSysEx(port->sysex.data(), port->sysex.size(), port->name);
                port->sysex.clear();
            }
            if (port->sysex.size() > (1u << 20)) port->sysex.clear();  // runaway message
        }
        if (!port->closing.load()) midiInAddBuffer(h, hdr, sizeof(MIDIHDR));  // give the buffer back
        return;
    }
    if (msg != MIM_DATA) return;
    const DWORD m = (DWORD)param1;
    uint8_t bytes[3] = {(uint8_t)(m & 0xFF), (uint8_t)((m >> 8) & 0xFF), (uint8_t)((m >> 16) & 0xFF)};
    const uint8_t status = bytes[0];
    size_t len = 3;
    if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 || status == 0xF1 || status == 0xF3) len = 2;
    else if (status >= 0xF4) len = 1;
    port->self->onBytes(bytes, len);
}
}  // namespace

MidiIn::MidiIn(hsynth::Engine& engine) : impl_(std::make_unique<Impl>()), engine_(engine) {}

MidiIn::~MidiIn() { closeAll(); }

std::vector<std::string> MidiIn::openAll() {
    closeAll();
    std::vector<std::string> names;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        auto port = std::make_unique<Port>();
        port->self = this;
        port->name = narrow(caps.szPname);
        HMIDIIN h = nullptr;
        if (midiInOpen(&h, i, (DWORD_PTR)&midiProc, (DWORD_PTR)port.get(), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            names.push_back(port->name + " (busy)");
            continue;
        }
        port->handle = h;
        for (int k = 0; k < Port::kBuffers; ++k) {
            MIDIHDR& hdr = port->hdr[k];
            hdr.lpData = port->data[k];
            hdr.dwBufferLength = Port::kBufferBytes;
            if (midiInPrepareHeader(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR) midiInAddBuffer(h, &hdr, sizeof hdr);
        }
        midiInStart(h);
        names.push_back(port->name);
        impl_->ports.push_back(std::move(port));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    names_ = names;
    return names;
}

void MidiIn::closeAll() {
    for (auto& p : impl_->ports) {
        p->closing.store(true);
        midiInStop(p->handle);
        midiInReset(p->handle);  // returns the SysEx buffers (MIM_LONGDATA, not re-added)
        for (int k = 0; k < Port::kBuffers; ++k) midiInUnprepareHeader(p->handle, &p->hdr[k], sizeof(MIDIHDR));
        midiInClose(p->handle);
    }
    impl_->ports.clear();
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

void MidiIn::setUmpInput(bool) {}
std::string MidiIn::status() const { return ""; }
bool MidiIn::rescanIfChanged() { return false; }  // winmm: the Rescan button

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

void MidiIn::setSysExHandler(SysExHandler h) {
    std::lock_guard<std::mutex> lock(sysexMutex_);
    sysex_ = std::move(h);
}

bool MidiIn::sendVirtual(const uint8_t*, size_t) { return false; }  // winmm has no virtual ports (use loopMIDI)

void MidiIn::onSysEx(const uint8_t* data, size_t len, const std::string& replyPort) {
    std::lock_guard<std::mutex> lock(sysexMutex_);
    if (sysex_) sysex_(data, len, replyPort);
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
