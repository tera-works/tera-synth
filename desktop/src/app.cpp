#include "app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

#include "hsynth/fx/ir_load.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/preset.h"
#include "hsynth/sample_set.h"
#include "hsynth/setup.h"
#include "hsynth/wav.h"
#include "imgui.h"
#include "platform.h"

using namespace hsynth;

namespace {

const ImVec4 kAccent(0.31f, 0.76f, 0.97f, 1.0f);
const ImVec4 kDim(0.55f, 0.58f, 0.64f, 1.0f);
const ImVec4 kWarn(1.0f, 0.72f, 0.30f, 1.0f);
const ImVec4 kErr(0.94f, 0.33f, 0.31f, 1.0f);

bool isBlack(int note) {
    switch (note % 12) {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

std::string noteName(int n) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lowerExt(const fs::path& p) {
    std::string e = pathToUtf8(p.extension());
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

// PC keyboard piano layout: A W S E D F T G Y H U J K O L P ;
struct PcKey { ImGuiKey key; int semitone; };
const PcKey kPcKeys[] = {
    {ImGuiKey_A, 0}, {ImGuiKey_W, 1}, {ImGuiKey_S, 2}, {ImGuiKey_E, 3}, {ImGuiKey_D, 4}, {ImGuiKey_F, 5},
    {ImGuiKey_T, 6}, {ImGuiKey_G, 7}, {ImGuiKey_Y, 8}, {ImGuiKey_H, 9}, {ImGuiKey_U, 10}, {ImGuiKey_J, 11},
    {ImGuiKey_K, 12}, {ImGuiKey_O, 13}, {ImGuiKey_L, 14}, {ImGuiKey_P, 15}, {ImGuiKey_Semicolon, 16},
};
constexpr int kNumPcKeys = (int)(sizeof kPcKeys / sizeof kPcKeys[0]);

struct EditPage {
    const char* title;
    bool perLayer;
    std::vector<std::string> groups;  // "%L" is replaced by "Layer A/B/C"
};
const std::vector<EditPage> kPages = {
    {"Layers", false, {"Layer A", "Layer B", "Layer C", "Master"}},
    {"VA", true, {"%L / VA"}},
    {"FM", true, {"%L / FM"}},
    {"PCM", true, {"%L / PCM"}},
    {"WT", true, {"%L / WT"}},
    {"Filter", false, {"Filter"}},
    {"Env", false, {"Amp Env", "Mod Env"}},
    {"LFO", false, {"LFO"}},
    {"FX", false, {}},  // drawn by drawFx(): chain editor + block panels
    {"FX Mod", false, {"FX Mod"}},
    {"Mod", false, {"Mod Matrix"}},
    {"Organ", true, {"%L / Organ"}},
    {"MSEG", false, {"MSEG"}},
    {"Macros / Sends", false, {"Macros", "Sends", "Send FX"}},
};

}  // namespace

App::App(fs::path dataDir, fs::path factoryDir)
    : audio_(engine_), midi_(engine_), lib_(std::move(dataDir), std::move(factoryDir)) {
    loader_ = std::thread([this] { loaderLoop(); });
    midi_.setLearn(&learn_);
    for (auto& n : partNames_) n = "Init";
}

App::~App() { shutdown(); }

void App::shutdown() {
    if (loader_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(taskMutex_);
            quit_ = true;
        }
        taskCv_.notify_all();
        loader_.join();
    }
    remote_.reset();  // before the MIDI ports and the engine
    midi_.closeAll();
    audio_.stop();
    saveSettings();
    saveMidiMap();
}

// ------------------------------------------------------------------ threads

void App::runAsync(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        tasks_.push_back(std::move(work));
    }
    taskCv_.notify_one();
}

void App::post(std::function<void()> ui) {
    std::lock_guard<std::mutex> lock(uiMutex_);
    uiQueue_.push_back(std::move(ui));
}

void App::loaderLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(taskMutex_);
            taskCv_.wait(lock, [this] { return quit_ || !tasks_.empty(); });
            if (quit_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        busy_ = true;
        task();
        busy_ = false;
    }
}

void App::log(const std::string& msg, int level) {
    char ts[16];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
    logLines_.push_back({std::string(ts) + "  " + msg, level});
    while (logLines_.size() > 500) logLines_.pop_front();
    logScrollToBottom_ = true;
}

// ------------------------------------------------------------------ settings

void App::loadSettings() {
    Json j;
    std::string err;
    if (!Json::parse(readFile(lib_.dataDir() / "settings.json"), j, err) || !j.isObject()) return;
    auto num = [&](const char* k, auto& v) { if (auto x = j.find(k)) v = (std::decay_t<decltype(v)>)x->number(v); };
    num("deviceIndex", s_.deviceIndex);
    num("sampleRate", s_.sampleRate);
    num("periodFrames", s_.periodFrames);
    num("voices", s_.voices);
    num("renderThreads", s_.renderThreads);
    num("quality", s_.quality);
    num("streamMode", s_.streamMode);
    num("firstNote", s_.firstNote);
    num("whiteKeys", s_.whiteKeys);
    num("pcOctave", s_.pcOctave);
    num("pcVelocity", s_.pcVelocity);
    num("uiScale", s_.uiScale);
    if (auto x = j.find("exclusive")) s_.exclusive = x->boolean();
    if (auto x = j.find("mpe")) s_.mpe = x->boolean();
    if (auto x = j.find("slideBend")) s_.slideBend = x->boolean();
    if (auto x = j.find("deviceName"); x && x->isString()) s_.deviceName = x->string();
    if (auto x = j.find("audioInput")) s_.audioInput = x->boolean();
    if (auto x = j.find("inputName"); x && x->isString()) s_.inputName = x->string();
    if (auto x = j.find("midiOutName"); x && x->isString()) s_.midiOutName = x->string();
    if (auto x = j.find("midiOutClock")) s_.midiOutClock = x->boolean();
    if (auto x = j.find("midiOutEcho")) s_.midiOutEcho = x->boolean();
    num("midiOutChannel", s_.midiOutChannel);
    if (auto x = j.find("lastPreset"); x && x->isString()) s_.lastPreset = x->string();
}

void App::saveSettings() {
    Json j = Json::object();
    j.set("deviceIndex", s_.deviceIndex);
    j.set("deviceName", s_.deviceName);
    j.set("sampleRate", s_.sampleRate);
    j.set("periodFrames", s_.periodFrames);
    j.set("exclusive", s_.exclusive);
    j.set("voices", s_.voices);
    j.set("renderThreads", s_.renderThreads);
    j.set("quality", s_.quality);
    j.set("streamMode", s_.streamMode);
    j.set("mpe", s_.mpe);
    j.set("firstNote", s_.firstNote);
    j.set("whiteKeys", s_.whiteKeys);
    j.set("slideBend", s_.slideBend);
    j.set("pcOctave", s_.pcOctave);
    j.set("pcVelocity", (double)s_.pcVelocity);
    j.set("lastPreset", s_.lastPreset);
    j.set("uiScale", (double)s_.uiScale);
    j.set("audioInput", s_.audioInput);
    j.set("inputName", s_.inputName);
    j.set("midiOutName", s_.midiOutName);
    j.set("midiOutClock", s_.midiOutClock);
    j.set("midiOutEcho", s_.midiOutEcho);
    j.set("midiOutChannel", s_.midiOutChannel);
    std::ofstream out(lib_.dataDir() / "settings.json", std::ios::binary);
    out << j.dump(2);
}

// ------------------------------------------------------------------ setup files

Json App::hostSettingsJson() const {
    // settings that mean the same on every host (device names / audio buffers stay local)
    Json h = Json::object();
    h.set("voices", s_.voices);
    h.set("quality", s_.quality);
    h.set("mpe", s_.mpe);
    return h;
}

void App::applyHostSettings(const Json& host) {
    if (const Json* v = host.find("voices"); v && v->isNumber()) {
        s_.voices = std::clamp((int)v->number(), 8, 256);
        engine_.setMaxVoices(s_.voices);
    }
    if (const Json* v = host.find("quality"); v && v->isNumber()) {
        s_.quality = std::clamp((int)v->number(), 0, 2);
        engine_.setQuality((Quality)s_.quality);
    }
    if (const Json* v = host.find("mpe")) {
        s_.mpe = v->boolean();
        midi_.setMpe(s_.mpe);
    }
    saveSettings();
}

void App::importSetup(const std::string& file) {
    const std::string root = pathToUtf8(lib_.libraryRoot());
    runAsync([this, file, root] {
        SetupResult res;
        std::string err;
        const bool ok = applySetupJson(engine_, readFile(utf8ToPath(file)), root, &learn_, res, err);
        post([this, ok, err, res, file] {
            if (!ok) { log("setup import failed: " + err, 2); return; }
            for (auto& w : res.warnings) log("  " + w, 1);
            if (res.hasState) {
                midi_.setMultiTimbral(res.multi);
                if (!res.multi) engine_.selectPart(0);
                for (int k = 0; k < kNumParts && k < (int)res.partNames.size(); ++k) partNames_[k] = res.partNames[(size_t)k];
                if (!res.partNames.empty()) currentPresetName_ = res.partNames[0];
            }
            applyHostSettings(res.host);
            if (res.hasMidiLearn) {
                std::string e;
                learn_.save(pathToUtf8(lib_.dataDir() / "midi_map.json"), e);
            }
            log("setup \"" + res.name + "\" imported from " + file);
        });
    });
}

