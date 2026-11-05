// Multi-timbral operation: 16 parts, channel routing, per-part mix, voice
// stealing across parts, multi presets, single-part compatibility.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/midi.h"
#include "hsynth/preset.h"
#include "hsynth/wav.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;
namespace fs = std::filesystem;

namespace {

Event onCh(Event e, int ch) {
    e.channel = (uint8_t)ch;
    return e;
}

// Plain sine on layer A of a part, no filter, no rack reverb.
void sinePart(Engine& e, int part) {
    e.setPartParam(part, P::layer(0, P::VaWave1), (float)VaWave::Sine);
    e.setPartParam(part, P::layer(0, P::VaMix), 0.0f);
    e.setPartParam(part, P::FilterType, (float)FilterType::Off);
    e.setPartParam(part, P::mod(0, P::ModAmt), 0.0f);
    e.setPartParam(part, P::AmpSustain, 1.0f);
    e.setPartParam(part, P::AmpRelease, 0.01f);
}

Engine* makeEngine(bool multi) {
    auto* e = new Engine;
    e->prepare(48000);
    e->setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e->setMultiTimbral(multi);
    return e;
}

void run(Engine& e, double seconds, std::vector<float>* L = nullptr, std::vector<float>* R = nullptr) {
    const int n = (int)(seconds * 48000);
    std::vector<float> l((size_t)n), r((size_t)n);
    for (int off = 0; off < n; off += 128) {
        const int m = std::min(128, n - off);
        e.process(l.data() + off, r.data() + off, m);
    }
    if (L) *L = std::move(l);
    if (R) *R = std::move(r);
}

struct EngineSink : EventSink {
    Engine& e;
    explicit EngineSink(Engine& eng) : e(eng) {}
    void onEvent(const Event& ev) override { e.sendEvent(ev); }
};

std::string tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / "hsynth_tests" / name;
    fs::create_directories(p);
    return p.generic_string();
}

}  // namespace

TEST(multi_channel_routing) {
    std::unique_ptr<Engine> e(makeEngine(true));
    for (int k = 0; k < 3; ++k) sinePart(*e, k);
    e->setPartParam(1, P::layer(0, P::LCoarse), 12.0f);   // part 2 (ch 2): one octave up
    e->setPartParam(2, P::layer(0, P::LCoarse), -12.0f);  // part 3 (ch 3): one octave down
    MidiTranslator t;
    t.setMultiTimbral(true);
    EngineSink sink(*e);
    const uint8_t on2[] = {0x91, 69, 100};
    t.processMidi1(on2, sizeof on2, sink);
    std::vector<float> L;
    run(*e, 0.5, &L);
    CHECK(e->partActiveVoices(1) == 1 && e->partActiveVoices(0) == 0 && e->partActiveVoices(2) == 0);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 24000), 880.0, 2.0);
    const uint8_t off2[] = {0x81, 69, 0, 0x92, 69, 100};
    t.processMidi1(off2, sizeof off2, sink);
    run(*e, 0.5, &L);
    CHECK(e->partActiveVoices(1) == 0 && e->partActiveVoices(2) == 1);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 24000), 220.0, 1.0);
    // events without a channel go to the selected part
    e->sendEvent(Event::control(EventType::AllSoundOff, 0));
    e->selectPart(1);
    e->sendEvent(Event::noteOn(7, 69, 1.0f));
    run(*e, 0.5, &L);
    CHECK(e->partActiveVoices(1) == 1 && e->partActiveVoices(2) == 0);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 24000), 880.0, 2.0);
    // channel-wide controls stay on their channel: pitch bend on ch 2 only
    e->setPartParam(1, P::BendRange, 12.0f);
    e->sendEvent(onCh(Event::control(EventType::PitchBend, -1.0f), 0));
    run(*e, 0.3, &L);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 14000), 880.0, 2.0);
    e->sendEvent(onCh(Event::control(EventType::PitchBend, -1.0f), 1));
    run(*e, 0.3, &L);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 14000), 440.0, 1.0);
    // two parts on the same channel layer
    e->sendEvent(Event::control(EventType::AllSoundOff, 0));
    e->setPartMix(2, Mix::Channel, 1);
    e->sendEvent(onCh(Event::noteOn(9, 60, 1.0f), 0));
    run(*e, 0.1);
    CHECK(e->partActiveVoices(0) == 1 && e->partActiveVoices(2) == 1);
    e->sendEvent(onCh(Event::noteOff(9, 60), 0));
    run(*e, 0.2);
    CHECK(e->stats().activeVoices == 0);

    // single-part mode ignores the channel: everything plays part 1
    std::unique_ptr<Engine> s(makeEngine(false));
    sinePart(*s, 0);
    s->setPartParam(1, P::layer(0, P::LCoarse), 12.0f);
    EngineSink ssink(*s);
    MidiTranslator st;
    st.processMidi1(on2, sizeof on2, ssink);
    run(*s, 0.5, &L);
    CHECK(s->partActiveVoices(0) == 1);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 24000), 440.0, 1.0);
}

