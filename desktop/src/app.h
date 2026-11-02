// Desktop host application (Dear ImGui UI). Mirrors the Android app's screens
// so features can be developed and tested natively on Windows before the
// Android build.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_out.h"
#include "hsynth/engine.h"
#include "hsynth/params.h"
#include "host_library.h"
#include "midi_in.h"

struct ImVec2;

struct AppSettings {
    int deviceIndex = -1;  // -1 = default
    std::string deviceName;
    int sampleRate = 48000;
    int periodFrames = 256;  // 0 = backend default
    bool exclusive = false;
    int voices = 32;
    int quality = 1;
    bool mpe = false;
    int firstNote = 48;
    int whiteKeys = 22;
    bool slideBend = false;
    int pcOctave = 4;        // PC keyboard: octave of the 'A' key (C4 = 60)
    float pcVelocity = 0.8f;
    std::string lastPreset;
    float uiScale = 0.0f;    // 0 = follow monitor DPI
    bool audioInput = false; // open a capture device (vocoder modulator)
    std::string inputName;   // empty = default capture device
};

class App {
public:
    App(fs::path dataDir, fs::path factoryDir);
    ~App();

    void init(float dpiScale);
    void frame();  // build the ImGui frame
    void onDropFiles(const std::vector<std::string>& paths);
    bool wantsQuit() const { return quitRequested_; }
    void shutdown();

    float uiScaleOverride() const { return s_.uiScale; }
    void setDpiScale(float s) { dpiScale_ = s; }
    // Scripted smoke test: plays notes, switches presets and tabs, saves
    // screenshots, a WAV recording and report.txt into outDir, then quits.
    void startAutotest(const fs::path& outDir);
    std::string takeScreenshotRequest() { std::string s; s.swap(screenshotRequest_); return s; }

private:
    // threading helpers
    void runAsync(std::function<void()> work);  // loader thread
    void post(std::function<void()> ui);        // run on UI thread next frame
    void loaderLoop();

    void log(const std::string& msg, int level = 0);  // 0 info, 1 warn, 2 error
    void loadSettings();
    void saveSettings();
    void startAudio();

    void loadPreset(const fs::path& file);
    void stepPreset(int delta);
    void savePreset(const std::string& name);
    void loadSamples(int layer, const fs::path& file);
    void loadImpulse(int target, const fs::path& file);  // 0 cabinet, 1 IR reverb
    void loadWavetable(int layer, const fs::path& file);
    void refreshLists();

    // UI
    void drawTopBar();
    void drawPlay();
    void drawKeyboard(float width, float height);
    void drawEdit();
    void drawParam(const hsynth::ParamInfo& info, int id);
    void drawFx();
    void drawLooper();
    void drawLibrary();
    void drawMonitor();
    void drawLog(float height);
    void handlePcKeyboard();

    hsynth::Engine engine_;
    AudioOut audio_;
    MidiIn midi_;
    HostLibrary lib_;
    AppSettings s_;

    std::thread loader_;
    std::mutex taskMutex_;
    std::condition_variable taskCv_;
    std::deque<std::function<void()>> tasks_;
    bool quit_ = false;
    std::mutex uiMutex_;
    std::vector<std::function<void()>> uiQueue_;

    struct LogLine { std::string text; int level; };
    std::deque<LogLine> logLines_;
    bool logScrollToBottom_ = false;

    std::vector<HostLibrary::Entry> presets_, samples_;
    fs::path currentPreset_;
    std::string currentPresetName_ = "Init";
    std::string presetComment_;
    std::atomic<bool> busy_{false};

    // play state
    int mouseNote_ = -1;
    uint32_t mouseNoteId_ = 0;
    float mouseDownX_ = 0;
    float lastTimbre_ = -1;
    uint32_t nextNoteId_ = 0x200000;
    int held_[128] = {};
    bool pcHeld_[32] = {};
    int pcHeldKey_[32] = {};
    float bend_ = 0, mod_ = 0;
    bool sustain_ = false;

    // edit state
    int editPage_ = 0;
    int forceEditPage_ = -1;  // select this edit tab on the next frame (autotest)
    int editLayer_ = 0;

    // library state
    char saveName_[128] = "";
    int selectedPreset_ = -1;

    // monitor state
    std::vector<AudioDeviceInfo> devices_, inputDevices_;
    std::vector<std::string> midiPorts_;
    std::vector<float> scopeL_, scopeR_;
    bool scopeFreeze_ = false;
    std::string audioError_;
    double lastStatsReset_ = 0;
    hsynth::EngineStats engineStats_;
    AudioOutStats audioStats_;
    double statsTime_ = 0;

    float dpiScale_ = 1.0f;

    // autotest
    void autotestStep();
    bool autotest_ = false;
    fs::path autotestDir_;
    double autotestStart_ = -1;
    int autotestStage_ = 0;
    int forceTab_ = -1;
    std::string screenshotRequest_;
    bool quitRequested_ = false;
    int autotestPeakVoices_ = 0;
    float autotestPeakLoad_ = 0;
};