void App::drawSetups() {
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Setup (export / import)");
    ImGui::TextColored(kDim, "Parts and their channels, presets, mix, effects, MIDI learn and polyphony in one file.");
    if (ImGui::Button("Export setup...")) {
        std::error_code ec;
        fs::create_directories(lib_.dataDir() / "setups", ec);
        const std::string path = platform::saveFile({{"HSynth setup (*.json)", "*.json"}}, "setup.json", "json");
        if (!path.empty()) {
            const std::string name = pathToUtf8(utf8ToPath(path).stem());
            std::vector<std::string> names(partNames_, partNames_ + kNumParts);
            const std::string text = makeSetupJson(engine_, name, pathToUtf8(lib_.libraryRoot()), &learn_,
                                                   hostSettingsJson(), "", names);
            std::ofstream out(utf8ToPath(path), std::ios::binary);
            out << text;
            if (out) log("setup exported: " + path);
            else log("could not write " + path, 2);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import setup...")) {
        const auto files = platform::openFiles({{"HSynth setup (*.json)", "*.json"}}, false);
        if (!files.empty()) importSetup(files.front());
    }
    // setups kept in data/setups: one click
    std::error_code ec;
    const fs::path dir = lib_.dataDir() / "setups";
    if (fs::is_directory(dir, ec)) {
        for (const auto& f : fs::directory_iterator(dir, ec)) {
            if (f.path().extension() != ".json") continue;
            const std::string label = "Load " + pathToUtf8(f.path().stem());
            if (ImGui::SmallButton(label.c_str())) importSetup(pathToUtf8(f.path()));
        }
    }
}

// ------------------------------------------------------------------ init

void App::init(float dpiScale) {
    dpiScale_ = dpiScale;
    std::error_code ec;
    fs::create_directories(lib_.dataDir(), ec);
    loadSettings();
    log("HSynth desktop - data folder: " + pathToUtf8(lib_.dataDir()));
    const std::string err = lib_.syncFactory();
    if (!err.empty()) log(err, 2);
    applyStreamingSettings();
    refreshLists();
    refreshScenes();
    {
        std::string e;
        if (fs::exists(lib_.dataDir() / "midi_map.json") && !learn_.load(pathToUtf8(lib_.dataDir() / "midi_map.json"), e))
            log("MIDI map: " + e, 1);
    }

    engine_.setMaxVoices(s_.voices);
    engine_.setRenderThreads(s_.renderThreads);
    engine_.setQuality((Quality)s_.quality);
    devices_ = audio_.listDevices();
    inputDevices_ = audio_.listInputDevices();
    // device indices can change between runs: prefer the saved name
    if (!s_.deviceName.empty()) {
        s_.deviceIndex = -1;
        for (int i = 0; i < (int)devices_.size(); ++i)
            if (devices_[(size_t)i].name == s_.deviceName) s_.deviceIndex = i;
    }
    startAudio();

    midi_.setMpe(s_.mpe);
    midiPorts_ = midi_.openAll();
    startRemote();
    log(midiPorts_.empty() ? "no MIDI input ports" : "MIDI inputs: " + std::to_string(midiPorts_.size()));
    if (!s_.midiOutName.empty()) {
        std::string e;
        if (midiOut_.open(s_.midiOutName, e)) {
            log("MIDI output: " + s_.midiOutName);
            if (s_.midiOutClock) midiOut_.setClock(true, [this] { return engine_.getParam(P::fxChain(P::FxTempo)); });
        } else {
            log(e, 1);
        }
    }

    fs::path start = utf8ToPath(s_.lastPreset);
    if (s_.lastPreset.empty() || !fs::exists(start)) start = presets_.empty() ? fs::path() : presets_.front().file;
    if (!start.empty()) loadPreset(start);
}

void App::startRemote() {
    RemoteControl::Config cfg;
    cfg.deviceName = "HSynth Desktop";
    cfg.version = "0.8.3";
    cfg.libraryRoot = pathToUtf8(lib_.libraryRoot());
    cfg.presetDirs = {pathToUtf8(lib_.dataDir() / "presets" / "factory"), pathToUtf8(lib_.userPresetDir())};
    cfg.userPresetDir = pathToUtf8(lib_.userPresetDir());
    cfg.midiDir = pathToUtf8(lib_.dataDir() / "midi");
    std::error_code ec;
    fs::create_directories(lib_.dataDir() / "midi", ec);
    cfg.learn = &learn_;
    RemoteControl::Hooks hooks;
    hooks.setMultiTimbral = [this](bool on) { midi_.setMultiTimbral(on); };
    hooks.setMpe = [this](bool on) {
        midi_.setMpe(on);
        post([this, on] { s_.mpe = on; });
    };
    hooks.hostSettings = [this](Json& host) { host = hostSettingsJson(); };
    hooks.applyHostSettings = [this](const Json& host) { post([this, host] { applyHostSettings(host); }); };
    hooks.presetLoaded = [this](int part, const std::string& name) {
        post([this, part, name] {
            if (part >= 0 && part < kNumParts) partNames_[part] = name;
            if (part <= 0) currentPresetName_ = name;
            log("remote: preset \"" + name + "\"");
        });
    };
    remote_ = std::make_unique<RemoteLink>(engine_, midi_, cfg, hooks);
}

void App::startAudio() {
    audioError_.clear();
    int input = -2;
    if (s_.audioInput) {
        input = -1;
        for (int i = 0; i < (int)inputDevices_.size(); ++i)
            if (inputDevices_[(size_t)i].name == s_.inputName) input = i;
    }
    if (!audio_.start(s_.deviceIndex, s_.sampleRate, s_.periodFrames, s_.exclusive, input, audioError_)) {
        log(audioError_, 2);
        return;
    }
    if (!audioError_.empty()) log(audioError_, 1);
    const AudioOutStats st = audio_.stats();
    if (st.input) log("audio input: " + st.inputName + " (vocoder modulator)");
    log("audio: " + st.backend + " / " + st.deviceName + "  " + std::to_string(st.sampleRate) + " Hz, period " +
        std::to_string(st.periodFrames) + " x" + std::to_string(st.periods) + (st.exclusive ? ", exclusive" : ", shared"));
}

void App::refreshLists() {
    presets_ = lib_.presets();
    samples_ = lib_.sampleSources();
    rescanBrowser();
}

// ------------------------------------------------------------------ actions

void App::loadPreset(const fs::path& file) {
    if (engine_.multiTimbral()) {
        loadPresetInto(file, engine_.selectedPart());
        return;
    }
    log("loading preset " + pathToUtf8(file.filename()) + " ...");
    const std::string root = pathToUtf8(lib_.libraryRoot());
    runAsync([this, file, root] {
        Preset p;
        std::string err;
        std::vector<std::string> warnings;
        const bool ok = loadPresetFile(pathToUtf8(file), p, err);
        if (ok) applyPreset(engine_, p, root, warnings);
        post([this, ok, err, warnings, p, file] {
            if (!ok) { log("preset load failed: " + err, 2); return; }
            for (auto& w : warnings) log("  " + w, 1);
            currentPreset_ = file;
            currentPresetName_ = p.name;
            partNames_[0] = p.name;
            presetComment_ = p.comment;
            loadedSnap_ = captureState();
            comparing_ = false;
            s_.lastPreset = pathToUtf8(file);
            log("preset \"" + p.name + "\" loaded");
        });
    });
}

void App::stepPreset(int delta) {
    if (presets_.empty()) return;
    int idx = -1;
    for (int i = 0; i < (int)presets_.size(); ++i)
        if (presets_[(size_t)i].file == currentPreset_) idx = i;
    const int n = (int)presets_.size();
    loadPreset(presets_[(size_t)(((idx < 0 ? 0 : idx + delta) % n + n) % n)].file);
}

void App::savePreset(const std::string& name) {
    const fs::path file = lib_.userPresetFile(name);
    Preset p = engine_.multiTimbral() ? capturePartPreset(engine_, editPart(), name, pathToUtf8(lib_.libraryRoot()))
                                      : capturePreset(engine_, name, pathToUtf8(lib_.libraryRoot()));
    p.category = "User";
    p.author = "user";
    std::string err;
    if (!savePresetFile(pathToUtf8(file), p, err)) {
        log("save failed: " + err, 2);
        return;
    }
    currentPreset_ = file;
    currentPresetName_ = name;
    s_.lastPreset = pathToUtf8(file);
    refreshLists();
    log("preset saved: " + pathToUtf8(file.filename()));
}

void App::loadSamples(int layer, const fs::path& file) {
    log(std::string("loading ") + pathToUtf8(file.filename()) + " into layer " + (char)('A' + layer) + " ...");
    const int part = editPart();
    runAsync([this, layer, file, part] {
        const auto t0 = std::chrono::steady_clock::now();
        std::string err;
        std::vector<std::string> warnings;
        const bool ok = loadPartSamples(engine_, part, layer, pathToUtf8(file), err, warnings);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ok) engine_.setPartParam(part, P::layer(layer, P::LType), (float)LayerType::PCM);
        post([this, ok, err, warnings, layer, file, ms] {
            if (ok) log(std::string("layer ") + (char)('A' + layer) + ": " + pathToUtf8(file.filename()) + " loaded in " +
                        std::to_string((int)ms) + " ms");
            else log("load failed: " + err, 2);
            for (auto& w : warnings) log("  " + w, 1);
        });
    });
}