TEST(multi_translator_mpe_off_and_channels) {
    struct Collect : EventSink {
        std::vector<Event> ev;
        void onEvent(const Event& e) override { ev.push_back(e); }
    } c;
    MidiTranslator t;
    t.setMpeEnabled(true);
    t.setMultiTimbral(true);
    const uint8_t data[] = {0x92, 60, 100, 0xE2, 0x00, 0x50, 0xB2, 74, 127, 0xD2, 64};
    t.processMidi1(data, sizeof data, c);
    CHECK(c.ev.size() == 4);
    for (auto& e : c.ev) CHECK(e.channel == 2);
    CHECK(c.ev[1].type == EventType::PitchBend);      // not per-note: MPE is off in multi mode
    CHECK(c.ev[2].type == EventType::Controller && c.ev[2].controller == 74);
    CHECK(c.ev[3].type == EventType::ChannelPressure);
    // MIDI 2.0 UMP carries the channel too
    c.ev.clear();
    const uint32_t ump[] = {0x40953C00u, 0x80000000u};
    t.processUmp(ump, 2, c);
    CHECK(c.ev.size() == 1 && c.ev[0].channel == 5);
}

TEST(multi_per_part_params_and_samples) {
    const std::string dir = tempDir("multi_samples");
    std::vector<float> wav(48000);
    for (size_t i = 0; i < wav.size(); ++i) wav[i] = 0.5f * std::sin(2.0f * kPi * 440.0f * (float)i / 48000.0f);
    std::string err;
    CHECK(writeWav(dir + "/a440.wav", wav, nullptr, 48000, false, err));
    std::unique_ptr<Engine> e(makeEngine(true));
    // part 1: PCM layer without samples (silent), part 4: same patch with samples
    for (int k : {0, 3}) {
        e->setPartParam(k, P::layer(0, P::LType), (float)LayerType::PCM);
        e->setPartParam(k, P::FilterType, (float)FilterType::Off);
        e->setPartParam(k, P::AmpSustain, 1.0f);
    }
    std::vector<std::string> warnings;
    CHECK(loadPartSamples(*e, 3, 0, dir + "/a440.wav", err, warnings));
    CHECK(e->partSampleSetName(3, 0) == "a440");
    CHECK(e->sampleSetName(0).empty() && e->partSampleSetName(0, 0).empty());
    // parameters are independent per part; global ids are shared
    e->setPartParam(5, P::FilterCutoff, 345.0f);
    CHECK(e->getPartParam(5, P::FilterCutoff) == 345.0f);
    CHECK(e->getParam(P::FilterCutoff) != 345.0f && e->getPartParam(4, P::FilterCutoff) != 345.0f);
    e->setPartParam(7, P::MasterVolume, 0.5f);  // master belongs to the global section
    CHECK(e->getParam(P::MasterVolume) == 0.5f && e->getPartParam(3, P::MasterVolume) == 0.5f);
    e->setParam(P::MasterVolume, 0.8f);
    e->setPartParam(9, P::fxp(fx::Delay, fx::delay::Mix), 0.33f);
    CHECK(e->getParam(P::fxp(fx::Delay, fx::delay::Mix)) == 0.33f);

    std::vector<float> L;
    e->sendEvent(onCh(Event::noteOn(1, 60, 1.0f), 0));
    run(*e, 0.3, &L);
    CHECK(rms(L, 2400, 14000) < 1e-6);  // part 1 has no samples
    e->sendEvent(onCh(Event::noteOn(2, 60, 1.0f), 3));  // root key of the WAV
    run(*e, 0.3, &L);
    CHECK(rms(L, 2400, 14000) > 0.01);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 2400, 14000), 440.0, 2.0);
    // swapping part 4's samples only stops part 4's PCM playback
    sinePart(*e, 5);
    e->sendEvent(onCh(Event::noteOn(3, 69, 1.0f), 5));
    run(*e, 0.1);
    e->setPartSampleSet(3, 0, nullptr);
    run(*e, 0.3, &L);
    CHECK(e->partSampleSetName(3, 0).empty());
    CHECK(e->partActiveVoices(5) == 1);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 14000), 440.0, 1.0);  // part 6's A4 keeps playing
}

