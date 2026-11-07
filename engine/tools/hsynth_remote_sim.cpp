// A synth without audio / MIDI hardware for testing SysEx remote clients
// (desktop/tools/hsynth_remote.py --sim): every stdin line is one SysEx message
// in hex ("F0 7D ... F7"), every reply / notification is printed the same way.
//
//   hsynth_remote_sim [--lib DIR] [--presets DIR;DIR] [--user DIR] [--midi DIR] [--id N]
#include <cctype>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "hsynth/engine.h"
#include "hsynth/midi_learn.h"
#include "hsynth/remote.h"

using namespace hsynth;

int main(int argc, char** argv) {
    RemoteControl::Config cfg;
    cfg.deviceName = "HSynth Sim";
    cfg.version = "sim";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--lib") cfg.libraryRoot = v;
        else if (k == "--user") cfg.userPresetDir = v;
        else if (k == "--midi") cfg.midiDir = v;
        else if (k == "--id") cfg.deviceId = std::stoi(v);
        else if (k == "--presets") {
            for (size_t a = 0; a <= v.size();) {
                size_t b = v.find(';', a);
                if (b == std::string::npos) b = v.size();
                if (b > a) cfg.presetDirs.push_back(v.substr(a, b - a));
                a = b + 1;
            }
        }
    }
    Engine engine;
    engine.prepare(48000.0f, 256);
    MidiLearn learn;
    cfg.learn = &learn;
    RemoteControl rc(engine, cfg);
    std::mutex outMutex;
    rc.setOutput([&](const std::vector<uint8_t>& m) {
        std::lock_guard<std::mutex> lock(outMutex);
        std::string line;
        char b[4];
        for (uint8_t c : m) {
            std::snprintf(b, sizeof b, "%02X ", c);
            line += b;
        }
        line.back() = '\n';
        std::fputs(line.c_str(), stdout);
        std::fflush(stdout);
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        std::vector<uint8_t> msg;
        for (size_t i = 0; i < line.size();) {
            while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
            if (i + 1 >= line.size()) break;
            msg.push_back((uint8_t)std::stoi(line.substr(i, 2), nullptr, 16));
            i += 2;
        }
        if (!msg.empty()) rc.onSysEx(msg.data(), msg.size());
        // the engine needs no audio for remote control, but a block now and then keeps
        // the phrase recorder / snapshots answering
        float l[256], r[256];
        engine.process(l, r, 256);
    }
    return 0;
}
