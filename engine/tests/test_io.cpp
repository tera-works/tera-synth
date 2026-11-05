#include <cstdio>
#include <filesystem>
#include <fstream>

#include "analysis.h"
#include "hsynth/engine.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/midi.h"
#include "hsynth/preset.h"
#include "hsynth/wav.h"
#include "test.h"

using namespace hsynth;
using namespace hsynth_test;
namespace fs = std::filesystem;

namespace {

std::string tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / "hsynth_tests" / name;
    fs::create_directories(p);
    return p.generic_string();
}

std::vector<float> sine(float freq, int frames, float sr = 48000, float amp = 0.5f) {
    std::vector<float> v((size_t)frames);
    for (int i = 0; i < frames; ++i) v[(size_t)i] = amp * std::sin(2.0f * kPi * freq * (float)i / sr);
    return v;
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

struct Collector : EventSink {
    std::vector<Event> events;
    std::vector<Event> clocks;  // MIDI clock / transport, kept apart
    std::vector<int> programs;
    void onEvent(const Event& e) override { (e.type == EventType::Clock ? clocks : events).push_back(e); }
    void onProgramChange(int, int p) override { programs.push_back(p); }
};

}  // namespace

TEST(wav_roundtrip) {
    const std::string dir = tempDir("wav");
    std::string err;
    auto l = sine(440, 4800), r = sine(660, 4800);
    CHECK(writeWav(dir + "/s16.wav", l, &r, 44100, false, err, 64, 100, 3999));
    CHECK(writeWav(dir + "/f32.wav", l, nullptr, 48000, true, err));
    WavData w;
    CHECK(readWav(dir + "/s16.wav", w, err));
    CHECK(w.channels == 2 && w.frames == 4800 && w.sampleRate == 44100);
    CHECK(w.unityNote == 64 && w.loopStart == 100 && w.loopEnd == 3999);
    CHECK_NEAR(w.left[123], l[123], 1.0 / 16384);
    CHECK_NEAR(w.right[321], r[321], 1.0 / 16384);
    CHECK(readWav(dir + "/f32.wav", w, err));
    CHECK(w.channels == 1);
    CHECK(w.left[77] == l[77]);
    CHECK(!readWav(dir + "/missing.wav", w, err));
    writeText(dir + "/bad.wav", "RIFFxxxxWAVEjunk");
    CHECK(!readWav(dir + "/bad.wav", w, err));
}

TEST(sfz_parse_and_mapping) {
    const std::string dir = tempDir("sfz");
    fs::create_directories(dir + "/samples");
    std::string err;
    CHECK(writeWav(dir + "/samples/low.wav", sine(261.63f, 9600), nullptr, 48000, false, err));
    CHECK(writeWav(dir + "/samples/high soft.wav", sine(523.25f, 9600), nullptr, 48000, false, err));
    CHECK(writeWav(dir + "/samples/high loud.wav", sine(523.25f, 9600, 48000, 0.9f), nullptr, 48000, false, err));
    writeText(dir + "/test.sfz",
              "// test instrument\n"
              "#define $VOL -3\n"
              "<control> default_path=samples/\n"
              "<global> ampeg_release=0.3 volume=$VOL\n"
              "<group> lokey=0 hikey=b4 cutoff=1000 fileg_attack=0.1 /* unsupported */\n"
              "<region> sample=low.wav pitch_keycenter=c4\n"
              "<group> lokey=c5 hikey=127 pitch_keycenter=72\n"
              "<region> sample=high soft.wav hivel=80\n"
              "<region> sample=high loud.wav lovel=81 loop_mode=loop_continuous loop_start=0 loop_end=9599\n"
              "<region> sample=missing.wav key=100\n"
              "<curve> foo=1\n");
    auto set = loadSampleSet(dir + "/test.sfz", err);
    CHECK(set != nullptr);
    if (!set) { INFO("%s\n", err.c_str()); return; }
    CHECK(set->regions.size() == 3);
    CHECK(set->samples.size() == 3);
    bool warnedCutoff = false, warnedMissing = false, warnedCurve = false;
    for (auto& w : set->warnings) {
        if (w.find("fileg_attack") != std::string::npos) warnedCutoff = true;
        if (w.find("missing.wav") != std::string::npos) warnedMissing = true;
        if (w.find("curve") != std::string::npos) warnedCurve = true;
    }
    CHECK(warnedCutoff && warnedMissing && warnedCurve);
    const Region* out[4];
    CHECK(set->findRegions(60, 100, 0.5f, out, 4) == 1 && out[0]->keycenter == 60);
    CHECK_NEAR(out[0]->volumeDb, -3.0, 1e-6);
    CHECK(out[0]->filter == RegionFilter::Lp2 && out[0]->cutoff == 1000.0f);
    CHECK(out[0]->ampeg.enabled && std::fabs(out[0]->ampeg.release - 0.3f) < 1e-6f);
    CHECK(set->findRegions(71, 100, 0.5f, out, 4) == 1);  // b4 = 71
    CHECK(set->findRegions(72, 60, 0.5f, out, 4) == 1 && out[0]->sample->path.find("soft") != std::string::npos);
    CHECK(set->findRegions(72, 100, 0.5f, out, 4) == 1 && out[0]->loopMode == LoopMode::Continuous);
    CHECK(out[0]->loopEnd == 9600);
}