TEST(multi_mute_volume_pan) {
    auto level = [](float volume, float pan, bool mute, int cc7, int cc10, double& l, double& r) {
        std::unique_ptr<Engine> e(makeEngine(true));
        sinePart(*e, 2);
        e->setPartMix(2, Mix::Volume, volume);
        e->setPartMix(2, Mix::Pan, pan);
        e->setPartMix(2, Mix::Mute, mute ? 1.0f : 0.0f);
        if (cc7 >= 0) e->sendEvent(onCh(Event::control(EventType::Controller, cc7 / 127.0f, 7), 2));
        if (cc10 >= 0) e->sendEvent(onCh(Event::control(EventType::Controller, cc10 / 127.0f, 10), 2));
        e->sendEvent(onCh(Event::noteOn(1, 69, 1.0f), 2));
        std::vector<float> L, R;
        run(*e, 0.5, &L, &R);
        l = rms(L, 4800, 24000);
        r = rms(R, 4800, 24000);
        return e->partActiveVoices(2);
    };
    double l0, r0, l, r;
    CHECK(level(1.0f, 0.0f, false, -1, -1, l0, r0) == 1);
    CHECK(l0 > 0.01 && std::fabs(l0 - r0) < 1e-6);
    level(0.5f, 0.0f, false, -1, -1, l, r);
    CHECK_NEAR(l / l0, 0.25, 0.005);  // volume taper: gain = volume^2
    level(1.0f, -1.0f, false, -1, -1, l, r);
    CHECK_NEAR(l / l0, 1.0, 0.005);
    CHECK(r < 1e-6);
    level(1.0f, 0.5f, false, -1, -1, l, r);
    CHECK_NEAR(l / l0, 0.5, 0.005);
    CHECK_NEAR(r / r0, 1.0, 0.005);
    CHECK(level(1.0f, 0.0f, true, -1, -1, l, r) == 0);  // muted parts ignore note-ons
    CHECK(l < 1e-9);
    level(1.0f, 0.0f, false, 64, 0, l, r);               // CC7 (squared) and CC10 hard left
    CHECK_NEAR(l / l0, (64.0 / 127) * (64.0 / 127), 0.005);
    CHECK(r < 1e-6);
}

TEST(multi_key_range_and_transpose) {
    std::unique_ptr<Engine> e(makeEngine(true));
    // keyboard split on channel 1: part 1 below C4, part 2 from C4 up, +12 st
    sinePart(*e, 0);
    sinePart(*e, 1);
    e->setPartMix(0, Mix::KeyHigh, 59);
    e->setPartMix(1, Mix::Channel, 1);
    e->setPartMix(1, Mix::KeyLow, 60);
    e->setPartMix(1, Mix::Transpose, 12);
    e->sendEvent(onCh(Event::noteOn(1, 57, 1.0f), 0));
    std::vector<float> L;
    run(*e, 0.4, &L);
    CHECK(e->partActiveVoices(0) == 1 && e->partActiveVoices(1) == 0);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 19000), 220.0, 1.0);
    // key-matched note-off (no note id) must find the transposed voice
    e->sendEvent(onCh(Event::noteOff(0, 57), 0));
    e->sendEvent(onCh(Event::noteOn(0, 69, 1.0f), 0));
    run(*e, 0.4, &L);
    CHECK(e->partActiveVoices(0) == 0 && e->partActiveVoices(1) == 1);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 19000), 880.0, 2.0);
    e->sendEvent(onCh(Event::perNote(EventType::NotePitch, 0, 69, 12.0f), 0));
    run(*e, 0.4, &L);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 19000), 1760.0, 4.0);
    e->sendEvent(onCh(Event::noteOff(0, 69), 0));
    run(*e, 0.2);
    CHECK(e->stats().activeVoices == 0);
}

