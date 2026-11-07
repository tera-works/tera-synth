// macOS / iOS MIDI input via CoreMIDI. Every source is connected to a port that
// uses the MIDI 2.0 protocol: CoreMIDI delivers Universal MIDI Packets (MIDI 2.0
// devices with their full resolution and per-note controllers, MIDI 1.0 devices
// converted), which go to the portable MidiTranslator through MidiIn::onUmp.
// The MIDI 1.0 byte path (readProc / onBytes) remains for older systems.
#include <CoreMIDI/CoreMIDI.h>

#include <cstdio>

#include "midi_in.h"

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

std::string endpointName(MIDIEndpointRef e) {
    CFStringRef s = nullptr;
    if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) != noErr || !s) return "MIDI";
    char buf[256];
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

// Length of the channel / system message starting with `status` (SysEx excluded).
size_t messageLength(uint8_t status) {
    if (status < 0xF0) {
        const uint8_t hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
    }
    switch (status) {
        case 0xF1: case 0xF3: return 2;
        case 0xF2: return 3;
        default: return 1;
    }
}
}  // namespace

struct MidiIn::Impl {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    std::vector<MIDIEndpointRef> sources;
    uint8_t running = 0;  // running status
};

namespace {
void readProc(const MIDIPacketList* list, void* refCon, void*) {
    auto* self = static_cast<MidiIn*>(refCon);
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 n = 0; n < list->numPackets; ++n) {
        size_t i = 0;
        uint8_t running = 0;
        while (i < p->length) {
            uint8_t b = p->data[i];
            if (b == 0xF0) {  // skip SysEx
                while (i < p->length && p->data[i] != 0xF7) ++i;
                ++i;
                continue;
            }
            uint8_t msg[3];
            size_t len;
            if (b & 0x80) {
                if (b < 0xF0) running = b;
                len = messageLength(b);
                msg[0] = b;
                ++i;
            } else {
                if (!running) { ++i; continue; }
                len = messageLength(running);
                msg[0] = running;
            }
            size_t k = 1;
            while (k < len && i < p->length) msg[k++] = p->data[i++];
            if (k == len) self->onBytes(msg, len);
        }
        p = MIDIPacketNext(p);
    }
}

void notifyProc(const MIDINotification*, void*) {}
}  // namespace

MidiIn::MidiIn(hsynth::Engine& engine) : impl_(std::make_unique<Impl>()), engine_(engine) {}
MidiIn::~MidiIn() {
    closeAll();
    if (impl_->port) MIDIPortDispose(impl_->port);
    if (impl_->client) MIDIClientDispose(impl_->client);
}

std::vector<std::string> MidiIn::openAll() {
    closeAll();
    if (!impl_->client) {
        if (MIDIClientCreate(CFSTR("HSynth"), notifyProc, this, &impl_->client) != noErr) return {};
        OSStatus st = -1;
        if (__builtin_available(macOS 11.0, iOS 14.0, *)) {
            st = MIDIInputPortCreateWithProtocol(impl_->client, CFSTR("HSynth In"), kMIDIProtocol_2_0, &impl_->port,
                                                 ^(const MIDIEventList* list, void*) {
                                                     const MIDIEventPacket* p = &list->packet[0];
                                                     for (UInt32 k = 0; k < list->numPackets; ++k) {
                                                         onUmp(p->words, p->wordCount);
                                                         p = MIDIEventPacketNext(p);
                                                     }
                                                 });
        }
        if (st != noErr && MIDIInputPortCreate(impl_->client, CFSTR("HSynth In"), readProc, this, &impl_->port) != noErr)
            return {};
    }
    std::vector<std::string> names;
    const ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef src = MIDIGetSource(i);
        if (!src) continue;
        if (MIDIPortConnectSource(impl_->port, src, nullptr) == noErr) {
            impl_->sources.push_back(src);
            names.push_back(endpointName(src));
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    names_ = names;
    return names;
}

void MidiIn::closeAll() {
    for (MIDIEndpointRef s : impl_->sources) MIDIPortDisconnectSource(impl_->port, s);
    impl_->sources.clear();
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
    char buf[40];
    std::snprintf(buf, sizeof buf, "UMP %08X%s", (unsigned)words[0], count > 1 ? " ..." : "");
    last_ = buf;
}

void MidiIn::onBytes(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    Sink sink(engine_, pendingProgram_, eventCount_);
    if (learn_) learn_->onMidi(data, len, engine_);
    translator_.processMidi1(data, len, sink);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02X %02X %02X", data[0], len > 1 ? data[1] : 0, len > 2 ? data[2] : 0);
    last_ = buf;
}