TEST(pcm_pitch_and_loop_playback) {
    const std::string dir = tempDir("pcm");
    std::string err;
    // 1 kHz sine at 44.1 kHz, 0.1 s, looped over whole cycles (441 frames = 10 cycles)
    std::vector<float> s((size_t)4410);
    for (size_t i = 0; i < s.size(); ++i) s[i] = 0.5f * std::sin(2.0f * kPi * 1000.0f * (float)i / 44100.0f);
    CHECK(writeWav(dir + "/tone.wav", s, nullptr, 44100, true, err, 60, 0, 4409));
    Engine e;
    e.prepare(48000);
    e.setParam(P::layer(0, P::LType), (float)LayerType::PCM);
    e.setParam(P::FilterType, (float)FilterType::Off);
    e.setParam(P::fxp(fx::Reverb, fx::reverb::On), 0.0f);
    e.setParam(P::AmpSustain, 1.0f);
    std::vector<std::string> warnings;
    CHECK(loadLayerSamples(e, 0, dir + "/tone.wav", err, warnings));
    RenderJob job;
    job.seconds = 1.0;
    job.events.push_back({0.0, Event::noteOn(1, 72, 1.0f)});  // one octave up
    std::vector<float> L, R;
    renderOffline(e, job, L, R);
    CHECK_NEAR(zeroCrossFreq(L, 48000, 4800, 9600), 2000.0, 5.0);
    // the sample is only 0.1 s long; looping must keep it sounding (at 2x speed 0.05 s)
    CHECK(rms(L, 40000, 48000) > 0.1);
}

TEST(preset_roundtrip) {
    const std::string dir = tempDir("preset");
    Preset p;
    p.name = "Round Trip";
    p.category = "Test";
    p.samples[2] = "factory/piano/piano.sfz";
    p.params[(size_t)P::layer(1, P::LType)] = (float)LayerType::FM;
    p.params[(size_t)P::FilterCutoff] = 1234.5f;
    p.params[(size_t)P::fmOp(1, 2, P::OpRatio)] = 7;
    p.params[(size_t)P::fxp(fx::Delay, fx::delay::Mode)] = 0;
    std::string err;
    CHECK(savePresetFile(dir + "/p.json", p, err));
    Preset q;
    CHECK(loadPresetFile(dir + "/p.json", q, err));
    CHECK(q.name == "Round Trip" && q.category == "Test");
    CHECK(q.samples[2] == p.samples[2]);
    bool same = true;
    for (int i = 0; i < P::kNumParams; ++i)
        if (std::fabs(q.params[(size_t)i] - p.params[(size_t)i]) > 1e-4f * std::max(1.0f, std::fabs(p.params[(size_t)i]))) {
            INFO("param %s differs %f vs %f\n", paramTable()[(size_t)i].name.c_str(), q.params[(size_t)i], p.params[(size_t)i]);
            same = false;
        }
    CHECK(same);
    CHECK(q.warnings.empty());

    Preset r;
    CHECK(presetFromJson(R"({"name":"x","params":{"bogus.param":1,"a.type":"fm","filter.reso":5}})", r, err));
    CHECK(r.params[(size_t)P::layer(0, P::LType)] == (float)LayerType::FM);
    CHECK(r.params[(size_t)P::FilterReso] == 1.0f);
    CHECK(r.warnings.size() == 2);
    CHECK(!presetFromJson("{\"params\": [1,2", r, err));
}