TEST(multi_voice_stealing_fair_across_parts) {
    std::unique_ptr<Engine> e(makeEngine(true));
    e->setMaxVoices(16);
    for (int k = 0; k < 3; ++k) e->setPartParam(k, P::AmpSustain, 1.0f);
    uint32_t id = 1;
    for (int i = 0; i < 16; ++i) e->sendEvent(onCh(Event::noteOn(id++, 40 + i, 0.8f), 0));
    run(*e, 0.1);
    CHECK(e->partActiveVoices(0) == 16);
    for (int i = 0; i < 4; ++i) e->sendEvent(onCh(Event::noteOn(id++, 60 + i, 0.8f), 1));
    run(*e, 0.1);
    CHECK(e->partActiveVoices(0) == 12 && e->partActiveVoices(1) == 4);
    // the busy part steals from itself, the small part keeps its notes
    for (int i = 0; i < 4; ++i) e->sendEvent(onCh(Event::noteOn(id++, 70 + i, 0.8f), 0));
    run(*e, 0.1);
    CHECK(e->partActiveVoices(0) == 12 && e->partActiveVoices(1) == 4);
    for (int i = 0; i < 8; ++i) e->sendEvent(onCh(Event::noteOn(id++, 50 + i, 0.8f), 2));
    run(*e, 0.1);
    INFO("parts after 8 notes on part 3: %d %d %d\n", e->partActiveVoices(0), e->partActiveVoices(1),
         e->partActiveVoices(2));
    CHECK(e->partActiveVoices(1) == 4);
    CHECK(e->partActiveVoices(0) == 6 && e->partActiveVoices(2) == 6);
    CHECK(e->stats().activeVoices == 16);
}

TEST(multi_fx_send_dry_bypasses_rack) {
    auto tail = [](float send) {
        std::unique_ptr<Engine> e(makeEngine(true));
        sinePart(*e, 0);
        e->setParam(P::fxp(fx::Delay, fx::delay::On), 1.0f);
        e->setParam(P::fxp(fx::Delay, fx::delay::Mix), 0.5f);
        e->setParam(P::fxp(fx::Delay, fx::delay::Feedback), 0.5f);
        e->setPartMix(0, Mix::FxSend, send);
        e->sendEvent(onCh(Event::noteOn(1, 69, 1.0f), 0));
        run(*e, 0.05);
        e->sendEvent(onCh(Event::noteOff(1, 69), 0));
        std::vector<float> L;
        run(*e, 1.5, &L);
        return rms(L, 4800, L.size());  // 0.1 s after the note-off
    };
    const double wet = tail(1.0f), dry = tail(0.0f);
    INFO("delay tail rms: send 1 = %g, send 0 = %g\n", wet, dry);
    CHECK(wet > 1e-3);
    CHECK(dry < 1e-6);
}

// Multi mode with only part 1 in use must sound exactly like single-part mode,
// and single-part mode must not apply the mix settings.
TEST(multi_single_part_mode_identical) {
    auto render = [](bool multi, bool oddMix) {
        std::unique_ptr<Engine> e(makeEngine(multi));
        e->setParam(P::fxp(fx::Reverb, fx::reverb::On), 1.0f);
        e->setParam(P::layer(1, P::LType), (float)LayerType::FM);
        e->setParam(P::Lfo2Wave, (float)LfoWave::SampleHold);
        e->setParam(P::mod(4, P::ModSrc), (float)ModSource::Lfo2);
        e->setParam(P::mod(4, P::ModDst), (float)ModDest::Cutoff);
        e->setParam(P::mod(4, P::ModAmt), 0.3f);
        e->setMaxVoices(4);
        if (oddMix) {
            e->setPartMix(0, Mix::Volume, 0.1f);
            e->setPartMix(0, Mix::Transpose, 7);
            e->setPartMix(0, Mix::FxSend, 0.0f);
            e->setPartMix(0, Mix::Channel, 9);
        }
        RenderJob job;
        job.seconds = 2.0;
        for (int i = 0; i < 10; ++i) addNote(job, 0.1 * i, 0.5, 48 + 3 * i, 0.5f + 0.05f * (float)i);
        job.events.push_back({0.3, Event::control(EventType::ModWheel, 0.7f)});
        job.events.push_back({0.4, Event::control(EventType::Sustain, 1.0f)});
        job.events.push_back({1.2, Event::control(EventType::Sustain, 0.0f)});
        std::stable_sort(job.events.begin(), job.events.end(),
                         [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.time < b.time; });
        if (multi) for (auto& ev : job.events) ev.event.channel = 0;
        std::vector<float> L, R;
        renderOffline(*e, job, L, R);
        L.insert(L.end(), R.begin(), R.end());
        return L;
    };
    const auto single = render(false, false);
    const auto singleOdd = render(false, true);
    const auto multi = render(true, false);
    double d1 = 0, d2 = 0;
    for (size_t i = 0; i < single.size(); ++i) {
        d1 = std::max(d1, (double)std::fabs(single[i] - singleOdd[i]));
        d2 = std::max(d2, (double)std::fabs(single[i] - multi[i]));
    }
    INFO("max diff single vs single+mix %g, single vs multi(part 1) %g\n", d1, d2);
    CHECK(rms(single) > 0.01);
    CHECK(d1 == 0.0);
    CHECK(d2 == 0.0);
}

