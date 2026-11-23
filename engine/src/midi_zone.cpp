#include "hsynth/midi_zone.h"
#include "hsynth/spsc_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace hsynth {
namespace {
constexpr int kSlots = 5;
constexpr int kDestinations = kSlots * 16;
constexpr int kQueue = 4096;
constexpr int kTracked = 1024;
constexpr int kPanicRoom = 2 * kDestinations;
int address(ZoneDestination d) { return d.slot * 16 + d.channel; }
uint32_t inputBit(uint8_t ch) { return 1u << (ch == kOmniChannel ? 16 : ch & 15); }
uint8_t wire(float value) {
    if (!std::isfinite(value)) return 0;
    return uint8_t(std::lround(127 * std::clamp(value, 0.0f, 1.0f)));
}
}  // namespace

struct ZoneNoteLedger::Impl {
    struct Message { uint8_t slot, size, data[3]; };
    struct Note {
        uint32_t id;
        uint8_t part, inputChannel, inputKey, output, outputKey;
        bool keyboard;
    };
    SpscQueue<Message> output{kQueue}, direct{kQueue};
    std::mutex hostWrite;
    std::timed_mutex hostRead;
    std::atomic<uint32_t> panic{0};
    Note notes[kTracked];
    int noteCount = 0, pedals = 0;
    uint32_t inputSustain = 0;
    uint16_t refs[kDestinations][128]{};
    uint32_t sustainSources[kDestinations]{};

    bool room(int events) const {
        return int(output.size()) + noteCount + pedals + kPanicRoom + events <= kQueue;
    }
    bool emit(int dest, uint8_t status, uint8_t a, uint8_t b = 0, uint8_t size = 3) {
        return output.push({uint8_t(dest / 16), size, {uint8_t(status | (dest & 15)), a, b}});
    }
    void forget(int i, uint8_t velocity) {
        const auto n = notes[i];
        auto& count = refs[n.output][n.outputKey];
        if (--count == 0) emit(n.output, 0x80, n.outputKey, velocity);
        notes[i] = notes[--noteCount];
    }
    void pedalDown(int dest, uint32_t bit) {
        auto& sources = sustainSources[dest];
        if (sources == 0) {
            if (!room(2) || !emit(dest, 0xB0, 64, 127)) return;
            ++pedals;
        }
        sources |= bit;
    }
    static bool matches(const Note& n, const Event& e) {
        if (e.noteId) return n.id == e.noteId &&
            (e.fromKeyboard ? n.keyboard : !n.keyboard && (e.channel == kOmniChannel || n.inputChannel == e.channel));
        return n.inputKey == e.key && (e.channel == kOmniChannel || n.inputChannel == e.channel);
    }
};

ZoneNoteLedger::ZoneNoteLedger() : impl_(std::make_unique<Impl>()) {}
ZoneNoteLedger::~ZoneNoteLedger() = default;

void ZoneNoteLedger::noteOn(int part, const Event& e, ZoneDestination dest, uint8_t key, float velocity) {
    auto& s = *impl_;
    if (dest.slot >= kSlots || dest.channel >= 16 || key >= 128) return;
    if (e.noteId) {
        for (int i = s.noteCount - 1; i >= 0; --i)
            if (s.notes[i].part == part && Impl::matches(s.notes[i], e)) s.forget(i, 0);
    }
    if (s.noteCount == kTracked || !s.room(2)) return;
    const int out = address(dest);
    const auto bit = inputBit(e.channel);
    if (s.inputSustain & bit) {
        s.pedalDown(out, bit);
        if (!s.room(2)) return;
    }
    auto& count = s.refs[out][key];
    if (count == 0 && !s.emit(out, 0x90, key, std::max(uint8_t(1), wire(velocity)))) return;
    ++count;
    s.notes[s.noteCount++] = {e.noteId, uint8_t(part), e.channel, e.key, uint8_t(out), key, e.fromKeyboard};
}

void ZoneNoteLedger::noteOff(const Event& e) {
    auto& s = *impl_;
    for (int i = s.noteCount - 1; i >= 0; --i)
        if (Impl::matches(s.notes[i], e)) s.forget(i, e.type == EventType::NoteOn ? 0 : wire(e.value));
}

void ZoneNoteLedger::sustain(uint8_t ch, const ZoneDestination* dests, int count, bool down) {
    auto& s = *impl_;
    const auto bit = inputBit(ch);
    if (!down) {
        s.inputSustain &= ~bit;
        if (s.pedals == 0) return;
        for (int i = 0; i < kDestinations; ++i) {
            auto& sources = s.sustainSources[i];
            if (!(sources & bit)) continue;
            sources &= ~bit;
            if (sources == 0) {
                s.emit(i, 0xB0, 64, 0);
                --s.pedals;
            }
        }
        return;
    }
    // Include still-held notes that a prior scene routed elsewhere.
    s.inputSustain |= bit;
    for (int i = 0; i < count; ++i) s.pedalDown(address(dests[i]), bit);
    for (int i = 0; i < s.noteCount; ++i)
        if (s.notes[i].inputChannel == ch) s.pedalDown(s.notes[i].output, bit);
}