TEST(preset_apply_to_engine) {
    const std::string dir = tempDir("apply");
    std::string err;
    CHECK(writeWav(dir + "/s.wav", sine(440, 4800), nullptr, 48000, false, err));
    Preset p;
    p.params[(size_t)P::layer(0, P::LType)] = (float)LayerType::PCM;
    p.samples[0] = "s.wav";
    p.samples[1] = "does/not/exist.sfz";
    Engine e;
    e.prepare(48000);
    std::vector<std::string> warnings;
    CHECK(applyPreset(e, p, dir, warnings));
    CHECK(e.sampleSetName(0) == "s");
    CHECK(warnings.size() == 1);
    Preset c = capturePreset(e, "cap", dir);
    CHECK(c.samples[0] == "s.wav");
    CHECK(c.params[(size_t)P::layer(0, P::LType)] == (float)LayerType::PCM);
}

TEST(json_basics) {
    Json j;
    std::string err;
    CHECK(Json::parse(R"({"a":[1,2.5,-3e2],"b":"xé\n","c":true,"d":null})", j, err));
    CHECK(j.find("a")->size() == 3 && j.find("a")->at(2).number() == -300);
    CHECK(j.find("b")->string() == "x\xc3\xa9\n");
    Json k;
    CHECK(Json::parse(j.dump(), k, err));
    CHECK(k.dump(0) == j.dump(0));
    CHECK(!Json::parse("{\"a\":1,}", k, err));
    CHECK(!Json::parse("[1 2]", k, err));
}

TEST(param_metadata_consistency) {
    const auto& t = paramTable();
    CHECK((int)t.size() == P::kNumParams);
    for (int i = 0; i < P::kNumParams; ++i) {
        CHECK(!t[(size_t)i].name.empty());
        CHECK(findParam(t[(size_t)i].name) == i);
        CHECK(t[(size_t)i].def >= t[(size_t)i].min && t[(size_t)i].def <= t[(size_t)i].max);
    }
    Json j;
    std::string err;
    CHECK(Json::parse(paramMetadataJson(), j, err));
    CHECK((int)j.size() == P::kNumParams);
}

TEST(midi1_translation) {
    MidiTranslator t;
    Collector c;
    const uint8_t data[] = {0x90, 60, 100, 64, 0,           // note on, running status note-off (vel 0)
                            0xF0, 0x7E, 0x01, 0xF7,         // sysex skipped
                            0xF8,                           // clock (separate event)
                            0xE0, 0x00, 0x60,               // pitch bend up
                            0xB0, 64, 127, 0xC0, 5,         // sustain, program
                            0xA0, 61, 64};                  // poly pressure
    t.processMidi1(data, sizeof data, c);
    CHECK(c.events.size() == 5);
    CHECK(c.clocks.size() == 1 && c.clocks[0].controller == (uint16_t)ClockMsg::Tick);
    CHECK(c.events[0].type == EventType::NoteOn && c.events[0].key == 60);
    CHECK(c.events[1].type == EventType::NoteOff && c.events[1].key == 64);
    CHECK(c.events[2].type == EventType::PitchBend && c.events[2].value > 0.4f);
    CHECK(c.events[3].type == EventType::Sustain && c.events[3].value == 1.0f);
    CHECK(c.programs.size() == 1 && c.programs[0] == 5);
    CHECK(c.events[4].type == EventType::NotePressure && c.events[4].noteId == MidiTranslator::noteId(0, 0, 61));
    // split across calls
    Collector d;
    const uint8_t p1[] = {0x91, 62}, p2[] = {90};
    t.processMidi1(p1, 2, d);
    t.processMidi1(p2, 1, d);
    CHECK(d.events.size() == 1 && d.events[0].key == 62);
}