TEST(multi_preset_json_roundtrip) {
    const std::string dir = tempDir("multi_preset");
    std::string err;
    std::vector<float> wav(4800, 0.25f);
    CHECK(writeWav(dir + "/s.wav", wav, nullptr, 48000, false, err));
    MultiPreset m;
    m.name = "Band";
    m.comment = "test";
    m.selectedPart = 9;
    m.global.params[(size_t)P::MasterVolume] = 0.55f;
    m.global.params[(size_t)P::fxp(fx::Delay, fx::delay::Mix)] = 0.42f;
    m.global.params[(size_t)P::FilterCutoff] = 999.0f;  // part param in the global section: not stored
    for (int k = 0; k < kNumParts; ++k) {
        m.parts[k].name = "Part " + std::to_string(k + 1);
        m.parts[k].params[(size_t)P::FilterCutoff] = 200.0f + 50.0f * (float)k;
        m.parts[k].params[(size_t)P::layer(1, P::LType)] = (float)(k % 3);  // off / va / fm
        m.mix[k][Mix::Volume] = 0.05f * (float)k;
        m.mix[k][Mix::Pan] = -1.0f + 0.1f * (float)k;
        m.mix[k][Mix::Mute] = (float)(k == 3);
        m.mix[k][Mix::Channel] = (float)(16 - k);
        m.mix[k][Mix::KeyLow] = (float)k;
        m.mix[k][Mix::KeyHigh] = (float)(100 + k);
        m.mix[k][Mix::Transpose] = (float)(k - 8);
        m.mix[k][Mix::FxSend] = (float)k / 15.0f;
    }
    m.parts[2].params[(size_t)P::layer(0, P::LType)] = (float)LayerType::PCM;
    m.parts[2].samples[0] = "s.wav";
    CHECK(saveMultiFile(dir + "/m.json", m, err));
    MultiPreset q;
    CHECK(loadMultiFile(dir + "/m.json", q, err));
    CHECK(q.warnings.empty());
    CHECK(q.name == "Band" && q.comment == "test" && q.selectedPart == 9);
    CHECK(q.global.params[(size_t)P::MasterVolume] == 0.55f);
    CHECK_NEAR(q.global.params[(size_t)P::fxp(fx::Delay, fx::delay::Mix)], 0.42f, 1e-6);
    CHECK(q.global.params[(size_t)P::FilterCutoff] == paramTable()[(size_t)P::FilterCutoff].def);
    bool same = true;
    for (int k = 0; k < kNumParts; ++k) {
        same = same && q.parts[k].name == m.parts[k].name;
        for (int i = 0; i < Mix::Count; ++i) same = same && std::fabs(q.mix[k][i] - m.mix[k][i]) < 1e-5f;
        for (int i = 0; i < P::kNumParams; ++i)
            if (isPartParam(i)) same = same && std::fabs(q.parts[k].params[(size_t)i] - m.parts[k].params[(size_t)i]) < 1e-3f;
    }
    CHECK(same);
    CHECK(q.parts[2].samples[0] == "s.wav");

    // apply to an engine and capture it back
    Engine e;
    e.prepare(48000);
    std::vector<std::string> warnings;
    CHECK(applyMulti(e, q, dir, warnings));
    for (auto& w : warnings) INFO("warning: %s\n", w.c_str());
    CHECK(warnings.empty());
    CHECK(e.multiTimbral() && e.selectedPart() == 9);
    CHECK(e.getPartParam(7, P::FilterCutoff) == 550.0f);
    CHECK(e.getPartMix(4, Mix::Channel) == 12.0f);
    CHECK(e.partSampleSetName(2, 0) == "s");
    CHECK(e.getParam(P::MasterVolume) == 0.55f);
    const MultiPreset c = captureMulti(e, "cap", dir);
    CHECK(c.parts[2].samples[0] == "s.wav" && c.parts[7].params[(size_t)P::FilterCutoff] == 550.0f);
    CHECK(multiToJson(c).find("\"cap\"") != std::string::npos);
    MultiPreset r;
    CHECK(multiFromJson(multiToJson(c), r, err));
    CHECK(r.mix[15][Mix::Transpose] == 7.0f);

    // a single preset loads into one part without touching the rack
    Preset p;
    p.params[(size_t)P::FilterCutoff] = 777.0f;
    p.params[(size_t)P::fxp(fx::Delay, fx::delay::Mix)] = 0.9f;
    CHECK(applyPartPreset(e, 5, p, dir, warnings));
    CHECK(e.getPartParam(5, P::FilterCutoff) == 777.0f);
    CHECK_NEAR(e.getParam(P::fxp(fx::Delay, fx::delay::Mix)), 0.42f, 1e-6);
    const Preset cp = capturePartPreset(e, 5, "p6", dir);
    CHECK(cp.params[(size_t)P::FilterCutoff] == 777.0f);

    // errors / tolerance
    CHECK(!multiFromJson(R"({"format":"hsynth-preset","params":{}})", r, err));
    CHECK(multiFromJson(R"({"format":"hsynth-multi","parts":[{"mix":{"bogus":1,"volume":0.5}}]})", r, err));
    CHECK(r.warnings.size() == 1 && r.mix[0][Mix::Volume] == 0.5f && r.mix[1][Mix::Channel] == 2.0f);
}

