// BLE MIDI packet codec (ble_midi.h).
#include <vector>

#include "hsynth/ble_midi.h"
#include "test.h"

using namespace hsynth;

namespace {
using Msgs = std::vector<std::vector<uint8_t>>;
Msgs decodeAll(ble::Decoder& d, const Msgs& packets) {
    Msgs out;
    for (const auto& p : packets)
        d.packet(p.data(), p.size(), [&](const uint8_t* m, size_t n) { out.emplace_back(m, m + n); });
    return out;
}
}  // namespace

TEST(ble_midi_short_messages_and_running_status) {
    ble::Decoder d;
    // header, ts, note on, running status without ts, ts + running status data, ts + CC
    const Msgs in = {{0x80, 0x81, 0x90, 60, 100, 62, 90, 0x82, 64, 80, 0x83, 0xB0, 1, 7, 0x83, 0xF8}};
    const Msgs out = decodeAll(d, in);
    CHECK(out.size() == 5);
    if (out.size() == 5) {
        CHECK((out[0] == std::vector<uint8_t>{0x90, 60, 100}));
        CHECK((out[1] == std::vector<uint8_t>{0x90, 62, 90}));
        CHECK((out[2] == std::vector<uint8_t>{0x90, 64, 80}));
        CHECK((out[3] == std::vector<uint8_t>{0xB0, 1, 7}));
        CHECK((out[4] == std::vector<uint8_t>{0xF8}));
    }
    // program change (one data byte) and channel pressure
    const Msgs out2 = decodeAll(d, {{0x80, 0x80, 0xC3, 5, 0x80, 0xD0, 99}});
    CHECK(out2.size() == 2 && out2[0].size() == 2 && out2[1][1] == 99);
    // not a BLE packet
    CHECK(decodeAll(d, {{0x10, 0x90, 60, 1}}).empty());
}

TEST(ble_midi_sysex_round_trip_over_packets) {
    std::vector<uint8_t> sx{0xF0, 0x7D};
    for (int i = 0; i < 700; ++i) sx.push_back((uint8_t)(i & 0x7F));
    sx.push_back(0xF7);
    for (size_t mtuPayload : {20u, 23u, 182u, 244u}) {
        const Msgs packets = ble::encode(sx.data(), sx.size(), mtuPayload, 12345);
        CHECK(packets.size() > 1);
        for (const auto& p : packets) CHECK(p.size() <= mtuPayload && (p[0] & 0x80));
        // a clock byte in the middle of the SysEx (ts + F8) must come out on its own
        Msgs withClock = packets;
        withClock[1].insert(withClock[1].begin() + 3, {0x81, 0xF8});
        ble::Decoder d;
        const Msgs out = decodeAll(d, withClock);
        CHECK(out.size() == 2);
        if (out.size() == 2) {
            CHECK((out[0] == std::vector<uint8_t>{0xF8}));
            CHECK(out[1] == sx);
        }
    }
    // short message encode -> decode
    const uint8_t on[3] = {0x92, 61, 33};
    ble::Decoder d;
    const Msgs out = decodeAll(d, ble::encode(on, 3, 20, 999));
    CHECK(out.size() == 1 && out[0].size() == 3 && out[0][0] == 0x92);
    // an aborted SysEx (a new status after a timestamp) is dropped, the message is kept
    const Msgs out3 = decodeAll(d, {{0x80, 0x80, 0xF0, 1, 2, 3}, {0x80, 4, 5, 0x81, 0x90, 60, 1}});
    CHECK(out3.size() == 1 && out3[0][0] == 0x90);
}