TEST(mpe_translation) {
    MidiTranslator t;
    t.setMpeEnabled(true);
    Collector c;
    const uint8_t data[] = {0x92, 60, 100, 0xE2, 0x00, 0x50, 0xB2, 74, 127, 0xD2, 64, 0xE0, 0x00, 0x40};
    t.processMidi1(data, sizeof data, c);
    CHECK(c.events.size() == 5);
    CHECK(c.events[1].type == EventType::NotePitch && c.events[1].noteId == c.events[0].noteId);
    CHECK(c.events[1].value > 0 && c.events[1].value < 48);
    CHECK(c.events[2].type == EventType::NoteTimbre && c.events[2].value == 1.0f);
    CHECK(c.events[3].type == EventType::NotePressure);
    CHECK(c.events[4].type == EventType::PitchBend);  // master channel stays global
}

TEST(ump_translation) {
    MidiTranslator t;
    Collector c;
    const uint32_t words[] = {
        0x40903C00u, 0x80000000u,  // MIDI2 note on ch0 note 60, vel 0x8000
        0x40603C00u, 0xC0000000u,  // per-note pitch bend +0.5
        0x40003C4Au, 0xFFFFFFFFu,  // registered per-note ctrl 74 (timbre)
        0x20B00140u,               // MIDI1-in-UMP CC1 = 64
        0x10F80000u,               // system real-time: clock tick
        0x40803C00u, 0x00000000u,  // note off
    };
    t.processUmp(words, sizeof words / 4, c);
    CHECK(c.events.size() == 5);
    CHECK(c.clocks.size() == 1);
    CHECK(c.events[0].type == EventType::NoteOn && c.events[0].key == 60 && std::fabs(c.events[0].value - 0.5f) < 0.01f);
    CHECK(c.events[1].type == EventType::NotePitch && std::fabs(c.events[1].value - 24.0f) < 0.01f);
    CHECK(c.events[2].type == EventType::NoteTimbre && c.events[2].value > 0.99f);
    CHECK(c.events[3].type == EventType::ModWheel);
    CHECK(c.events[4].type == EventType::NoteOff);
    // byte stream form (big endian)
    Collector d;
    const uint8_t bytes[] = {0x40, 0x90, 0x3C, 0x00, 0xFF, 0xFF, 0x00, 0x00};
    t.processUmpBytes(bytes, 3, d);
    t.processUmpBytes(bytes + 3, 5, d);
    CHECK(d.events.size() == 1 && d.events[0].value == 1.0f);
}