TEST(multi_mode_switch_and_stress) {
    std::unique_ptr<Engine> e(makeEngine(true));
    Rng rng(42);
    uint32_t id = 1;
    std::vector<float> L(128), R(128);
    for (int b = 0; b < 2000; ++b) {
        const int ch = (int)(rng.unipolar() * 16);
        e->sendEvent(onCh(Event::noteOn(id++, 30 + (int)(rng.unipolar() * 60), 0.7f), ch));
        if (b % 3 == 0) e->sendEvent(onCh(Event::noteOff(id - 2, 0), ch));
        if (b % 50 == 0) e->setPartMix((int)(rng.unipolar() * 16), Mix::Pan, rng.bipolar());
        if (b % 70 == 0) e->setPartParam((int)(rng.unipolar() * 16), P::FilterCutoff, 100.0f + 5000.0f * rng.unipolar());
        if (b == 1000) e->setMultiTimbral(false);
        if (b == 1500) e->setMultiTimbral(true);
        e->process(L.data(), R.data(), 128);
        if (b == 1001) {
            for (int k = 1; k < kNumParts; ++k) CHECK(e->partActiveVoices(k) == 0);
        }
        for (int i = 0; i < 128; ++i) CHECK(std::isfinite(L[(size_t)i]) && std::fabs(L[(size_t)i]) < 4.0f);
    }
    // a host buffer larger than kMaxBlockSize in multi mode
    std::vector<float> big(3000), bigR(3000);
    e->process(big.data(), bigR.data(), 3000);
    CHECK(allFinite(big));
}

// ---------------------------------------------------------------- part inserts

namespace {
void ringInsert(Engine& e, int part, float freq) {
    e.setPartParam(part, P::insSlot(1), (float)(fx::insertIndex(fx::RingMod) + 1));
    e.setPartParam(part, P::insp(fx::RingMod, fx::ringmod::Freq), freq);
    e.setPartParam(part, P::insp(fx::RingMod, fx::ringmod::Mix), 1.0f);
    e.setPartParam(part, P::insp(fx::RingMod, fx::ringmod::Spread), 0.0f);
}
}  // namespace

TEST(part_insert_metadata) {
    CHECK(findParam("ins.slot1") == P::insSlot(0));
    CHECK(findParam("ins.chorus.mix") == P::insp(fx::Chorus, fx::chorus::Mix));
    CHECK(paramTable()[(size_t)P::insp(fx::Comp, 0)].def == 1.0f);
    CHECK(isPartParam(P::insp(fx::Stereo, fx::stereo::Output)) && !isPartParam(P::fxSlot(0)));
    CHECK(paramTable()[(size_t)P::insSlot(2)].options.size() == (size_t)fx::kNumInsertBlocks + 1);
    CHECK(fx::insertIndex(fx::Delay) < 0);
}