void App::loadWavetable(int layer, const fs::path& file) {
    log(std::string("loading wavetable ") + pathToUtf8(file.filename()) + " into layer " + (char)('A' + layer) + " ...");
    const int part = editPart();
    runAsync([this, layer, file, part] {
        std::string err;
        fs::path src;
        try {
            src = lib_.importWav(file);  // keep a copy in the library so presets can find it
        } catch (const std::exception& e) {
            err = e.what();
        }
        bool ok = err.empty() && loadPartWavetable(engine_, part, layer, pathToUtf8(src), err);
        if (ok) {
            engine_.setPartParam(part, P::layer(layer, P::LType), (float)LayerType::WT);
            engine_.setPartParam(part, P::layer(layer, P::WtTable), (float)builtinWavetableCount());  // "user"
        }
        post([this, ok, err, file, layer] {
            if (ok) log(std::string("layer ") + (char)('A' + layer) + ": wavetable " + pathToUtf8(file.filename()) + " loaded");
            else log("wavetable load failed: " + err, 2);
        });
    });
}

void App::loadImpulse(int target, const fs::path& file) {
    log(std::string("loading ") + (target == 0 ? "cabinet" : "reverb") + " IR " + pathToUtf8(file.filename()) + " ...");
    runAsync([this, target, file] {
        std::string err;
        fs::path src;
        try {
            src = lib_.importIr(file);
        } catch (const std::exception& e) {
            err = e.what();
        }
        std::unique_ptr<fx::ConvIr> ir;
        if (err.empty()) ir = fx::loadImpulseResponse(pathToUtf8(src), engine_.sampleRate(), target, err);
        const bool ok = ir != nullptr;
        const int len = ir ? ir->length : 0;
        if (ok) {
            engine_.setFxImpulse(target, std::move(ir));
            // switch the block to its user-IR mode
            if (target == 0) engine_.setParam(P::fxp(fx::Cab, fx::cab::Model), 8.0f);
            else engine_.setParam(P::fxp(fx::Convo, fx::convo::Ir), 3.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));  // let the audio thread pick it up
            engine_.collectGarbage();
        }
        post([this, ok, err, file, len, target] {
            if (ok) log(std::string(target == 0 ? "cabinet" : "reverb") + " IR loaded: " + pathToUtf8(file.filename()) + " (" +
                        std::to_string(len) + " samples)");
            else log("IR load failed: " + err, 2);
        });
    });
}

void App::onDropFiles(const std::vector<std::string>& paths) {
    for (const auto& s : paths) {
        const fs::path p = utf8ToPath(s);
        try {
            if (fs::is_directory(p)) {
                int n = 0;
                auto sfz = lib_.importFolder(p, n);
                log("imported folder " + pathToUtf8(p.filename()) + ": " + std::to_string(n) + " files, " +
                    std::to_string(sfz.size()) + " .sfz");
            } else if (lowerExt(p) == ".wav") {
                log("imported " + pathToUtf8(lib_.importWav(p).filename()));
            } else if (lowerExt(p) == ".json") {
                const fs::path f = lib_.importPreset(p);
                log("imported preset " + pathToUtf8(f.filename()));
                loadPreset(f);
            } else if (lowerExt(p) == ".sfz") {
                int n = 0;
                lib_.importFolder(p.parent_path(), n);
                log("imported SFZ folder " + pathToUtf8(p.parent_path().filename()) + " (" + std::to_string(n) + " files)");
            } else {
                log("unsupported file: " + s, 1);
            }
        } catch (const std::exception& e) {
            log(std::string("import failed: ") + e.what(), 2);
        }
    }
    refreshLists();
}

// ------------------------------------------------------------------ frame

void App::frame() {
    {
        std::vector<std::function<void()>> q;
        {
            std::lock_guard<std::mutex> lock(uiMutex_);
            q.swap(uiQueue_);
        }
        for (auto& f : q) f();
    }
    const double now = ImGui::GetTime();
    if (now - statsTime_ > 0.25) {
        statsTime_ = now;
        engineStats_ = engine_.stats();
        audioStats_ = audio_.stats();
        engine_.collectGarbage();
        const int pc = midi_.takeProgramChange();
        if (pc >= 0 && !presets_.empty()) {
            log("MIDI program change " + std::to_string(pc));
            loadPreset(presets_[(size_t)pc % presets_.size()].file);
        }
    }

    handlePcKeyboard();
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redo(); else undo(); }
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("HSynth", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawTopBar();
    if (ImGui::BeginTabBar("tabs")) {
        auto flag = [&](int i) { return forceTab_ == i ? ImGuiTabItemFlags_SetSelected : 0; };
        if (ImGui::BeginTabItem("Play", nullptr, flag(0))) { drawPlay(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Edit", nullptr, flag(1))) { drawEdit(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Browser", nullptr, flag(4))) { drawBrowser(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Multi", nullptr, flag(5))) { drawMulti(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Seq", nullptr, flag(6))) { drawSeq(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Library", nullptr, flag(2))) { drawLibrary(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Monitor", nullptr, flag(3))) { drawMonitor(); ImGui::EndTabItem(); }
        forceTab_ = -1;
        ImGui::EndTabBar();
    }
    ImGui::End();
    if (autotest_) autotestStep();
}

// ------------------------------------------------------------------ autotest

void App::startAutotest(const fs::path& outDir) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    autotestDir_ = outDir;
    autotest_ = true;
    autotestStage_ = 0;
    autotestStart_ = -1;
    log("autotest: output to " + pathToUtf8(outDir));
}

