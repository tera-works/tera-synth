// Standard MIDI File playback: parser (tempo map, running status, tracks),
// sample-accurate song playback in the parts, GM sound selection.
#include <cmath>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/gm_bank.h"
#include "hsynth/host_utils.h"
#include "hsynth/smf.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;

namespace {

struct SmfWriter {
    std::vector<uint8_t> bytes;
    void be16(int v) { bytes.push_back((uint8_t)(v >> 8)); bytes.push_back((uint8_t)v); }
    void be32(uint32_t v) { for (int s = 24; s >= 0; s -= 8) bytes.push_back((uint8_t)(v >> s)); }
    static void vlq(std::vector<uint8_t>& t, uint32_t v) {
        uint8_t buf[5];
        int n = 0;
        buf[n++] = v & 0x7F;
        while (v >>= 7) buf[n++] = (uint8_t)(0x80 | (v & 0x7F));
        while (n) t.push_back(buf[--n]);
    }
    void track(const std::vector<uint8_t>& t) {
        bytes.insert(bytes.end(), {'M', 'T', 'r', 'k'});
        be32((uint32_t)t.size());
        bytes.insert(bytes.end(), t.begin(), t.end());
    }
};

// format 1, 480 ppq: tempo track (120 bpm, 60 bpm from beat 2) + a note track
std::vector<uint8_t> testSmf() {
    SmfWriter w;
    w.bytes = {'M', 'T', 'h', 'd'};
    w.be32(6);
    w.be16(1);
    w.be16(3);
    w.be16(480);
    std::vector<uint8_t> t0;
    SmfWriter::vlq(t0, 0);
    t0.insert(t0.end(), {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});  // 500000 us = 120 bpm
    SmfWriter::vlq(t0, 960);
    t0.insert(t0.end(), {0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40});  // 1000000 us = 60 bpm
    SmfWriter::vlq(t0, 0);
    t0.insert(t0.end(), {0xFF, 0x2F, 0x00});
    w.track(t0);
    std::vector<uint8_t> t1;
    SmfWriter::vlq(t1, 0);
    t1.insert(t1.end(), {0xC0, 33});                // program: Electric Bass (finger)
    SmfWriter::vlq(t1, 480);
    t1.insert(t1.end(), {0x90, 69, 100});           // 0.5 s
    SmfWriter::vlq(t1, 240);
    t1.insert(t1.end(), {69, 0});                   // running status note-off at 0.75 s
    SmfWriter::vlq(t1, 720);
    t1.insert(t1.end(), {0x90, 72, 90});            // tick 1440: 1.0 s + 480 ticks at 60 bpm = 2.0 s
    SmfWriter::vlq(t1, 480);
    t1.insert(t1.end(), {0x80, 72, 0});             // 3.0 s
    SmfWriter::vlq(t1, 0);
    t1.insert(t1.end(), {0xFF, 0x2F, 0x00});
    w.track(t1);
    std::vector<uint8_t> t2;
    SmfWriter::vlq(t2, 960);
    t2.insert(t2.end(), {0x99, 36, 110});           // drum (channel 10) at 1.0 s
    SmfWriter::vlq(t2, 60);
    t2.insert(t2.end(), {0x89, 36, 0});
    SmfWriter::vlq(t2, 0);
    t2.insert(t2.end(), {0xFF, 0x2F, 0x00});
    w.track(t2);
    return w.bytes;
}

}  // namespace

TEST(song_smf_parser_tempo_map_and_channels) {
    const auto data = testSmf();
    Song s;
    std::string err;
    CHECK(parseSmf(data.data(), data.size(), s, err));
    INFO("song: %zu events, %.3f s, tempo %.1f\n", s.events.size(), s.length, s.initialTempo);
    CHECK_NEAR(s.initialTempo, 120.0, 0.01);
    CHECK(s.initialProgram[0] == 33 && s.initialProgram[1] == -1);
    CHECK(s.channelUsed[0] && s.channelUsed[9] && !s.channelUsed[1]);
    CHECK(s.noteCount == 3);
    CHECK_NEAR(s.length, 3.0, 1e-6);
    // note-on of key 72 lands at 2.0 s (tempo change at beat 2), drum at 1.0 s
    double t72 = -1, tDrum = -1;
    for (const auto& e : s.events) {
        if (e.event.type == EventType::NoteOn && e.event.key == 72) t72 = e.time;
        if (e.event.type == EventType::NoteOn && e.event.channel == 9) tDrum = e.time;
    }
    CHECK_NEAR(t72, 2.0, 1e-6);
    CHECK_NEAR(tDrum, 1.0, 1e-6);
    // garbage is rejected, never crashes
    const uint8_t junk[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 9, 1, 0xE0, 'M', 'T', 'r', 'k', 0xFF, 0xFF, 0xFF, 0xFF};
    Song j;
    (void)parseSmf(junk, sizeof junk, j, err);
}

