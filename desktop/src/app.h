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
#include "hsynth/gm_bank.h"
#include "hsynth/json.h"
#include "hsynth/midi_learn.h"
#include "hsynth/params.h"
#include "hsynth/preset_index.h"
#include "host_library.h"
#include "midi_in.h"
#include "midi_out.h"
#include "remote_link.h"
#include "local_control.h"

struct ImVec2;

struct AppSettings {
    int deviceIndex = -1;  // -1 = default
    std::string deviceName;
    int sampleRate = 48000;
    int periodFrames = 256;  // 0 = backend default
    bool exclusive = false;
    int voices = hsynth::kDefaultVoices;
    int renderThreads = 0;  // 0 = auto
    int quality = 1;
    int streamMode = 1;      // disk streaming: 0 off, 1 auto (large sets), 2 always
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
    std::string midiOutName; // empty = no MIDI output
    bool midiOutClock = false;
    bool midiOutEcho = true;  // screen / PC keyboard notes also go to the MIDI output
    int midiOutChannel = 0;
    bool localControl = true;  // CLI / MCP control on 127.0.0.1 (local_control.h)
    int localPort = 47800;
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
    void setAudioMuted(bool m) { audio_.setMuted(m); }
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
    void loadPresetInto(const fs::path& file, int part);  // part of a multi
    void applyStreamingSettings();

    // parameters of the part being edited (part 0 in single mode)
    int editPart() const { return engine_.multiTimbral() ? engine_.selectedPart() : 0; }
    float getP(int id) const { return engine_.getPartParam(editPart(), id); }
    void setP(int id, float v);  // with undo history
    void setMultiMode(bool on);

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
    void drawBrowser();
    void drawMulti();
    void drawSceneBar();
    void drawZones(float width, float height);
    void drawMidiMap();
    // setup files (engine setup.h): export / import the whole playing setup
    void drawSetups();
    hsynth::Json hostSettingsJson() const;
    void applyHostSettings(const hsynth::Json& host);  // UI thread
    void importSetup(const std::string& file);
    bool paramContextMenu(const hsynth::ParamInfo& info, int id, float& v);  // true if v changed
    // browser
    void rescanBrowser();
    void updateBrowserResults();
    bool presetPicker(const char* id, int part);  // popup body: search + list, loads into part
    // scenes (multi snapshots in <data>/scenes)
    fs::path scenePath(int slot) const;
    void refreshScenes();
    void recallScene(int slot);
    void storeScene(int slot, const std::string& name);
    void saveMidiMap();
    // performance: notes from the screen / PC keyboard (also echoed to MIDI out)
    void noteEvent(bool on, uint32_t id, int key, float vel);
    void setTransport(bool on);
    void drawSeq();
    void drawMacros();
    void drawMidiOut();
    bool quickAssign(const char* id, hsynth::ModSource src);  // "assign to..." popup: a free mod slot
    // undo / redo / compare (snapshots of the edited part + global parameters)
    struct Snap { int part = 0; std::vector<float> p; };
    Snap captureState() const;
    void applyState(const Snap& s);
    void undo();
    void redo();
    void toggleCompare();
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

    // browser state
    hsynth::PresetIndex index_;
    std::vector<int> browserResults_;
    char browserSearch_[128] = "";
    std::string browserCategory_;
    bool browserFavorites_ = false, browserUser_ = false;
    bool browserDirty_ = true;
    int browserSel_ = -1;
    char tagBuf_[256] = "", catBuf_[96] = "";
    char pickerSearch_[96] = "";

    // MIDI output, transport
    MidiOut midiOut_;
    // undo / compare
    std::deque<Snap> undo_, redo_;
    int lastEditId_ = -1;
    double lastEditTime_ = 0;
    Snap loadedSnap_, compareSaved_;
    bool comparing_ = false;
    int seqLane_ = 0;

    // MIDI learn
    hsynth::MidiLearn learn_;
    int learnSeen_ = -1;

    // SysEx remote control (docs/SYSEX.md)
    std::unique_ptr<RemoteLink> remote_;
    std::unique_ptr<LocalControl> local_;  // CLI / MCP on this PC
    std::string localError_;
    uint32_t remoteSeen_ = 0, localSeen_ = 0;
    void startRemote();
    void startLocal();
    hsynth::RemoteControl::Config remoteCfg_;
    hsynth::RemoteControl::Hooks remoteHooks_;
    // "host.*" requests of the remote protocol (worker thread): screenshot, tab, log, player, live
    bool hostOp(const std::string& op, const hsynth::Json& req, hsynth::Json& reply);
    bool runOnUi(std::function<void()> fn, int timeoutMs = 5000);  // waits for the UI thread