// Every shipped factory preset / multi (android/app/src/main/assets, generated
// by hsynth_assets) loads without warnings and renders a finite, audible chord.
TEST(factory_presets_render) {
    const fs::path assets = fs::path(HSYNTH_TEST_DATA) / ".." / ".." / ".." / "android" / "app" / "src" / "main" / "assets";
    if (!fs::exists(assets / "presets")) { INFO("no assets folder, skipped\n"); return; }
    const std::string lib = (assets / "library").generic_string();
    int count = 0;
    for (const auto& ent : fs::directory_iterator(assets / "presets")) {
        if (ent.path().extension() != ".json") continue;
        Preset p;
        std::string err;
        CHECK(loadPresetFile(ent.path().generic_string(), p, err));
        Engine e;
        e.prepare(48000);
        std::vector<std::string> warnings;
        CHECK(applyPreset(e, p, lib, warnings));
        for (auto& w : warnings) INFO("%s: %s\n", ent.path().filename().string().c_str(), w.c_str());
        CHECK(warnings.empty());
        RenderJob job;
        job.seconds = 1.5;
        for (int k : {36, 42, 48, 55, 60, 64}) addNote(job, 0.05, 1.0, k, 0.8f);  // 36 / 42: drum kit keys
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        CHECK(allFinite(L) && allFinite(R));
        const bool needsInput = p.name.find("Vocoder") != std::string::npos;
        if (!needsInput && rms(L, 4800, 48000) < 1e-3) INFO("silent preset: %s\n", p.name.c_str());
        CHECK(needsInput || rms(L, 4800, 48000) > 1e-3);
        CHECK(peak(L) < 1.5);
        ++count;
    }
    INFO("%d factory presets rendered\n", count);
    CHECK(count >= 36);
    for (const auto& ent : fs::directory_iterator(assets / "multis")) {
        MultiPreset m;
        std::string err;
        CHECK(loadMultiFile(ent.path().generic_string(), m, err));
        CHECK(m.warnings.empty());
        Engine e;
        e.prepare(48000);
        std::vector<std::string> warnings;
        CHECK(applyMulti(e, m, lib, warnings));
        CHECK(warnings.empty());
        RenderJob job;
        job.seconds = 1.0;
        const int channels[] = {0, 1, 2, 3, 9};
        uint32_t id = 1;
        for (int ch : channels) {
            Event on = Event::noteOn(id, ch == 9 ? 36 : 60, 0.8f), off = Event::noteOff(id, ch == 9 ? 36 : 60);
            on.channel = off.channel = (uint8_t)ch;
            job.events.push_back({0.05, on});
            job.events.push_back({0.6, off});
            ++id;
        }
        std::vector<float> L, R;
        renderOffline(e, job, L, R);
        CHECK(allFinite(L) && rms(L, 4800, 24000) > 1e-3);
        for (int ch : channels) CHECK(e.partActiveVoices(ch) >= 0);
    }
}

TEST(wav_write_unequal_channel_lengths) {
    // a shorter channel is padded with silence (it was read out of bounds)
    const std::string dir = tempDir("wav_unequal");
    std::vector<float> l(1000, 0.25f), r(400, -0.5f);
    std::string err;
    for (bool f32 : {false, true}) {
        CHECK(writeWav(dir + "/u.wav", l, &r, 48000, f32, err));
        WavData w;
        CHECK(readWav(dir + "/u.wav", w, err));
        CHECK(w.frames == 1000 && w.channels == 2);
        CHECK(std::fabs(w.right[399] + 0.5f) < 1e-3f && w.right[400] == 0.0f && w.right[999] == 0.0f);
        CHECK(std::fabs(w.left[999] - 0.25f) < 1e-3f);
        // longer right channel: the file follows the longer one
        CHECK(writeWav(dir + "/u2.wav", r, &l, 48000, f32, err));
        CHECK(readWav(dir + "/u2.wav", w, err) && w.frames == 1000 && w.left[999] == 0.0f);
    }
}

TEST(wav_reader_rejects_bad_block_align) {
    // fmt chunk declaring a frame stride smaller than the samples it holds
    // (found by fuzzing: the last frame was read past the data chunk)
    auto make = [](uint16_t channels, uint16_t bits, uint16_t blockAlign, uint32_t dataBytes) {
        std::vector<uint8_t> v;
        auto w32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); };
        auto w16 = [&](uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); };
        auto tag = [&](const char* t) { v.insert(v.end(), t, t + 4); };
        tag("RIFF"); w32(36 + dataBytes); tag("WAVE");
        tag("fmt "); w32(16); w16(1); w16(channels); w32(48000); w32(48000u * blockAlign); w16(blockAlign); w16(bits);
        tag("data"); w32(dataBytes);
        for (uint32_t i = 0; i < dataBytes; ++i) v.push_back((uint8_t)i);
        return v;
    };
    WavData w;
    std::string err;
    auto bad = make(1, 16, 1, 101);   // 16-bit samples, 1-byte stride
    CHECK(!readWavMemory(bad.data(), bad.size(), w, err));
    auto bad2 = make(2, 24, 3, 99);   // stereo 24-bit, stride of one sample
    CHECK(!readWavMemory(bad2.data(), bad2.size(), w, err));
    auto good = make(2, 16, 4, 100);
    CHECK(readWavMemory(good.data(), good.size(), w, err) && w.frames == 25 && w.channels == 2);
}