TEST(song_playback_sample_accurate_in_parts) {
    const auto data = testSmf();
    auto song = std::make_unique<Song>();
    std::string err;
    CHECK(parseSmf(data.data(), data.size(), *song, err));
    for (int threads : {1, 4}) {
        Engine e;
        e.prepare(48000);
        e.setRenderThreads(threads);
        e.setMultiTimbral(true);
        for (int k : {0, 9}) {  // plain sines on the song's parts
            e.setPartParam(k, P::layer(0, P::VaWave1), (float)VaWave::Sine);
            e.setPartParam(k, P::layer(0, P::VaMix), 0.0f);
            e.setPartParam(k, P::FilterType, 0.0f);
            e.setPartParam(k, P::AmpAttack, 0.0005f);
            e.setPartParam(k, P::AmpSustain, 1.0f);
            e.setPartParam(k, P::AmpRelease, 0.005f);
            e.setPartParam(k, P::mod(0, P::ModAmt), 0.0f);
        }
        e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
        auto copy = std::make_unique<Song>(*song);
        e.setSong(std::move(copy));
        e.songPlay(true);
        std::vector<float> L(48000 * 4), R(L.size());
        for (size_t pos = 0; pos < L.size(); pos += 480) e.process(&L[pos], &R[pos], 480);
        size_t on = 0;
        while (on < L.size() && std::fabs(L[on]) < 0.01f) ++on;
        INFO("%d thread(s): first note at sample %zu (expected 24000), pitch %.1f Hz\n", threads, on,
             zeroCrossFreq(L, 48000, 26000, 34000));
        CHECK(std::abs((long)on - 24000) <= 16);
        CHECK_NEAR(zeroCrossFreq(L, 48000, 26000, 34000), 440.0, 2.0);
        CHECK(rms(L, 37000, 47000) < 1e-3);  // released at 0.75 s, drum at 1.0 s comes later
        CHECK(rms(L, 98000, 140000) > 0.01); // key 72 from 2.0 s (tempo map)
        CHECK(e.songPosition() > 3.9 && e.songPlaying());
        e.songPlay(false);
        e.process(&L[0], &R[0], 480);
        CHECK(!e.songPlaying());
    }
}

TEST(song_gm_bank_selection) {
    std::vector<PresetInfo> ps;
    auto add = [&](const char* name, const char* cat) {
        PresetInfo p;
        p.name = name;
        p.category = cat;
        p.file = std::string(name) + ".json";
        ps.push_back(p);
    };
    add("SC GM2 Capital 001 Piano 1", "SonicCell");
    add("SC GM2 Capital 026 Steel-str.Gt", "SonicCell");
    add("Finger Bass", "GM Test");
    add("String Ensemble", "GM Test");
    add("Standard Drum Kit", "GM Test");
    add("SC GM2 Rhythm Sets 001 GM2 STANDARD", "SonicCell");
    add("Square Lead", "VA");
    GmBank b;
    b.build(ps);
    CHECK(b.pick(0, false)->name == "SC GM2 Capital 001 Piano 1");
    CHECK(b.pick(27, false)->name == "SC GM2 Capital 026 Steel-str.Gt");  // clean guitar -> nearest guitar
    CHECK(b.pick(34, false)->name == "Finger Bass");                       // pick bass -> finger bass
    CHECK(b.pick(44, false)->name == "String Ensemble");                   // tremolo strings
    CHECK(b.pick(81, false)->name == "Square Lead");                       // saw lead -> square lead
    CHECK(b.pick(0, true)->name == "SC GM2 Rhythm Sets 001 GM2 STANDARD");
    CHECK(std::string(gmProgramName(73)) == "Flute");
    GmBank empty;
    CHECK(empty.pick(0, false) == nullptr);
}

