#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "hsynth/event.h"

namespace hsynth {

struct ZoneDestination {
    uint8_t slot = 0, channel = 0;
};

class ZoneNoteLedger {
public:
    ZoneNoteLedger();
    ~ZoneNoteLedger();

    // Audio thread: sustain and release follow the original input and output
    // addresses, independently of the current multi configuration.
    void noteOn(int part, const Event& input, ZoneDestination dest, uint8_t key, float velocity);
    void noteOff(const Event& input);
    void sustain(uint8_t inputChannel, const ZoneDestination* destinations, int count, bool down);
    void channelPanic(uint8_t inputChannel);
    void control(const Event& e, const ZoneDestination* destinations, int count);
    bool program(ZoneDestination dest, int bankMsb, int bankLsb, int program);
    void service();

    // Host threads. Drain is serialized internally. Raw data is emitted in
    // contiguous chunks of up to three bytes, without reinterpreting it.
    void requestPanic(int slot);
    void send(int slot, const uint8_t* bytes, size_t size);
    std::vector<uint8_t> next(int timeoutMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hsynth