void ZoneNoteLedger::channelPanic(uint8_t ch) {
    auto& s = *impl_;
    if (ch != kOmniChannel) {
        sustain(ch, nullptr, 0, false);
        for (int i = s.noteCount - 1; i >= 0; --i)
            if (s.notes[i].inputChannel == ch) s.forget(i, 0);
        return;
    }
    for (int i = 0; i < kDestinations; ++i)
        if (s.sustainSources[i] != 0) {
            s.emit(i, 0xB0, 64, 0);
            s.sustainSources[i] = 0;
        }
    s.pedals = 0;
    s.inputSustain = 0;
    while (s.noteCount) s.forget(s.noteCount - 1, 0);
}

void ZoneNoteLedger::control(const Event& e, const ZoneDestination* destinations, int n) {
    auto& s = *impl_;
    bool sent[kDestinations]{};
    for (int i = 0; i < n && s.room(1); ++i) {
        const auto dest = address(destinations[i]);
        if (sent[dest]) continue;
        sent[dest] = true;
        switch (e.type) {
            case EventType::PitchBend: {
                const int bend = std::clamp(int(std::lround(e.value * 8192)) + 8192, 0, 16383);
                s.emit(dest, 0xE0, bend & 127, bend >> 7);
                break;
            }
            case EventType::ModWheel: s.emit(dest, 0xB0, 1, wire(e.value)); break;
            case EventType::Controller:
                if (e.controller < 120) s.emit(dest, 0xB0, uint8_t(e.controller), wire(e.value));
                break;
            case EventType::ChannelPressure: s.emit(dest, 0xD0, wire(e.value), 0, 2); break;
            default: break;
        }
    }
}

bool ZoneNoteLedger::program(ZoneDestination dest, int msb, int lsb, int program) {
    auto& s = *impl_;
    if (!s.room((msb >= 0) + (lsb >= 0) + (program >= 0))) return false;
    const int d = address(dest);
    if (msb >= 0) s.emit(d, 0xB0, 0, uint8_t(msb));
    if (lsb >= 0) s.emit(d, 0xB0, 32, uint8_t(lsb));
    if (program >= 0) s.emit(d, 0xC0, uint8_t(program), 0, 2);
    return true;
}

void ZoneNoteLedger::service() {
    auto& s = *impl_;
    if (s.panic.load(std::memory_order_relaxed) == 0) return;
    const auto slots = s.panic.exchange(0);
    for (int slot = 0; slot < kSlots; ++slot) {
        if (!(slots & (1u << slot))) continue;
        if (int(s.output.size()) + s.noteCount + s.pedals + 32 > kQueue) {
            s.panic.fetch_or(1u << slot);
            continue;
        }
        for (int i = s.noteCount - 1; i >= 0; --i)
            if (s.notes[i].output / 16 == slot) s.forget(i, 0);
        for (int ch = 0; ch < 16; ++ch) {
            const int d = slot * 16 + ch;
            if (s.sustainSources[d]) --s.pedals;
            s.sustainSources[d] = 0;
            s.emit(d, 0xB0, 64, 0);
            s.emit(d, 0xB0, 123, 0);
        }
    }
}

void ZoneNoteLedger::requestPanic(int slot) {
    if (slot >= 0 && slot < kSlots) impl_->panic.fetch_or(1u << slot);
}
void ZoneNoteLedger::send(int slot, const uint8_t* data, size_t size) {
    if (slot < 0 || slot >= kSlots || !data || size == 0 || size > kQueue * 3) return;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.hostWrite);
    if (s.direct.size() + (size + 2) / 3 > kQueue) return;
    for (size_t i = 0; i < size; i += 3) {
        const auto count = std::min(size_t(3), size - i);
        Impl::Message message{uint8_t(slot), uint8_t(count), {}};
        std::copy_n(data + i, count, message.data);
        s.direct.push(message);
    }
}

std::vector<uint8_t> ZoneNoteLedger::next(int timeoutMs) {
    auto& s = *impl_;
    std::vector<uint8_t> bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    std::unique_lock<std::timed_mutex> lock(s.hostRead, std::defer_lock);
    if (!(timeoutMs > 0 ? lock.try_lock_until(deadline) : lock.try_lock())) return bytes;
    for (;;) {
        Impl::Message m{};
        for (int i = 0; i < 2*kQueue; ++i) {
            if (!s.output.pop(m) && !s.direct.pop(m)) break;
            bytes.push_back(m.slot);
            bytes.push_back(m.size);
            bytes.insert(bytes.end(), m.data, m.data + m.size);
        }
        if (!bytes.empty() || std::chrono::steady_clock::now() >= deadline) return bytes;
        std::this_thread::sleep_until(std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
    }
}

}  // namespace hsynth