TEST(song_meters_seek_chase_and_end) {
    auto song = std::make_unique<Song>();
    auto ctl = [](int cc, float v) {
        Event e;
        e.type = EventType::Controller;
        e.controller = (uint16_t)cc;
        e.value = v;
        e.channel = 0;
        return e;
    };
    Event on = Event::noteOn(5, 64, 0.9f), off = Event::noteOff(5, 64);
    on.channel = off.channel = 0;
    song->events = {{0.0, ctl(7, 0.25f)}, {0.0, ctl(91, 0.5f)}, {0.5, on}, {2.5, off}};
    song->channelUsed[0] = true;
    song->length = 2.5;
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    e.setSong(std::move(song));
    e.songSeek(1.0);  // past the controllers and the note-on
    e.songPlay(true);
    std::vector<float> L(480), R(480);
    for (int b = 0; b < 10; ++b) e.process(L.data(), R.data(), 480);
    Engine::PartMeter m[kNumParts];
    e.partMeters(m);
    INFO("after seek: volume %.2f reverb %.2f voices %d\n", m[0].volume, m[0].reverb, m[0].voices);
    CHECK_NEAR(m[0].volume, 0.25, 1e-6);  // chased
    CHECK_NEAR(m[0].reverb, 0.5, 1e-6);
    CHECK(m[0].voices == 0);              // the note-on before the seek point is not replayed
    // play from the start: the note sounds and shows on the keyboard
    e.songSeek(0.0);
    for (int b = 0; b < 70; ++b) e.process(L.data(), R.data(), 480);  // 0.7 s
    e.partMeters(m);
    CHECK((m[0].notes[2] >> 0) & 1u);     // key 64 = word 2, bit 0
    CHECK(m[0].level > 0.0f);
    const uint32_t ends = e.songEndCount();
    for (int b = 0; b < 600; ++b) e.process(L.data(), R.data(), 480);  // 6 s: end + 3 s
    CHECK(e.songEndCount() == ends + 1);
    CHECK(!e.songPlaying());
}

// bend ranges (RPN 0) and controllers come back after a seek and after a pause
TEST(song_chase_rpn_bend_range_and_resume) {
    auto song = std::make_unique<Song>();
    auto ctl = [](int cc, int v) {
        Event e = Event::control(EventType::Controller, v / 127.0f, cc);
        e.channel = 0;
        return e;
    };
    Event bend = Event::control(EventType::PitchBend, 0.5f);
    bend.channel = 0;
    song->events = {{0.0, ctl(101, 0)}, {0.0, ctl(100, 0)}, {0.0, ctl(6, 12)}, {0.0, ctl(7, 32)},
                    {0.1, ctl(101, 127)}, {0.1, ctl(100, 127)}, {0.2, bend}, {5.0, ctl(7, 100)}};
    song->channelUsed[0] = true;
    song->length = 5.0;
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    e.setSong(std::move(song));
    std::vector<float> L(480), R(480);
    auto check = [&](const char* what) {
        Engine::PartMeter m[kNumParts];
        e.partMeters(m);
        INFO("%s: volume %.3f\n", what, m[0].volume);
        CHECK_NEAR(m[0].volume, 32.0 / 127.0, 1e-4);
        CHECK_NEAR(e.debugPitchBendSemis(0), 6.0, 1e-4);  // half of 12 st
    };
    e.songSeek(1.0);
    e.songPlay(true);
    for (int b = 0; b < 5; ++b) e.process(L.data(), R.data(), 480);
    check("after seek");
    e.songPlay(false);  // pause: controllers reset
    for (int b = 0; b < 2; ++b) e.process(L.data(), R.data(), 480);
    CHECK_NEAR(e.debugPitchBendSemis(0), 0.0, 1e-4);
    e.songPlay(true);   // resume where it was
    for (int b = 0; b < 2; ++b) e.process(L.data(), R.data(), 480);
    check("after resume");
}
