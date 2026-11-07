// SysEx remote control (remote.h, docs/SYSEX.md).
#include <chrono>
#include <filesystem>
#include <thread>

#include "hsynth/engine.h"
#include "hsynth/json.h"
#include "hsynth/midi_learn.h"
#include "hsynth/params.h"
#include "hsynth/remote.h"
#include "test.h"

using namespace hsynth;
namespace fs = std::filesystem;

namespace {

Json req(const std::string& text) {
    Json j;
    std::string err;
    Json::parse(text, j, err);
    return j;
}

// Sends a request as SysEx and waits for the whole reply (through poll()).
Json roundTrip(RemoteControl& rc, const std::string& json, int seq = 5) {
    for (const auto& m : sysex::encode(sysex::kAllDevices, sysex::Request, seq, json)) rc.onSysEx(m.data(), m.size());
    sysex::Assembler as;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::vector<uint8_t> msg;
    while (std::chrono::steady_clock::now() < until) {
        if (!rc.poll(msg)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        sysex::Frame f;
        if (!sysex::parseFrame(msg.data(), msg.size(), f) || f.type != sysex::Reply || f.seq != seq) continue;
        std::string text;
        if (as.add(f, text)) return req(text);
    }
    return Json();
}

}  // namespace

TEST(sysex_framing_chunks_and_ascii) {
    Json j = Json::object();
    j.set("name", "ピアノ \xF0\x9F\x8E\xB9");  // Japanese + an emoji (surrogate pair)
    std::string big(1000, 'x');
    j.set("big", big);
    const std::string text = sysex::asciiJson(j);
    for (char c : text) CHECK((unsigned char)c < 0x80);
    const auto msgs = sysex::encode(3, sysex::Reply, 9, text);
    CHECK(msgs.size() == (text.size() + sysex::kChunkBytes - 1) / sysex::kChunkBytes);
    sysex::Assembler as;
    std::string out;
    bool done = false;
    // out of order: last chunk first
    for (size_t k = msgs.size(); k-- > 0;) {
        const auto& m = msgs[k];
        CHECK(m.front() == 0xF0 && m.back() == 0xF7 && m.size() <= 256);
        for (size_t i = 1; i + 1 < m.size(); ++i) CHECK(m[i] < 0x80);
        sysex::Frame f;
        CHECK(sysex::parseFrame(m.data(), m.size(), f));
        CHECK(f.device == 3 && f.type == sysex::Reply && f.seq == 9);
        done = as.add(f, out);
    }
    CHECK(done);
    Json back;
    std::string err;
    CHECK(Json::parse(out, back, err));
    CHECK(back.find("name") && back.find("name")->string() == "ピアノ \xF0\x9F\x8E\xB9");
    CHECK(back.find("big") && back.find("big")->string() == big);
    // other manufacturers are not ours
    const uint8_t roland[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
    sysex::Frame f;
    CHECK(!sysex::parseFrame(roland, sizeof roland, f));
}

TEST(remote_get_set_params_and_mix) {
    Engine e;
    e.prepare(48000.0f, 256);
    RemoteControl rc(e, RemoteControl::Config{});
    Json r = rc.handle(req(R"({"op":"hello"})"));
    CHECK(r.find("ok")->boolean() && (int)r.find("params")->number() == P::kNumParams);

    // set by name (number, enum option, bool) and read back
    const auto& table = paramTable();
    const std::string cutoff = table[P::FilterCutoff].name;
    const std::string ftype = table[P::FilterType].name;
    const std::string opt = table[P::FilterType].options.back();
    r = rc.handle(req("{\"op\":\"set\",\"values\":{\"" + cutoff + "\":1234,\"" + ftype + "\":\"" + opt + "\"}}"));
    CHECK(r.find("ok")->boolean());
    CHECK_NEAR(e.getParam(P::FilterCutoff), 1234.0f, 0.5f);
    CHECK((int)e.getParam(P::FilterType) == (int)table[P::FilterType].options.size() - 1);
    r = rc.handle(req("{\"op\":\"get\",\"names\":[\"" + cutoff + "\",\"" + ftype + "\"]}"));
    CHECK_NEAR(r.find("values")->find(cutoff)->number(), 1234.0, 0.5);
    CHECK(r.find("values")->find(ftype)->string() == opt);
    // the whole table / only the changed ones
    r = rc.handle(req(R"({"op":"get"})"));
    CHECK((int)r.find("values")->size() == P::kNumParams);
    r = rc.handle(req(R"({"op":"get","changed":true})"));
    CHECK(r.find("values")->find(cutoff) != nullptr && r.find("values")->size() < 10);
    // unknown names are reported, the rest is applied
    r = rc.handle(req("{\"op\":\"set\",\"values\":{\"no.such\":1,\"" + cutoff + "\":500}}"));
    CHECK(!r.find("ok")->boolean());
    CHECK_NEAR(e.getParam(P::FilterCutoff), 500.0f, 0.5f);

    // multi mode, part params and mix
    r = rc.handle(req(R"({"op":"mode","multi":true})"));
    CHECK(e.multiTimbral());
    r = rc.handle(req("{\"op\":\"set\",\"part\":5,\"values\":{\"" + cutoff + "\":800}}"));
    CHECK_NEAR(e.getPartParam(5, P::FilterCutoff), 800.0f, 0.5f);
    CHECK_NEAR(e.getPartParam(0, P::FilterCutoff), 500.0f, 0.5f);
    r = rc.handle(req(R"({"op":"mix_set","part":5,"values":{"volume":0.5,"mute":true,"channel":3}})"));
    CHECK_NEAR(e.getPartMix(5, Mix::Volume), 0.5f, 1e-6);
    CHECK(e.getPartMix(5, Mix::Mute) > 0.5f);
    CHECK((int)e.getPartMix(5, Mix::Channel) == 3);
    r = rc.handle(req(R"({"op":"mix_get"})"));
    CHECK(r.find("parts")->size() == (size_t)kNumParts);
    CHECK(rc.handle(req(R"({"op":"nope"})")).find("ok")->boolean() == false);
}

TEST(remote_state_round_trip) {
    Engine a, b;
    a.prepare(48000.0f, 256);
    b.prepare(48000.0f, 256);
    RemoteControl ra(a, RemoteControl::Config{}), rb(b, RemoteControl::Config{});
    a.setMultiTimbral(true);
    a.setPartParam(2, P::FilterCutoff, 700.0f);
    a.setPartParam(0, P::MasterVolume, 0.3f);
    a.setPartMix(2, Mix::Pan, -0.5f);
    a.selectPart(2);
    Json st = ra.handle(req(R"({"op":"get_state"})"));
    CHECK(st.find("ok")->boolean() && st.find("multi")->boolean());
    // compact: far fewer parameters than the full dump
    CHECK(sysex::asciiJson(st).size() < 40000);
    Json set = Json::object();
    set.set("op", "set_state");
    set.set("state", *st.find("state"));
    Json r = rb.handle(set);
    CHECK(r.find("ok")->boolean());
    CHECK(b.multiTimbral());
    CHECK_NEAR(b.getPartParam(2, P::FilterCutoff), 700.0f, 0.5f);
    CHECK_NEAR(b.getParam(P::MasterVolume), 0.3f, 1e-4);
    CHECK_NEAR(b.getPartMix(2, Mix::Pan), -0.5f, 1e-6);
    CHECK(b.selectedPart() == 2);
    // single mode state
    a.setMultiTimbral(false);
    st = ra.handle(req(R"({"op":"get_state"})"));
    CHECK(!st.find("multi")->boolean());
    set.set("state", *st.find("state"));
    rb.handle(set);
    CHECK(!b.multiTimbral());
    CHECK_NEAR(b.getParam(P::MasterVolume), 0.3f, 1e-4);
}

TEST(remote_sysex_round_trip_and_notifications) {
    Engine e;
    e.prepare(48000.0f, 256);
    MidiLearn learn;
    RemoteControl::Config cfg;
    cfg.deviceId = 4;
    cfg.learn = &learn;
    RemoteControl rc(e, cfg);
    Json r = roundTrip(rc, R"({"op":"hello"})");
    CHECK(r.find("op") && r.find("op")->string() == "hello");
    CHECK(r.find("device_id") && (int)r.find("device_id")->number() == 4);
    // a request for another device id is ignored
    for (const auto& m : sysex::encode(9, sysex::Request, 1, R"({"op":"hello"})")) rc.onSysEx(m.data(), m.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::vector<uint8_t> msg;
    CHECK(!rc.poll(msg));
    // a long request (many chunks)
    Json big = Json::object();
    big.set("op", "set");
    Json values = Json::object();
    const auto& table = paramTable();
    for (int i = 0; i < 60; ++i)
        if (table[(size_t)i].kind == ParamKind::Float) values.set(table[(size_t)i].name, (double)table[(size_t)i].min);
    big.set("values", values);
    const uint32_t before = rc.changeCount();
    r = roundTrip(rc, sysex::asciiJson(big), 7);
    CHECK(r.find("ok") && r.find("ok")->boolean());
    CHECK(rc.changeCount() > before);
    // MIDI learn over SysEx
    r = roundTrip(rc, "{\"op\":\"learn\",\"cmd\":\"arm\",\"param\":\"" + table[P::FilterCutoff].name + "\"}", 8);
    CHECK(learn.armedParam() == P::FilterCutoff);

    // notifications: a change made by someone else is pushed, our own are not echoed
    r = rc.handle(req(R"({"op":"subscribe","what":["params"]})"));
    rc.collectChanges();
    e.setParam(P::FilterReso, 0.77f);
    auto n = rc.collectChanges();
    CHECK(n.size() == 1);
    if (!n.empty()) {
        CHECK(n[0].find("event")->string() == "changed");
        CHECK_NEAR(n[0].find("values")->find(table[P::FilterReso].name)->number(), 0.77, 1e-4);
    }
    CHECK(rc.collectChanges().empty());
    e.setMultiTimbral(true);
    e.setPartMix(3, Mix::Volume, 0.25f);
    n = rc.collectChanges();
    bool sawMix = false, sawMode = false;
    for (const Json& x : n) {
        if (x.find("mix") && (int)x.find("part")->number() == 3) sawMix = true;
        if (x.find("event")->string() == "mode") sawMode = true;
    }
    CHECK(sawMix && sawMode);
}

TEST(remote_files_and_presets_stay_inside) {
    const fs::path dir = fs::temp_directory_path() / "hsynth_tests" / "remote";
    fs::remove_all(dir);
    fs::create_directories(dir / "midi" / "sub");
    fs::create_directories(dir / "user");
    { FILE* f = std::fopen((dir / "midi" / "a.mid").string().c_str(), "wb"); if (f) std::fclose(f); }
    Engine e;
    e.prepare(48000.0f, 256);
    RemoteControl::Config cfg;
    cfg.midiDir = (dir / "midi").generic_string();
    cfg.userPresetDir = (dir / "user").generic_string();
    cfg.presetDirs = {cfg.userPresetDir};
    RemoteControl rc(e, cfg);
    Json r = rc.handle(req(R"({"op":"songs"})"));
    CHECK(r.find("ok")->boolean());
    CHECK(r.find("files")->size() == 1 && r.find("files")->at(0).string() == "a.mid");
    CHECK(r.find("dirs")->size() == 1);
    CHECK(!rc.handle(req(R"({"op":"files","kind":"midi","dir":"../.."})")).find("ok")->boolean());
    CHECK(!rc.handle(req(R"({"op":"song_load","file":"../x.mid"})")).find("ok")->boolean());
    // save a preset, find it, load it back
    e.setParam(P::FilterCutoff, 321.0f);
    r = rc.handle(req(R"({"op":"save_preset","name":"Remote Test","category":"Test"})"));
    CHECK(r.find("ok")->boolean());
    e.setParam(P::FilterCutoff, 5000.0f);
    r = rc.handle(req(R"({"op":"presets","rescan":true,"query":"remote"})"));
    CHECK((int)r.find("total")->number() == 1);
    r = rc.handle(req(R"({"op":"load_preset","name":"Remote Test"})"));
    CHECK(r.find("ok")->boolean());
    CHECK_NEAR(e.getParam(P::FilterCutoff), 321.0f, 0.5f);
    fs::remove_all(dir);
}