TEST(part_insert_single_mode) {
    // ring modulator insert on the single patch: 440 Hz * 300 Hz -> 140 / 740 Hz
    std::unique_ptr<Engine> e(makeEngine(false));
    sinePart(*e, 0);
    ringInsert(*e, 0, 300.0f);
    e->sendEvent(Event::noteOn(1, 69, 1.0f));
    std::vector<float> L;
    run(*e, 0.6, &L);
    const double s140 = goertzel(L, 48000, 140, 9600, 19200), s740 = goertzel(L, 48000, 740, 9600, 19200),
                 s440 = goertzel(L, 48000, 440, 9600, 19200);
    INFO("ring insert: 140 %g 740 %g 440 %g\n", s140, s740, s440);
    CHECK(s140 > 10 * s440 && s740 > 10 * s440);
    // removing the insert returns the clean sine (crossfade, no stale state)
    e->setPartParam(0, P::insSlot(1), 0.0f);
    run(*e, 0.3, &L);
    CHECK(goertzel(L, 48000, 440, 4800, 9600) > 10 * goertzel(L, 48000, 740, 4800, 9600));
}

TEST(part_insert_only_affects_its_part) {
    std::unique_ptr<Engine> e(makeEngine(true));
    sinePart(*e, 0);
    sinePart(*e, 1);
    ringInsert(*e, 0, 300.0f);
    e->setPartMix(0, Mix::FxSend, 0.0f);  // inserts run before the dry/send split
    e->sendEvent(onCh(Event::noteOn(1, 69, 1.0f), 0));  // part 1: 440 Hz ring-modulated
    e->sendEvent(onCh(Event::noteOn(2, 81, 1.0f), 1));  // part 2: clean 880 Hz
    std::vector<float> L;
    run(*e, 0.6, &L);
    const double s880 = goertzel(L, 48000, 880, 9600, 19200);
    const double s580 = goertzel(L, 48000, 580, 9600, 19200), s1180 = goertzel(L, 48000, 1180, 9600, 19200);
    const double s740 = goertzel(L, 48000, 740, 9600, 19200), s440 = goertzel(L, 48000, 440, 9600, 19200);
    INFO("part 2 clean 880 %g, its would-be sidebands 580 %g 1180 %g; part 1 sideband 740 %g, carrier 440 %g\n", s880,
         s580, s1180, s740, s440);
    CHECK(s880 > 0.05);
    CHECK(s580 < s880 * 0.01 && s1180 < s880 * 0.01);
    CHECK(s740 > 10 * s440);
    // part 1's insert is part of its preset
    const Preset p = capturePartPreset(*e, 0, "ring", "");
    CHECK(p.params[(size_t)P::insSlot(1)] == (float)(fx::insertIndex(fx::RingMod) + 1));
    const Preset q = capturePartPreset(*e, 1, "clean", "");
    CHECK(q.params[(size_t)P::insSlot(1)] == 0.0f);
}

TEST(part_insert_all_blocks_finite) {
    for (int k = 0; k < fx::kNumInsertBlocks; ++k) {
        std::unique_ptr<Engine> e(makeEngine(true));
        const int part = k % kNumParts;
        e->setPartParam(part, P::insSlot(0), (float)(k + 1));
        e->setPartParam(part, P::insSlot(2), (float)((k + 3) % fx::kNumInsertBlocks + 1));
        e->setQuality(Quality::High);
        for (int i = 0; i < 6; ++i) e->sendEvent(onCh(Event::noteOn((uint32_t)(i + 1), 40 + 5 * i, 0.9f), part));
        std::vector<float> L, R;
        run(*e, 0.5, &L, &R);
        CHECK(allFinite(L) && allFinite(R) && peak(L) < 2.0);
        CHECK(rms(L) > 1e-3);
        e->sendEvent(onCh(Event::control(EventType::AllNotesOff, 0), part));
        run(*e, 3.0, &L, &R);
        CHECK(rms(L, L.size() - 4800, L.size()) < 1e-4);  // tails end
    }
}