void App::autotestStep() {
    const double now = ImGui::GetTime();
    if (autotestStart_ < 0) autotestStart_ = now;
    const double t = now - autotestStart_;
    autotestPeakVoices_ = std::max(autotestPeakVoices_, engineStats_.activeVoices);
    autotestPeakLoad_ = std::max(autotestPeakLoad_, engineStats_.cpuLoadPeak);
    auto shot = [&](const char* name) { screenshotRequest_ = pathToUtf8(autotestDir_ / name); };
    auto chord = [&](bool on, int base) {
        const int keys[] = {base, base + 4, base + 7, base + 12};
        for (int i = 0; i < 4; ++i) {
            const int k = keys[i];
            if (on) { held_[k]++; engine_.sendEvent(Event::noteOn(0x400000u + (uint32_t)k, k, 0.8f)); }
            else { held_[k] = std::max(0, held_[k] - 1); engine_.sendEvent(Event::noteOff(0x400000u + (uint32_t)k, k)); }
        }
    };
    auto findPreset = [&](const char* part) -> fs::path {
        for (auto& e : presets_)
            if (pathToUtf8(e.file.filename()).find(part) != std::string::npos) return e.file;
        return {};
    };
    // stage table: {time, action}
    switch (autotestStage_) {
        case 0: if (t > 1.0) { forceTab_ = 0; audio_.startRecording(60.0); engine_.resetPeakStats(); audio_.resetPeaks(); ++autotestStage_; } break;
        case 1: if (t > 1.5) { chord(true, 60); ++autotestStage_; } break;
        case 2: if (t > 2.2) { shot("1_play_va_chord.bmp"); ++autotestStage_; } break;
        case 3: if (t > 3.0) { chord(false, 60); if (auto p = findPreset("hybrid_piano"); !p.empty()) loadPreset(p); ++autotestStage_; } break;
        case 4: if (t > 4.0 && !busy_) { chord(true, 55); ++autotestStage_; } break;
        case 5: if (t > 4.5) { engine_.sendEvent(Event::control(EventType::ModWheel, 1.0f)); ++autotestStage_; } break;
        case 6: if (t > 5.0) { shot("2_play_hybrid_pcm_fm.bmp"); ++autotestStage_; } break;
        case 7: if (t > 6.0) { chord(false, 55); engine_.sendEvent(Event::control(EventType::ModWheel, 0.0f));
                               if (auto p = findPreset("drum_kit"); !p.empty()) loadPreset(p); ++autotestStage_; } break;
        case 8: if (t > 7.0 && !busy_) { chord(true, 36); ++autotestStage_; } break;   // 36 kick, 40 snare, 43, 48
        case 9: if (t > 7.4) { chord(false, 36); forceTab_ = 1; forceEditPage_ = 2; ++autotestStage_; } break;
        case 10: if (t > 8.0) { shot("3_edit.bmp"); forceTab_ = 2; ++autotestStage_; } break;
        case 11: if (t > 8.6) { shot("4_library.bmp"); forceTab_ = 3; ++autotestStage_; } break;
        case 12: if (t > 9.4) { shot("5_monitor.bmp"); if (auto p = findPreset("amp_lead"); !p.empty()) loadPreset(p); ++autotestStage_; } break;
        case 13:
            if (t > 10.2 && !busy_) {
                chord(true, 52);
                forceTab_ = 1;
                for (int i = 0; i < (int)kPages.size(); ++i)
                    if (std::string(kPages[(size_t)i].title) == "FX") forceEditPage_ = i;
                ++autotestStage_;
            }
            break;
        case 14: if (t > 11.2) { shot("6_fx_chain.bmp"); chord(false, 52); if (auto p = findPreset("trance_gate"); !p.empty()) loadPreset(p); ++autotestStage_; } break;
        case 15: if (t > 11.8 && !busy_) { chord(true, 57); engine_.looperCommand(fx::LooperCmd::Tap); ++autotestStage_; } break;
        case 16: if (t > 13.0) { engine_.looperCommand(fx::LooperCmd::Tap); chord(false, 57);
                                 if (auto p = findPreset("shimmer"); !p.empty()) loadPreset(p); ++autotestStage_; } break;
        case 17: if (t > 13.6 && !busy_) { chord(true, 60); ++autotestStage_; } break;
        case 18: if (t > 14.6) { shot("7_fx_looper_shimmer.bmp"); chord(false, 60); ++autotestStage_; } break;
        case 19: if (t > 15.2) { forceTab_ = 4; std::snprintf(browserSearch_, sizeof browserSearch_, "#fm"); browserDirty_ = true; ++autotestStage_; } break;
        case 20:
            if (t > 16.0) {
                shot("8_browser.bmp");
                // multi: part 1 below C4, part 2 (FM e-piano, same MIDI channel) from C4 up = keyboard split
                setMultiMode(true);
                engine_.setPartMix(0, Mix::KeyHigh, 59.0f);
                engine_.setPartMix(1, Mix::Channel, 1.0f);
                engine_.setPartMix(1, Mix::KeyLow, 60.0f);
                if (auto p = findPreset("fm6_epiano"); !p.empty()) loadPresetInto(p, 1);
                forceTab_ = 5;
                ++autotestStage_;
            }
            break;
        case 21:
            if (t > 17.0 && !busy_) {
                const int keys[] = {43, 47, 50, 64, 67, 71};
                for (int k : keys) {
                    Event e = Event::noteOn(0x500000u + (uint32_t)k, k, 0.8f);
                    e.channel = 0;
                    engine_.sendEvent(e);
                }
                ++autotestStage_;
            }
            break;
        case 22:
            if (t > 17.8) {
                shot("9_multi_split.bmp");
                log("multi split: part 1 " + std::to_string(engine_.partActiveVoices(0)) + " voices, part 2 " +
                    std::to_string(engine_.partActiveVoices(1)) + " voices");
                const int keys[] = {43, 47, 50, 64, 67, 71};
                for (int k : keys) {
                    Event e = Event::noteOff(0x500000u + (uint32_t)k, k);
                    e.channel = 0;
                    engine_.sendEvent(e);
                }
                ++autotestStage_;
            }
            break;
        case 23:
            if (t > 18.6) {
                // step sequencer on part 1 (selected), free mode with the transport
                engine_.selectPart(0);
                engine_.setPartParam(0, P::SeqOn, 1.0f);
                engine_.setPartParam(0, P::SeqSteps, 8.0f);
                for (int s = 0; s < 8; ++s) {
                    engine_.setPartParam(0, P::seqStep(s, P::SeqVel), s % 2 ? 0.6f : 0.9f);
                    engine_.setPartParam(0, P::seqStep(s, P::SeqNote), (float)(48 + (s * 5) % 12));
                }
                setTransport(true);
                forceTab_ = 6;
                ++autotestStage_;
            }
            break;
        case 24:
            if (t > 20.0) {
                shot("10_seq.bmp");
                log("sequencer: step " + std::to_string(engine_.seqPosition(0)) + ", voices " +
                    std::to_string(engine_.stats().activeVoices));
                setTransport(false);
                engine_.setPartParam(0, P::SeqOn, 0.0f);
                ++autotestStage_;
            }
            break;
        case 25:
            if (t > 21.0) {
                std::vector<float> inter;
                const size_t frames = audio_.stopRecording(inter);
                std::vector<float> l(frames), r(frames);
                float peak = 0;
                double energy = 0;
                for (size_t i = 0; i < frames; ++i) {
                    l[i] = inter[2 * i];
                    r[i] = inter[2 * i + 1];
                    peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
                    energy += (double)l[i] * l[i];
                }
                std::string err;
                const bool wavOk = writeWav(pathToUtf8(autotestDir_ / "autotest_output.wav"), l, &r, audioStats_.sampleRate, true, err);
                const auto as = audio_.stats();
                std::ofstream rep(autotestDir_ / "report.txt");
                rep << "HSynth desktop autotest\n";
                rep << "audio: " << as.backend << " / " << as.deviceName << " " << as.sampleRate << " Hz period " << as.periodFrames
                    << " x" << as.periods << (as.exclusive ? " exclusive" : " shared") << "\n";
                rep << "recorded frames: " << frames << " (" << (as.sampleRate ? (double)frames / as.sampleRate : 0) << " s)\n";
                rep << "output peak: " << peak << "  rms: " << (frames ? std::sqrt(energy / (double)frames) : 0) << "\n";
                rep << "callbacks: " << as.callbacks << "  late callbacks: " << as.lateCallbacks << "\n";
                rep << "callback avg us: " << as.callbackUsAvg << "  max us: " << as.callbackUsMax << "  peak load: " << as.callbackLoadMax << "\n";
                rep << "engine peak load: " << autotestPeakLoad_ << "  peak voices: " << autotestPeakVoices_ << "\n";
                {
                    int chain[fx::kFxSlots];
                    const int nc = engine_.fxChain(chain);
                    rep << "fx chain (last preset):";
                    for (int i = 0; i < nc; ++i) rep << " " << fx::blockKey(chain[i]);
                    const auto ls = engine_.looperStatus();
                    rep << "\nlooper: state " << (int)ls.state << " length " << ls.lengthSec << " s\n";
                }
                rep << "wav written: " << (wavOk ? "yes" : err) << "\n";
                rep << "result: " << (frames > 0 && peak > 0.01f && as.running ? "PASS (audio produced)" : "FAIL") << "\n";
                rep << "\nlog:\n";
                for (auto& lnn : logLines_) rep << (lnn.level == 2 ? "ERROR " : lnn.level == 1 ? "warn  " : "") << lnn.text << "\n";
                quitRequested_ = true;
                ++autotestStage_;
            }
            break;
        default: break;
    }
}

void App::drawTopBar() {
    if (ImGui::Button("<")) stepPreset(-1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * dpiScale_);
    if (ImGui::BeginCombo("##preset", currentPresetName_.c_str())) {
        for (auto& e : presets_) {
            const std::string label = e.title + (e.user ? "  (user)" : "") + "##" + pathToUtf8(e.file);
            if (ImGui::Selectable(label.c_str(), e.file == currentPreset_)) loadPreset(e.file);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(">")) stepPreset(1);
    ImGui::SameLine();
    float vol = engine_.getParam(P::MasterVolume);
    ImGui::SetNextItemWidth(160 * dpiScale_);
    if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f, "%.2f")) engine_.setParam(P::MasterVolume, vol);
    ImGui::SameLine();
    {
        const bool playing = engine_.transportPlaying();
        if (playing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.3f, 1.0f));
        if (ImGui::Button(playing ? "Stop##transport" : "Play##transport")) setTransport(!playing);
        if (playing) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("transport: step sequencers (free mode), MIDI clock out");
        ImGui::SameLine();
        float tempo = engine_.getParam(P::fxChain(P::FxTempo));
        ImGui::SetNextItemWidth(90 * dpiScale_);
        if (ImGui::DragFloat("##tempo", &tempo, 0.2f, 20, 300, "%.1f bpm")) engine_.setParam(P::fxChain(P::FxTempo), tempo);
        ImGui::SameLine();
        ImGui::BeginDisabled(undo_.empty());
        if (ImGui::Button("Undo")) undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(redo_.empty());
        if (ImGui::Button("Redo")) redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (comparing_) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.1f, 1.0f));
        if (ImGui::Button(comparing_ ? "Compare: saved" : "Compare")) toggleCompare();
        if (comparing_) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("switch between your edit and the preset as loaded");
    }
    ImGui::SameLine();
    if (ImGui::Button("Panic")) {
        engine_.sendEvent(Event::control(EventType::AllSoundOff, 0));
        for (auto& h : held_) h = 0;
    }
    ImGui::SameLine();
    const auto& es = engineStats_;
    const auto& as = audioStats_;
    const bool hot = es.cpuLoadPeak > 0.8f || as.lateCallbacks > 0;
    ImGui::TextColored(hot ? kWarn : kDim, "  voices %d/%d   DSP %.0f%% (peak %.0f%%)   late cb %d   %s %d Hz / %d   %s",
                       es.activeVoices, engine_.maxVoices(), es.cpuLoad * 100, es.cpuLoadPeak * 100, as.lateCallbacks,
                       as.running ? "audio" : "AUDIO OFF", as.sampleRate, as.periodFrames, busy_ ? "loading..." : "");
    if (engine_.multiTimbral()) {
        ImGui::TextColored(kAccent, "Multi: presets load into part %d", engine_.selectedPart() + 1);
        if (currentScene_ >= 0) {
            ImGui::SameLine();
            ImGui::TextColored(kDim, "   scene %d  %s", currentScene_ + 1, scenes_[currentScene_].name.c_str());
        }
    } else if (!presetComment_.empty()) {
        ImGui::TextColored(kDim, "%s", presetComment_.c_str());
    }
}