    // multi / scenes
    std::string partNames_[hsynth::kNumParts];
    int midiPoll_ = 0;
    static constexpr int kScenes = 16;
    struct SceneSlot { std::string name; bool used = false; };
    SceneSlot scenes_[kScenes];
    int currentScene_ = -1;
    int sceneEdit_ = -1;
    char sceneName_[64] = "";

    // screens of the phone apps (app_mobile.cpp)
    // Player: MIDI files in <data>/midi, playlists (<data>/playlists.json), 16-channel mixer
    struct Playlist { std::string name; std::vector<std::string> songs; };
    fs::path songsDir() const;
    void refreshSongs();
    void loadPlaylists();
    void savePlaylists();
    std::string loadSongFile(const std::string& path);  // any thread; "" = ok
    void playSong(int index);
    void playerNext(bool manual);
    void playerApplyMix();
    void playerTick();
    double songTempoAt(double t) const;
    void drawPlayer();
    void drawPlaylist();
    std::vector<fs::path> allSongs_, songs_;
    std::vector<Playlist> playlists_;
    int activePlaylist_ = -1, songIndex_ = -1, repeatMode_ = 1;  // repeat: 0 off, 1 all, 2 one
    uint32_t lastSongEnd_ = 0;
    bool songsScanned_ = false, playerShowList_ = false, playlistEdit_ = false;
    std::atomic<bool> songLoading_{false};
    std::string songName_, playerMsg_;
    std::vector<std::pair<double, double>> songTempos_;
    std::vector<hsynth::SongPart> songParts_;
    std::unique_ptr<hsynth::SongProgramLoader> programs_;
    mutable std::mutex songMutex_;
    bool chMute_[hsynth::kNumParts] = {}, chSolo_[hsynth::kNumParts] = {}, chVolSet_[hsynth::kNumParts] = {};
    float chVol_[hsynth::kNumParts] = {}, chLevel_[hsynth::kNumParts] = {};
    float seekTarget_ = -1;
    char playlistName_[64] = "";
    // Live: set lists in <data>/setlists (library presets, sound / multi snapshots)
    struct SetEntry { int kind = 0; std::string name, file; };  // kind 0 library (file = preset path), 1 sound, 2 multi
    struct SetList { std::string name; std::vector<SetEntry> entries; };
    fs::path setListDir() const;
    void loadSetLists();
    void saveSetLists();
    void addCurrentToSet();
    void recallSetEntry(int k);
    void drawLive();
    std::vector<SetList> setLists_;
    int activeSet_ = -1, setCurrent_ = -1;
    bool setEdit_ = false, setListsLoaded_ = false;
    char setName_[64] = "";
    // Sample: record the input / output, trim, loop, save as an instrument
    void drawSample();
    std::vector<float> takeL_, takeR_;
    int takeRate_ = 48000, takeRoot_ = 60, dragHandle_ = -1;
    int64_t takeStart_ = 0, takeEnd_ = 0, loopStart_ = 0, loopEnd_ = 0;
    bool takeLoop_ = false, takeNormalize_ = true, takeFromInput_ = true, sampling_ = false;
    char takeName_[64] = "My Sample";
    std::string sampleMsg_;
    // Perform: Super Knob, scale lock, chord mode, phrase recorder
    void drawPerform();
    std::string phraseMsg_;
    // Edit graphs
    void drawEnvGraph(int attackId, int decayId, int sustainId, int releaseId);
    void drawFilterGraph();
    void drawFmAlgorithm(int layer);
    int envDrag_ = -1;

    // autotest
    void autotestStep();
    bool autotest_ = false;
    fs::path autotestDir_;
    double autotestStart_ = -1;
    int autotestStage_ = 0;
    double autotestMark_ = 0;  // time a stage started (stages after the song load)
    int forceTab_ = -1;
    std::string screenshotRequest_;
    bool quitRequested_ = false;
    int autotestPeakVoices_ = 0;
    float autotestPeakLoad_ = 0;
};