// Random parameters of random parts (inserts, arps, voice modes, FM 6-op,
// filters, mix) while notes arrive on random channels: output stays finite
// and limited, nothing hangs.
TEST(multi_random_part_parameter_stress) {
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    const auto& table = paramTable();
    Rng r(1234);
    std::vector<float> L(128), R(128);
    bool finite = true;
    float maxPeak = 0;
    uint32_t id = 1;
    for (int b = 0; b < 48000 * 12 / 128; ++b) {  // 12 s
        for (int k = 0; k < 6; ++k) {
            const int part = (int)(r.next() % kNumParts);
            const int pid = (int)(r.next() % (uint32_t)P::kPartParams);
            if (!isPartParam(pid)) continue;
            const ParamInfo& pi = table[(size_t)pid];
            e.setPartParam(part, pid, pi.min + r.unipolar() * (pi.max - pi.min));
        }
        if (b % 13 == 0) {
            const int part = (int)(r.next() % kNumParts), mid = (int)(r.next() % Mix::Count);
            const ParamInfo& mi = partMixTable()[(size_t)mid];
            e.setPartMix(part, mid, mi.min + r.unipolar() * (mi.max - mi.min));
        }
        if (b % 400 == 399) e.setMultiTimbral(r.unipolar() < 0.8f);
        if (b % 5 == 0) {
            Event on = Event::noteOn(id++, 24 + (int)(r.next() % 80), r.unipolar());
            on.channel = (uint8_t)(r.next() % 16);
            e.sendEvent(on);
        }
        if (b % 4 == 0) {
            Event off = Event::noteOff(id - 1 - (r.next() % 6), 0);
            off.channel = (uint8_t)(r.next() % 16);
            e.sendEvent(off);
        }
        e.process(L.data(), R.data(), 128);
        for (int i = 0; i < 128; ++i) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
            maxPeak = std::max(maxPeak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
    }
    CHECK(finite);
    CHECK(maxPeak <= 1.0f);
    e.sendEvent(Event::control(EventType::AllSoundOff, 0));
    for (int b = 0; b < 10; ++b) e.process(L.data(), R.data(), 128);
    CHECK(e.stats().activeVoices == 0);
}

// Thread contract: a UI thread edits parts / mix / modes and reads state back,
// a loader thread swaps part sample sets, while the audio thread renders.
// Meant to be run under ThreadSanitizer as well.
TEST(multi_api_concurrent_threads) {
    Engine e;
    e.prepare(48000);
    e.setMultiTimbral(true);
    for (int k = 0; k < 4; ++k) e.setPartParam(k, P::layer(1, P::LType), (float)LayerType::PCM);
    auto makeSet = [](float freq) {
        auto set = std::make_unique<SampleSet>();
        auto s = std::make_unique<Sample>();
        s->frames = 2400;
        s->data[0].resize(2400 + Sample::kPadFrames);
        for (int i = 0; i < 2400; ++i) s->data[0][(size_t)i] = 0.4f * std::sin(2.0f * kPi * freq * (float)i / 48000.0f);
        Region r;
        r.sample = s.get();
        r.loopMode = LoopMode::Continuous;
        r.loopStart = 0;
        r.loopEnd = 2400;
        set->regions.push_back(r);
        set->samples.push_back(std::move(s));
        return set;
    };
    std::atomic<bool> stop{false};
    std::thread ui([&] {
        Rng r(5);
        int i = 0;
        std::vector<float> all((size_t)P::kNumParams);
        while (!stop.load()) {
            const int part = (int)(r.next() % 4);
            e.setPartParam(part, P::FilterCutoff, 200.0f + 4000.0f * r.unipolar());
            e.setPartMix(part, Mix::Pan, r.bipolar());
            e.setPartParam(part, P::ArpOn, (i % 97) < 30 ? 1.0f : 0.0f);
            e.setParam(P::fxp(fx::Delay, fx::delay::Mix), r.unipolar() * 0.3f);
            if (i % 50 == 0) e.selectPart(part);
            if (i % 200 == 0) e.setMultiTimbral(i % 400 == 0);
            e.getPartParams(part, all.data());
            (void)e.getPartMix(part, Mix::Volume);
            (void)e.stats();
            (void)e.partActiveVoices(part);
            (void)e.partSampleSetName(part, 1);
            if (i % 300 == 0) (void)captureMulti(e, "snap", "");
            ++i;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    std::thread loader([&] {
        int i = 0;
        while (!stop.load()) {
            e.setPartSampleSet(i % 4, 1, makeSet(200.0f + (float)(i % 7) * 30.0f));
            e.collectGarbage();
            ++i;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    std::vector<float> L(64), R(64);
    bool finite = true;
    uint32_t id = 1;
    for (int b = 0; b < 15000; ++b) {
        if (b % 20 == 0) {
            Event on = Event::noteOn(id++, 48 + (b / 20) % 24, 0.8f);
            on.channel = (uint8_t)((b / 20) % 4);
            e.sendEvent(on);
        }
        if (b % 20 == 10) {
            Event off = Event::noteOff(id - 1, 0);
            off.channel = (uint8_t)(((b - 10) / 20) % 4);
            e.sendEvent(off);
        }
        e.process(L.data(), R.data(), 64);
        for (float v : L) if (!std::isfinite(v)) finite = false;
    }
    stop = true;
    ui.join();
    loader.join();
    e.collectGarbage();
    CHECK(finite);
}