// ------------------------------------------------------------------ play

void App::handlePcKeyboard() {
    ImGuiIO& io = ImGui::GetIO();
    const bool typing = io.WantTextInput;
    for (int i = 0; i < kNumPcKeys; ++i) {
        const PcKey& k = kPcKeys[i];
        if (!typing && !io.KeyCtrl && ImGui::IsKeyPressed(k.key, false) && !pcHeld_[i]) {
            const int note = std::max(0, std::min(127, (s_.pcOctave + 1) * 12 + k.semitone));
            pcHeld_[i] = true;
            pcHeldKey_[i] = note;
            held_[note]++;
            noteEvent(true, 0x300000u + (uint32_t)i, note, s_.pcVelocity);
        }
        if (pcHeld_[i] && !ImGui::IsKeyDown(k.key)) {
            pcHeld_[i] = false;
            held_[pcHeldKey_[i]] = std::max(0, held_[pcHeldKey_[i]] - 1);
            noteEvent(false, 0x300000u + (uint32_t)i, pcHeldKey_[i], 0.5f);
        }
    }
    if (typing || io.KeyCtrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) s_.pcOctave = std::max(0, s_.pcOctave - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_X, false)) s_.pcOctave = std::min(8, s_.pcOctave + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_C, true)) s_.pcVelocity = std::max(0.05f, s_.pcVelocity - 0.05f);
    if (ImGui::IsKeyPressed(ImGuiKey_V, true)) s_.pcVelocity = std::min(1.0f, s_.pcVelocity + 0.05f);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        sustain_ = true;
        engine_.sendEvent(Event::control(EventType::Sustain, 1.0f));
    }
    if (ImGui::IsKeyReleased(ImGuiKey_Space)) {
        sustain_ = false;
        engine_.sendEvent(Event::control(EventType::Sustain, 0.0f));
    }
}

void App::drawPlay() {
    const float d = dpiScale_;
    drawSceneBar();
    drawMacros();
    if (ImGui::Button("Oct -")) s_.firstNote = std::max(12, s_.firstNote - 12);
    ImGui::SameLine();
    if (ImGui::Button("Oct +")) s_.firstNote = std::min(96, s_.firstNote + 12);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * d);
    ImGui::SliderInt("white keys", &s_.whiteKeys, 7, 52);
    ImGui::SameLine();
    ImGui::Checkbox("Slide = per-note bend (MPE style)", &s_.slideBend);
    ImGui::SameLine();
    if (ImGui::Checkbox("Hold (sustain)", &sustain_)) engine_.sendEvent(Event::control(EventType::Sustain, sustain_ ? 1.0f : 0.0f));
    ImGui::TextColored(kDim,
                       "PC keys: A W S E D F T G Y H U J K O L P ;  = C%d..   Z/X octave   C/V velocity (%.2f)   Space sustain   "
                       "Mouse: click height = velocity, drag up/down = per-note timbre",
                       s_.pcOctave, s_.pcVelocity);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float stripW = 56 * d;
    const float h = std::max(120.0f * d, avail.y - 4 * d);

    // pitch bend (springs back) and mod wheel
    ImGui::VSliderFloat("##bend", ImVec2(stripW, h), &bend_, -1.0f, 1.0f, "Bend\n%.2f");
    if (ImGui::IsItemActive()) engine_.sendEvent(Event::control(EventType::PitchBend, bend_));
    else if (bend_ != 0.0f) { bend_ = 0.0f; engine_.sendEvent(Event::control(EventType::PitchBend, 0.0f)); }
    ImGui::SameLine();
    if (ImGui::VSliderFloat("##mod", ImVec2(stripW, h), &mod_, 0.0f, 1.0f, "Mod\n%.2f"))
        engine_.sendEvent(Event::control(EventType::ModWheel, mod_));
    ImGui::SameLine();
    drawKeyboard(ImGui::GetContentRegionAvail().x, h);
}

void App::drawKeyboard(float width, float height) {
    const int whites = std::max(7, s_.whiteKeys);
    auto whiteToNote = [&](int index) {
        int n = s_.firstNote, i = 0;
        while (i < index) { ++n; if (!isBlack(n)) ++i; }
        return n;
    };
    ImGui::InvisibleButton("##keyboard", ImVec2(width, height));
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const float ww = width / (float)whites;
    auto noteAt = [&](float x, float y) {
        const int wi = std::max(0, std::min(whites - 1, (int)((x - p0.x) / ww)));
        const int wn = whiteToNote(wi);
        if (y - p0.y < height * 0.62f) {
            const float local = x - p0.x - (float)wi * ww;
            if (local > ww * 0.68f && isBlack(wn + 1) && wi < whites - 1) return wn + 1;
            if (local < ww * 0.32f && wn - 1 >= s_.firstNote && isBlack(wn - 1)) return wn - 1;
        }
        return wn;
    };
    const ImVec2 m = ImGui::GetIO().MousePos;
    auto startNote = [&](int note) {
        const float keyH = isBlack(note) ? height * 0.62f : height;
        const float vel = std::max(0.05f, std::min(1.0f, 0.25f + 0.75f * (m.y - p0.y) / keyH));
        mouseNote_ = note;
        mouseNoteId_ = nextNoteId_++;
        mouseDownX_ = m.x;
        lastTimbre_ = -1;
        held_[note]++;
        noteEvent(true, mouseNoteId_, note, vel);
    };
    auto stopNote = [&] {
        if (mouseNote_ < 0) return;
        held_[mouseNote_] = std::max(0, held_[mouseNote_] - 1);
        noteEvent(false, mouseNoteId_, mouseNote_, 0.5f);
        mouseNote_ = -1;
    };
    if (ImGui::IsItemActivated()) startNote(noteAt(m.x, m.y));
    if (ImGui::IsItemActive() && mouseNote_ >= 0) {
        const float cx = std::max(p0.x, std::min(p0.x + width - 1, m.x));
        const float cy = std::max(p0.y, std::min(p0.y + height - 1, m.y));
        if (!s_.slideBend) {
            const int n = noteAt(cx, cy);
            if (n != mouseNote_) { stopNote(); startNote(n); }
        } else {
            engine_.sendEvent(Event::perNote(EventType::NotePitch, mouseNoteId_, mouseNote_, (m.x - mouseDownX_) / ww * 1.7f));
            engine_.sendEvent(Event::perNote(EventType::NoteX, mouseNoteId_, mouseNote_,
                                             std::max(-1.0f, std::min(1.0f, (m.x - mouseDownX_) / ww))));
        }
        const float timbre = 1.0f - (cy - p0.y) / height;
        if (std::fabs(timbre - lastTimbre_) > 0.004f) {
            engine_.sendEvent(Event::perNote(EventType::NoteTimbre, mouseNoteId_, mouseNote_, timbre));
            lastTimbre_ = timbre;
        }
    }
    if (ImGui::IsItemDeactivated()) stopNote();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 white = IM_COL32(236, 238, 242, 255), whiteOn = IM_COL32(120, 200, 245, 255);
    const ImU32 black = IM_COL32(24, 26, 32, 255), blackOn = IM_COL32(40, 140, 200, 255);
    const ImU32 border = IM_COL32(60, 64, 72, 255), label = IM_COL32(90, 96, 110, 255);
    for (int i = 0; i < whites; ++i) {
        const int n = whiteToNote(i);
        const ImVec2 a(p0.x + (float)i * ww, p0.y), b(p0.x + (float)(i + 1) * ww, p0.y + height);
        dl->AddRectFilled(a, b, n < 128 && held_[n] > 0 ? whiteOn : white);
        dl->AddRect(a, b, border);
        if (n % 12 == 0) {
            const std::string t = noteName(n);
            dl->AddText(ImVec2(a.x + 4, b.y - ImGui::GetFontSize() - 4), label, t.c_str());
        }
    }
    for (int i = 0; i < whites - 1; ++i) {
        const int n = whiteToNote(i);
        if (!isBlack(n + 1)) continue;
        const float cx = p0.x + (float)(i + 1) * ww;
        dl->AddRectFilled(ImVec2(cx - ww * 0.32f, p0.y), ImVec2(cx + ww * 0.32f, p0.y + height * 0.62f),
                          n + 1 < 128 && held_[n + 1] > 0 ? blackOn : black, 4.0f);
    }
}

// ------------------------------------------------------------------ edit

void App::drawParam(const ParamInfo& info, int id) {
    ImGui::PushID(id);
    float v = getP(id);
    bool changed = false;
    ImGui::SetNextItemWidth(std::min(520.0f * dpiScale_, ImGui::GetContentRegionAvail().x - 200 * dpiScale_));
    switch (info.kind) {
        case ParamKind::Enum: {
            int idx = (int)std::lround(v);
            const char* preview = idx >= 0 && idx < (int)info.options.size() ? info.options[(size_t)idx].c_str() : "?";
            if (ImGui::BeginCombo(info.label.c_str(), preview)) {
                for (int k = 0; k < (int)info.options.size(); ++k)
                    if (ImGui::Selectable(info.options[(size_t)k].c_str(), k == idx)) { v = (float)k; changed = true; }
                ImGui::EndCombo();
            }
            break;
        }
        case ParamKind::Bool: {
            bool b = v > 0.5f;
            if (ImGui::Checkbox(info.label.c_str(), &b)) { v = b ? 1.0f : 0.0f; changed = true; }
            break;
        }
        case ParamKind::Int: {
            int i = (int)std::lround(v);
            const std::string fmt = "%d " + info.unit;
            if (ImGui::SliderInt(info.label.c_str(), &i, (int)info.min, (int)info.max, fmt.c_str())) { v = (float)i; changed = true; }
            break;
        }
        default: {
            const std::string fmt = (std::fabs(info.max) >= 100 ? "%.1f " : "%.3f ") + info.unit;
            const ImGuiSliderFlags fl = info.curve == ParamCurve::Exp && info.min > 0 ? ImGuiSliderFlags_Logarithmic : 0;
            changed = ImGui::SliderFloat(info.label.c_str(), &v, info.min, info.max, fmt.c_str(), fl);
            break;
        }
    }
    if (paramContextMenu(info, id, v)) changed = true;
    if (changed) setP(id, v);
    ImGui::PopID();
}

void App::drawEdit() {
    const auto& table = paramTable();
    if (ImGui::BeginTabBar("pages")) {
        for (int i = 0; i < (int)kPages.size(); ++i) {
            const ImGuiTabItemFlags fl = forceEditPage_ == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(kPages[(size_t)i].title, nullptr, fl)) { editPage_ = i; ImGui::EndTabItem(); }
        }
        forceEditPage_ = -1;
        ImGui::EndTabBar();
    }
    const EditPage& page = kPages[(size_t)editPage_];
    if (page.groups.empty()) {
        drawFx();
        return;
    }
    const std::string layerName = std::string("Layer ") + (char)('A' + editLayer_);
    if (page.perLayer) {
        ImGui::Text("Layer:");
        for (int l = 0; l < 3; ++l) {
            ImGui::SameLine();
            const std::string b = std::string(1, (char)('A' + l));
            if (ImGui::RadioButton(b.c_str(), editLayer_ == l)) editLayer_ = l;
        }
        if (engine_.multiTimbral()) {
            ImGui::SameLine();
            ImGui::TextColored(kAccent, "   editing part %d (%s)", editPart() + 1, partNames_[editPart()].c_str());
        }
        const int typeId = P::layer(editLayer_, P::LType);
        const int type = (int)std::lround(getP(typeId));
        const std::string expected = page.title;
        const std::string typeName = table[(size_t)typeId].options[(size_t)type];
        std::string want = expected;
        for (auto& c : want) c = (char)std::tolower((unsigned char)c);
        drawParam(table[(size_t)typeId], typeId);
        if (typeName != want) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "layer is '%s' - switch Type to '%s' to hear these", typeName.c_str(), want.c_str());
        }
        if (expected == "WT") {
            const std::string name = engine_.partSampleSetName(editPart(), editLayer_);
            ImGui::Text("User table: %s", name.empty() ? "(none - built-in tables only)" : name.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Load .wav...")) {
                const auto files = platform::openFiles({{"Wavetable (*.wav)", "*.wav"}}, false);
                if (!files.empty()) loadWavetable(editLayer_, utf8ToPath(files.front()));
            }
            ImGui::SameLine();
            if (ImGui::Button("Unload##wt")) { engine_.setPartSampleSet(editPart(), editLayer_, nullptr); engine_.collectGarbage(); }
            ImGui::TextColored(kDim, "Serum-style 2048-sample frames (up to 256) or one single cycle of any length; select Table 'user'");
        }
        if (expected == "PCM") {
            const std::string name = engine_.partSampleSetName(editPart(), editLayer_);
            ImGui::Text("Samples: %s", name.empty() ? "(none)" : name.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Choose...")) ImGui::OpenPopup("choose_samples");
            ImGui::SameLine();
            if (ImGui::Button("Unload")) { engine_.setPartSampleSet(editPart(), editLayer_, nullptr); engine_.collectGarbage(); }
            if (ImGui::BeginPopup("choose_samples")) {
                for (auto& e : samples_)
                    if (ImGui::Selectable(e.title.c_str())) loadSamples(editLayer_, e.file);
                ImGui::EndPopup();
            }
        }
        ImGui::Separator();
    }
    ImGui::BeginChild("params", ImVec2(0, 0));
    for (std::string g : page.groups) {
        if (const auto pos = g.find("%L"); pos != std::string::npos) g.replace(pos, 2, layerName);
        ImGui::TextColored(kAccent, "%s", g.c_str());
        if (g == "Mod Matrix") ImGui::TextColored(kDim, "destination += source x via x amount   (via 'none' = 1)");
        for (int i = 0; i < (int)table.size(); ++i) {
            const ParamInfo& info = table[(size_t)i];
            if (info.group != g) continue;
            if (page.perLayer && info.name.size() > 5 && info.name.compare(info.name.size() - 5, 5, ".type") == 0) continue;
            drawParam(info, i);
            if (g == "Mod Matrix" && info.name.find(".amt") != std::string::npos) ImGui::Spacing();
        }
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ effects

void App::drawFx() {
    const auto& table = paramTable();
    const float d = dpiScale_;
    ImGui::BeginChild("fx", ImVec2(0, 0));

    // ---- chain
    ImGui::TextColored(kAccent, "Effect chain");
    ImGui::SameLine();
    ImGui::TextColored(kDim, "signal flows top to bottom; each block appears once");
    int blocks[fx::kFxSlots];
    for (int s = 0; s < fx::kFxSlots; ++s) blocks[s] = (int)std::lround(engine_.getParam(P::fxSlot(s)));
    bool seen[fx::BlockCount] = {};
    auto swapSlots = [&](int a, int b) {
        engine_.setParam(P::fxSlot(a), (float)blocks[b]);
        engine_.setParam(P::fxSlot(b), (float)blocks[a]);
    };
    if (ImGui::BeginTable("slots", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        for (int s = 0; s < fx::kFxSlots; ++s) {
            const int b = blocks[s];
            ImGui::TableNextRow();
            ImGui::PushID(s);
            ImGui::TableNextColumn();
            ImGui::Text("%2d", s + 1);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(220 * d);
            if (ImGui::BeginCombo("##blk", b > 0 ? fx::blockTitle(b) : "-")) {
                for (int k = 0; k < fx::BlockCount; ++k)
                    if (ImGui::Selectable(k > 0 ? fx::blockTitle(k) : "-  (empty)", k == b)) {
                        engine_.setParam(P::fxSlot(s), (float)k);
                        if (k > 0) engine_.setParam(P::fxp(k, 0), 1.0f);  // inserting a block switches it on
                    }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            if (b > 0) {
                bool on = engine_.getParam(P::fxp(b, 0)) > 0.5f;
                if (ImGui::Checkbox("on", &on)) engine_.setParam(P::fxp(b, 0), on ? 1.0f : 0.0f);
                if (seen[b]) { ImGui::SameLine(); ImGui::TextColored(kWarn, "duplicate (ignored)"); }
                seen[b] = true;
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(s == 0);
            if (ImGui::ArrowButton("up", ImGuiDir_Up)) swapSlots(s, s - 1);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(s == fx::kFxSlots - 1);
            if (ImGui::ArrowButton("down", ImGuiDir_Down)) swapSlots(s, s + 1);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) engine_.setParam(P::fxSlot(s), 0.0f);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Clear chain"))
        for (int s = 0; s < fx::kFxSlots; ++s) engine_.setParam(P::fxSlot(s), 0.0f);
    ImGui::SameLine();
    int active[fx::kFxSlots];
    const int na = engine_.fxChain(active);
    std::string flow = "running: ";
    for (int i = 0; i < na; ++i) flow += std::string(i ? " > " : "") + fx::blockTitle(active[i]);
    ImGui::TextColored(kDim, "%s", na ? flow.c_str() : "running: (dry)");

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Tempo / macros");
    for (int k : {P::FxTempo, P::FxRetrig, P::FxMacro1, P::FxMacro2})
        drawParam(table[(size_t)P::fxChain(k)], P::fxChain(k));
    ImGui::TextColored(kDim, "Macros, mod wheel, expression ... drive any FX parameter on the FX Mod page.");

    // ---- block panels in chain order
    bool shown[fx::BlockCount] = {};
    for (int s = 0; s < fx::kFxSlots; ++s) {
        const int b = blocks[s];
        if (b <= 0 || shown[b]) continue;
        shown[b] = true;
        ImGui::Spacing();
        ImGui::PushID(1000 + b);
        const bool on = engine_.getParam(P::fxp(b, 0)) > 0.5f;
        const std::string head = std::to_string(s + 1) + ". " + fx::blockTitle(b) + (on ? "" : "   (off)") + "###blk";
        if (ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(12 * d);
            for (int k = 0; k < fx::kBlockParams[b]; ++k) drawParam(table[(size_t)P::fxp(b, k)], P::fxp(b, k));
            if (b == fx::Cab || b == fx::Convo) {
                const int target = b == fx::Cab ? 0 : 1;
                const std::string name = engine_.fxImpulseName(target);
                ImGui::Text("User IR: %s", name.empty() ? "(none)" : name.c_str());
                ImGui::SameLine();
                if (ImGui::Button("Load IR (.wav)...")) {
                    const auto files = platform::openFiles({{"Impulse response (*.wav)", "*.wav"}}, false);
                    if (!files.empty()) loadImpulse(target, utf8ToPath(files.front()));
                }
                if (!name.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Unload")) engine_.setFxImpulse(target, nullptr);
                }
                ImGui::TextColored(kDim, target == 0 ? "cabinet IRs up to 0.68 s (mono), select model 'user ir'"
                                                     : "reverb IRs up to 6 s (stereo), select impulse 'user'");
            }
            if (b == fx::Vocoder) {
                const auto& as = audioStats_;
                if (as.input) ImGui::Text("Modulator: %s   input peak %.2f", as.inputName.c_str(), as.inputPeak);
                else ImGui::TextColored(kWarn, "No audio input: enable it on the Monitor tab (Audio input) and speak while holding chords.");
            }
            ImGui::Unindent(12 * d);
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    drawLooper();
    ImGui::EndChild();
}

void App::drawLooper() {
    const auto st = engine_.looperStatus();
    static const char* kState[] = {"empty", "recording", "playing", "overdubbing", "stopped"};
    ImGui::TextColored(kAccent, "Looper");
    ImGui::SameLine();
    ImGui::Text("%s   %.2f / %.2f s", kState[(int)st.state], st.positionSec, st.lengthSec);
    const char* tap = st.state == fx::LooperState::Empty ? "Record"
                      : st.state == fx::LooperState::Recording ? "Close loop"
                      : st.state == fx::LooperState::Playing ? "Overdub"
                      : "Play";
    if (ImGui::Button(tap)) engine_.looperCommand(fx::LooperCmd::Tap);
    ImGui::SameLine();
    if (ImGui::Button("Stop")) engine_.looperCommand(fx::LooperCmd::Stop);
    ImGui::SameLine();
    ImGui::BeginDisabled(!st.canUndo);
    if (ImGui::Button("Undo / Redo")) engine_.looperCommand(fx::LooperCmd::Undo);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) engine_.looperCommand(fx::LooperCmd::Clear);
    if (st.lengthSec > 0) ImGui::ProgressBar(st.positionSec / st.lengthSec, ImVec2(-1, 0), "");
    drawParam(paramTable()[(size_t)P::fxChain(P::LooperLevel)], P::fxChain(P::LooperLevel));
    ImGui::TextColored(kDim, "Records the output after the effect chain (max 60 s).");
}

// ------------------------------------------------------------------ library

void App::drawLog(float height) {
    ImGui::BeginChild("log", ImVec2(0, height), ImGuiChildFlags_Borders);
    for (auto& l : logLines_) {
        if (l.level == 2) ImGui::TextColored(kErr, "%s", l.text.c_str());
        else if (l.level == 1) ImGui::TextColored(kWarn, "%s", l.text.c_str());
        else ImGui::TextUnformatted(l.text.c_str());
    }
    if (logScrollToBottom_) { ImGui::SetScrollHereY(1.0f); logScrollToBottom_ = false; }
    ImGui::EndChild();
}

void App::drawLibrary() {
    const float d = dpiScale_;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float colW = (avail.x - 16 * d) / 3.0f;

    // presets
    ImGui::BeginChild("presets", ImVec2(colW, 0));
    ImGui::TextColored(kAccent, "Presets");
    if (ImGui::Button("Save as...")) {
        std::snprintf(saveName_, sizeof saveName_, "%s", currentPresetName_.c_str());
        ImGui::OpenPopup("save_preset");
    }
    ImGui::SameLine();
    if (ImGui::Button("Import...")) {
        for (auto& f : platform::openFiles({{"HSynth preset (*.json)", "*.json"}}, true)) onDropFiles({f});
    }
    ImGui::SameLine();
    if (ImGui::Button("Export...")) {
        const std::string path = platform::saveFile({{"HSynth preset (*.json)", "*.json"}},
                                                    HostLibrary::safeName(currentPresetName_) + ".json", "json");
        if (!path.empty()) {
            std::string err;
            Preset p = capturePreset(engine_, currentPresetName_, pathToUtf8(lib_.libraryRoot()));
            if (savePresetFile(path, p, err)) log("exported " + path + " (sample paths are library-relative)");
            else log(err, 2);
        }
    }
    if (ImGui::BeginPopup("save_preset")) {
        ImGui::Text("Preset name");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", saveName_, sizeof saveName_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save") || enter) {
            std::string n = saveName_;
            if (n.empty()) n = "Untitled";
            savePreset(n);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::BeginChild("presetlist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    fs::path toDelete;
    for (auto& e : presets_) {
        const std::string label = e.title + "##" + pathToUtf8(e.file);
        if (ImGui::Selectable(label.c_str(), e.file == currentPreset_)) loadPreset(e.file);
        if (e.user && ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete user preset")) toDelete = e.file;
            ImGui::EndPopup();
        }
        ImGui::SameLine(colW - 80 * d);
        ImGui::TextColored(kDim, e.user ? "user" : "factory");
    }
    ImGui::EndChild();
    ImGui::EndChild();
    if (!toDelete.empty()) {
        std::error_code ec;
        fs::remove(toDelete, ec);
        log(ec ? "delete failed: " + ec.message() : "deleted " + pathToUtf8(toDelete.filename()), ec ? 2 : 0);
        refreshLists();
    }

    // samples
    ImGui::SameLine();
    ImGui::BeginChild("samples", ImVec2(colW, 0));
    ImGui::TextColored(kAccent, "Samples (WAV / SFZ)");
    if (ImGui::Button("Import WAV...")) {
        auto files = platform::openFiles({{"WAV audio (*.wav)", "*.wav"}}, true);
        if (!files.empty()) onDropFiles(files);
    }
    ImGui::SameLine();
    if (ImGui::Button("Import SFZ folder...")) {
        const std::string dir = platform::pickFolder("Select the folder that contains the .sfz file");
        if (!dir.empty()) onDropFiles({dir});
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) platform::openInExplorer(pathToUtf8(lib_.libraryRoot()));
    ImGui::TextColored(kDim, "Click A/B/C to load into a layer. Drag & drop WAV / SFZ / folders / presets onto the window.");
    ImGui::BeginChild("samplelist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (auto& e : samples_) {
        ImGui::PushID(pathToUtf8(e.file).c_str());
        for (int l = 0; l < 3; ++l) {
            const std::string b(1, (char)('A' + l));
            if (ImGui::SmallButton(b.c_str())) loadSamples(l, e.file);
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(e.title.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();

    // log
    ImGui::SameLine();
    ImGui::BeginChild("logcol", ImVec2(0, 0));
    ImGui::TextColored(kAccent, "Messages");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) logLines_.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("SFZ support")) ImGui::OpenPopup("sfzinfo");
    if (ImGui::BeginPopup("sfzinfo")) {
        ImGui::Text("Headers: <control> <global> <master> <group> <region>, #define, #include");
        std::string ops;
        int k = 0;
        for (auto& o : supportedSfzOpcodes()) ops += o + (++k % 6 == 0 ? "\n" : ", ");
        ImGui::TextUnformatted(ops.c_str());
        ImGui::TextColored(kDim, "Other opcodes are ignored and reported as warnings. Samples: WAV only.");
        ImGui::EndPopup();
    }
    drawLog(0);
    ImGui::EndChild();
}

// ------------------------------------------------------------------ monitor

void App::drawMonitor() {
    const float d = dpiScale_;
    const auto& es = engineStats_;
    const auto& as = audioStats_;
    ImGui::BeginChild("monleft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0));
    ImGui::TextColored(kAccent, "Audio / DSP status");
    ImGui::Text("stream      %s   %s   %s", as.running ? "running" : "STOPPED", as.backend.c_str(), as.exclusive ? "exclusive" : "shared");
    ImGui::Text("device      %s", as.deviceName.c_str());
    ImGui::Text("format      %d Hz float32 stereo", as.sampleRate);
    ImGui::Text("buffer      period %d x %d  (%.1f ms)   last callback %d frames", as.periodFrames, as.periods,
                as.sampleRate ? 1000.0 * as.periodFrames / as.sampleRate : 0.0, as.lastCallbackFrames);
    ImGui::Text("callback    avg %.0f us   max %.0f us   peak load %.0f %%", as.callbackUsAvg, as.callbackUsMax, as.callbackLoadMax * 100);
    ImGui::TextColored(as.lateCallbacks ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                       "late cb     %d   (callback interval > 2.5 periods: likely dropout)", as.lateCallbacks);
    ImGui::Text("engine DSP  load %.1f %%  peak %.1f %%   render avg %.0f us  max %.0f us", es.cpuLoad * 100, es.cpuLoadPeak * 100,
                es.renderUsAvg, es.renderUsPeak);
    ImGui::Text("voices      %d active  peak %d  limit %d   render threads %d", es.activeVoices, es.peakVoices,
                engine_.maxVoices(), engine_.renderThreads());
    ImGui::Text("output      peak %.2f   dropped events %u", es.outputPeak, es.droppedEvents);
    ImGui::TextColored(es.streamUnderruns ? kWarn : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                       "disk        %d streaming players   %u underruns   %.1f MB read", es.streamVoices, es.streamUnderruns,
                       (double)es.streamBytesRead / 1048576.0);
    if (ImGui::Button("Reset peaks")) { engine_.resetPeakStats(); audio_.resetPeaks(); }

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Audio output");
    ImGui::SetNextItemWidth(360 * d);
    const char* devPreview = s_.deviceIndex >= 0 && s_.deviceIndex < (int)devices_.size() ? devices_[(size_t)s_.deviceIndex].name.c_str() : "System default";
    if (ImGui::BeginCombo("Device", devPreview)) {
        if (ImGui::Selectable("System default", s_.deviceIndex < 0)) { s_.deviceIndex = -1; s_.deviceName.clear(); }
        for (int i = 0; i < (int)devices_.size(); ++i)
            if (ImGui::Selectable((devices_[(size_t)i].name + "##" + std::to_string(i)).c_str(), s_.deviceIndex == i)) {
                s_.deviceIndex = i;
                s_.deviceName = devices_[(size_t)i].name;
            }
        ImGui::EndCombo();
    }
    static const int rates[] = {44100, 48000, 96000};
    ImGui::SetNextItemWidth(160 * d);
    if (ImGui::BeginCombo("Sample rate", std::to_string(s_.sampleRate).c_str())) {
        for (int r : rates) if (ImGui::Selectable(std::to_string(r).c_str(), r == s_.sampleRate)) s_.sampleRate = r;
        ImGui::EndCombo();
    }
    static const int periods[] = {0, 64, 128, 192, 256, 480, 512, 1024};
    ImGui::SetNextItemWidth(160 * d);
    if (ImGui::BeginCombo("Period (frames)", s_.periodFrames ? std::to_string(s_.periodFrames).c_str() : "driver default")) {
        for (int p : periods)
            if (ImGui::Selectable(p ? std::to_string(p).c_str() : "driver default", p == s_.periodFrames)) s_.periodFrames = p;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("WASAPI exclusive mode (lowest latency; other apps are muted)", &s_.exclusive);
    ImGui::Checkbox("Audio input (vocoder modulator)", &s_.audioInput);
    if (s_.audioInput) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260 * d);
        if (ImGui::BeginCombo("##input", s_.inputName.empty() ? "default input" : s_.inputName.c_str())) {
            if (ImGui::Selectable("default input", s_.inputName.empty())) s_.inputName.clear();
            for (auto& dv : inputDevices_)
                if (ImGui::Selectable(dv.name.c_str(), dv.name == s_.inputName)) s_.inputName = dv.name;
            ImGui::EndCombo();
        }
    }
    if (as.input) ImGui::Text("input       %s   peak %.2f", as.inputName.c_str(), as.inputPeak);
    if (ImGui::Button("Apply / restart audio")) startAudio();
    ImGui::SameLine();
    if (ImGui::Button("Rescan devices")) { devices_ = audio_.listDevices(); inputDevices_ = audio_.listInputDevices(); }
    if (!audioError_.empty()) ImGui::TextColored(kErr, "%s", audioError_.c_str());

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Engine");
    static const int voiceOpts[] = {32, 64, 128, 256};
    ImGui::SetNextItemWidth(160 * d);
    if (ImGui::BeginCombo("Polyphony", std::to_string(s_.voices).c_str())) {
        for (int v : voiceOpts)
            if (ImGui::Selectable(std::to_string(v).c_str(), v == s_.voices)) { s_.voices = v; engine_.setMaxVoices(v); }
        ImGui::EndCombo();
    }
    {
        // voices are spread over this many cores (the audio thread + helpers)
        static const int threadOpts[] = {0, 1, 2, 3, 4, 6, 8};
        auto label = [](int t) {
            return t == 0 ? "Auto (" + std::to_string(autoRenderThreads()) + ")" : std::to_string(t);
        };
        ImGui::SetNextItemWidth(160 * d);
        if (ImGui::BeginCombo("Render threads", label(s_.renderThreads).c_str())) {
            for (int t : threadOpts)
                if (ImGui::Selectable(label(t).c_str(), t == s_.renderThreads)) {
                    s_.renderThreads = t;
                    engine_.setRenderThreads(t);
                }
            ImGui::EndCombo();
        }
    }
    static const char* qual[] = {"Low (SBC / power saving)", "Normal", "High"};
    ImGui::SetNextItemWidth(260 * d);
    if (ImGui::Combo("DSP quality", &s_.quality, qual, 3)) {
        engine_.setQuality((Quality)s_.quality);
        // low-power setting: also keep the polyphony at the low-power size
        if (s_.quality == (int)Quality::Low && s_.voices > kLowPowerVoices) {
            s_.voices = kLowPowerVoices;
            engine_.setMaxVoices(s_.voices);
        }
    }
    {
        static const char* modes[] = {"Off (everything in memory)", "Auto (sets > 32 MB)", "Always (long unlooped samples)"};
        ImGui::SetNextItemWidth(260 * d);
        if (ImGui::Combo("Disk streaming", &s_.streamMode, modes, 3)) applyStreamingSettings();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Large sample sets keep only the first 0.34 s of each sample in memory and read the rest\n"
                              "from disk while playing. Applies to sample sets loaded afterwards.");
    }
    ImGui::SetNextItemWidth(160 * d);
    float scale = s_.uiScale > 0 ? s_.uiScale : dpiScale_;
    if (ImGui::SliderFloat("UI scale (restart)", &scale, 0.75f, 3.0f, "%.2f")) s_.uiScale = scale;

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "MIDI input");
    if (ImGui::Checkbox("MPE (ch 1 master, ch 2-16 per-note)", &s_.mpe)) midi_.setMpe(s_.mpe);
    ImGui::SameLine();
    if (ImGui::Button("Rescan MIDI")) midiPorts_ = midi_.openAll();
    if (midiPorts_.empty()) ImGui::TextColored(kDim, "no MIDI input ports");
    for (auto& p : midiPorts_) ImGui::BulletText("%s", p.c_str());
    ImGui::Text("events %d   last %s", midi_.eventCount(), midi_.lastMessage().c_str());
    if (const std::string st = midi_.status(); !st.empty()) ImGui::TextColored(kDim, "%s", st.c_str());
#ifdef __APPLE__
    {
        bool ump = midi_.umpInput();
        if (ImGui::Checkbox("MIDI 2.0 input (UMP)", &ump)) midiPorts_ = (midi_.setUmpInput(ump), midi_.openPorts());
    }
#endif
    if (remote_) {
        const std::string port = remote_->replyPort();
        ImGui::TextColored(kDim, "SysEx remote: %u requests%s%s", remote_->requests(), port.empty() ? "" : ", replies to ",
                           port.c_str());
    }
    drawMidiOut();
    drawMidiMap();
    drawSetups();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("monright", ImVec2(0, 0));
    ImGui::TextColored(kAccent, "Oscilloscope");
    ImGui::SameLine();
    ImGui::Checkbox("freeze", &scopeFreeze_);
    const int n = 1024;
    if (!scopeFreeze_ || scopeL_.empty()) {
        scopeL_.resize(n);
        scopeR_.resize(n);
        audio_.copyScope(scopeL_.data(), scopeR_.data(), n);
    }
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PlotLines("##L", scopeL_.data(), n, 0, "L", -1.0f, 1.0f, ImVec2(w, 140 * d));
    ImGui::PlotLines("##R", scopeR_.data(), n, 0, "R", -1.0f, 1.0f, ImVec2(w, 140 * d));

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Record output to WAV");
    if (!audio_.recording()) {
        if (ImGui::Button("Start recording (max 5 min)")) { audio_.startRecording(300.0); log("recording started"); }
    } else {
        ImGui::TextColored(kErr, "REC %.1f s", audio_.recordedSeconds());
        ImGui::SameLine();
        if (ImGui::Button("Stop and save...")) {
            std::vector<float> inter;
            const size_t frames = audio_.stopRecording(inter);
            char def[64];
            std::time_t t = std::time(nullptr);
            std::strftime(def, sizeof def, "hsynth_%Y%m%d_%H%M%S.wav", std::localtime(&t));
            const std::string path = platform::saveFile({{"WAV (*.wav)", "*.wav"}}, def, "wav");
            if (!path.empty()) {
                std::vector<float> l(frames), r(frames);
                for (size_t i = 0; i < frames; ++i) { l[i] = inter[2 * i]; r[i] = inter[2 * i + 1]; }
                std::string err;
                if (writeWav(path, l, &r, as.sampleRate, true, err)) log("recorded " + std::to_string(frames) + " frames to " + path);
                else log(err, 2);
            }
        }
    }
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Messages");
    drawLog(0);
    ImGui::EndChild();
}
