// Desktop UI: the screens the phone apps have and the desktop lacked before 0.8.6 -
// Player (MIDI files, playlists, 16-channel mixer), Live (set lists), Sample
// (record, trim, loop, save as an instrument), Perform (Super Knob, scale lock,
// chord mode, phrase recorder) and the graphs of the Edit screen (envelopes,
// filter, FM algorithm). Same App class as app.cpp; behaviour follows
// ios/HSynth/PlayerScreen.swift, LiveScreen.swift and PerformScreens.swift.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <thread>
#include <sstream>

#include "app.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/gm_bank.h"
#include "hsynth/host_utils.h"
#include "hsynth/params.h"
#include "hsynth/preset.h"
#include "hsynth/smf.h"
#include "imgui.h"
#include "platform.h"

using namespace hsynth;

namespace {

const ImVec4 kAccent(1.0f, 0.62f, 0.22f, 1.0f);  // LED orange
const ImVec4 kDim(0.63f, 0.65f, 0.70f, 1.0f);  // engraved
const ImVec4 kWarn(1.0f, 0.72f, 0.30f, 1.0f);
const ImU32 kLine = IM_COL32(174, 234, 0, 255);  // LCD
const ImU32 kFill = IM_COL32(174, 234, 0, 36);
const ImU32 kGrid = IM_COL32(44, 62, 22, 255);
const ImU32 kBg = IM_COL32(12, 18, 8, 255);

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << text;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string clock(double s) {
    if (s < 0) s = 0;
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d", (int)s / 60, (int)s % 60);
    return b;
}

std::string noteName(int n) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
}

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

std::string uniqueName() {
    static unsigned counter = 0;
    const auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
    char b[48];
    std::snprintf(b, sizeof b, "%llx%04x", (unsigned long long)ns, ++counter & 0xffff);
    return b;
}

}  // namespace

// ================================================================== player

fs::path App::songsDir() const { return lib_.dataDir() / "midi"; }

void App::refreshSongs() {
    allSongs_.clear();
    std::error_code ec;
    fs::create_directories(songsDir(), ec);
    for (auto it = fs::directory_iterator(songsDir(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        std::string ext = pathToUtf8(it->path().extension());
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".mid" || ext == ".midi") allSongs_.push_back(it->path());
    }
    std::sort(allSongs_.begin(), allSongs_.end(), [](const fs::path& a, const fs::path& b) {
        std::string x = pathToUtf8(a.filename()), y = pathToUtf8(b.filename());
        for (auto& c : x) c = (char)std::tolower((unsigned char)c);
        for (auto& c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    });
    const fs::path cur = songIndex_ >= 0 && songIndex_ < (int)songs_.size() ? songs_[(size_t)songIndex_] : fs::path();
    songs_.clear();
    if (activePlaylist_ >= 0 && activePlaylist_ < (int)playlists_.size()) {
        for (const auto& name : playlists_[(size_t)activePlaylist_].songs)
            for (const auto& f : allSongs_)
                if (pathToUtf8(f.filename()) == name) { songs_.push_back(f); break; }
    } else {
        songs_ = allSongs_;
    }
    songIndex_ = -1;
    for (int i = 0; i < (int)songs_.size(); ++i)
        if (songs_[(size_t)i] == cur) songIndex_ = i;
    songsScanned_ = true;
}

void App::loadPlaylists() {
    playlists_.clear();
    activePlaylist_ = -1;
    Json j;
    std::string err;
    if (!Json::parse(readText(lib_.dataDir() / "playlists.json"), j, err) || !j.isObject()) return;
    if (const Json* lists = j.find("lists"); lists && lists->isArray())
        for (size_t i = 0; i < lists->size(); ++i) {
            const Json& l = lists->at(i);
            Playlist p;
            if (const Json* n = l.find("name")) p.name = n->string();
            if (const Json* s = l.find("songs"); s && s->isArray())
                for (size_t k = 0; k < s->size(); ++k) p.songs.push_back(s->at(k).string());
            playlists_.push_back(std::move(p));
        }
    if (const Json* a = j.find("active")) {
        const int k = (int)a->number(-1);
        activePlaylist_ = k >= 0 && k < (int)playlists_.size() ? k : -1;
    }
}

void App::savePlaylists() {
    Json j = Json::object(), lists = Json::array();
    for (const auto& p : playlists_) {
        Json l = Json::object(), s = Json::array();
        l.set("name", p.name);
        for (const auto& n : p.songs) s.push(n);
        l.set("songs", std::move(s));
        lists.push(std::move(l));
    }
    j.set("active", activePlaylist_);
    j.set("lists", std::move(lists));
    if (!writeText(lib_.dataDir() / "playlists.json", j.dump())) log("cannot save playlists.json", 2);
}

std::string App::loadSongFile(const std::string& path) {
    auto song = std::make_unique<Song>();
    std::string err;
    if (!loadSmf(path, *song, err)) return err;
    // the presets the GM / bank selection may use: factory + user (GM tests, captures)
    PresetIndex idx;
    idx.setFolders({{pathToUtf8(lib_.dataDir() / "presets" / "factory"), false}, {pathToUtf8(lib_.userPresetDir()), true}});
    idx.rescan();
    GmBank bank;
    bank.build(idx.all());
    const std::string root = pathToUtf8(lib_.libraryRoot());
    std::lock_guard<std::mutex> lock(songMutex_);
    if (programs_) programs_->stop();  // before prepareSong: it must not stage into the parts being loaded
    engine_.songPlay(false);
    std::vector<std::string> report;
    std::vector<SongPart> parts;
    std::string loaded[kNumParts];
    prepareSong(engine_, *song, bank, root, loaded, report, &parts);
    std::vector<std::pair<double, double>> tempos;
    for (const auto& t : song->tempos) tempos.push_back({t.time, t.bpm});
    if (tempos.empty()) tempos.push_back({0.0, song->initialTempo});
    const std::string name = song->name.empty() ? pathToUtf8(utf8ToPath(path).stem()) : song->name;
    if (!programs_) programs_ = std::make_unique<SongProgramLoader>();
    programs_->start(engine_, *song, bank, root);
    engine_.setSong(std::move(song));
    engine_.songSeek(0.0);
    post([this, parts, tempos, name, report] {
        songParts_ = parts;
        songTempos_ = tempos;
        songName_ = name;
        for (const auto& r : report) log("  " + r);
        midi_.setMultiTimbral(true);
        for (const auto& p : parts)
            if (p.channel >= 0 && p.channel < kNumParts) partNames_[p.channel] = p.presetName.empty() ? p.gmName : p.presetName;
        currentPresetName_ = "Song: " + name;
        lastSongEnd_ = engine_.songEndCount();
        playerApplyMix();
    });
    return "";
}

void App::playSong(int index) {
    if (index < 0 || index >= (int)songs_.size() || songLoading_) return;
    songIndex_ = index;
    const std::string path = pathToUtf8(songs_[(size_t)index]);
    playerMsg_ = "Loading sounds...";
    songLoading_ = true;
    log("song " + pathToUtf8(songs_[(size_t)index].filename()) + " ...");
    runAsync([this, path] {
        const std::string err = loadSongFile(path);
        post([this, err] {
            songLoading_ = false;
            if (!err.empty()) { playerMsg_ = "Cannot load: " + err; log("song load failed: " + err, 2); return; }
            playerMsg_.clear();
            engine_.songPlay(true);
        });
    });
}

void App::playerNext(bool manual) {
    if (songs_.empty()) return;
    const int n = songIndex_ + 1;
    if (n >= (int)songs_.size()) {
        if (manual || repeatMode_ == 1) playSong(0);
        else { engine_.songPlay(false); engine_.songSeek(0); }
    } else {
        playSong(n);
    }
}

void App::playerApplyMix() {
    bool anySolo = false;
    for (bool s : chSolo_) anySolo |= s;
    for (int k = 0; k < kNumParts; ++k) {
        engine_.setPartMix(k, 2, chMute_[k] || (anySolo && !chSolo_[k]) ? 1.0f : 0.0f);
        if (chVolSet_[k]) engine_.setPartMix(k, 0, chVol_[k]);
    }
}

void App::playerTick() {
    if (!songsScanned_) {
        loadPlaylists();
        refreshSongs();
    }
    const uint32_t ends = engine_.songEndCount();
    if (ends != lastSongEnd_) {
        lastSongEnd_ = ends;
        if (repeatMode_ == 2) { engine_.songSeek(0); engine_.songPlay(true); }
        else playerNext(false);
    }
}

double App::songTempoAt(double t) const {
    double b = songTempos_.empty() ? 120.0 : songTempos_.front().second;
    for (const auto& x : songTempos_) {
        if (x.first > t) break;
        b = x.second;
    }
    return b;
}

void App::drawPlayer() {
    const float d = dpiScale_;
    const double len = engine_.songLength(), pos = engine_.songPosition();
    const bool playing = engine_.songPlaying();

    // display
    ImGui::BeginChild("lcd", ImVec2(0, 64 * d), ImGuiChildFlags_Borders);
    const std::string title = songIndex_ >= 0 && songIndex_ < (int)songs_.size() ? pathToUtf8(songs_[(size_t)songIndex_].stem())
                                                                              : std::string("(no song)");
    ImGui::TextColored(kAccent, "%s", title.c_str());
    ImGui::Text("%s / %s   %.0f BPM   song %d / %d   voices %d   DSP %.0f%%", clock(pos).c_str(), clock(len).c_str(),
                songTempoAt(pos), songIndex_ + 1, (int)songs_.size(), engineStats_.activeVoices, engineStats_.cpuLoad * 100);
    if (!playerMsg_.empty()) ImGui::TextColored(kWarn, "%s", playerMsg_.c_str());
    ImGui::EndChild();

    // transport
    if (ImGui::Button("|<<")) {
        if (pos > 3 || songs_.empty()) engine_.songSeek(0);
        else playSong(songIndex_ <= 0 ? (int)songs_.size() - 1 : songIndex_ - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button(playing ? " Pause " : " Play  ")) {
        if (len <= 0) playSong(std::max(0, songIndex_));
        else engine_.songPlay(!playing);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) { engine_.songPlay(false); engine_.songSeek(0); }
    ImGui::SameLine();
    if (ImGui::Button(">>|")) playerNext(true);
    ImGui::SameLine();
    static const char* rep[3] = {"Repeat: off", "Repeat: all", "Repeat: one"};
    if (ImGui::Button(rep[repeatMode_])) repeatMode_ = (repeatMode_ + 1) % 3;
    ImGui::SameLine();
    float seek = seekTarget_ >= 0 ? seekTarget_ : (float)pos;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##seek", &seek, 0.0f, (float)std::max(1.0, len), clock(seek).c_str())) seekTarget_ = seek;
    if (ImGui::IsItemDeactivatedAfterEdit() && seekTarget_ >= 0) { engine_.songSeek(seekTarget_); seekTarget_ = -1; }
    else if (!ImGui::IsItemActive()) seekTarget_ = -1;

    // view switch
    if (ImGui::RadioButton("Channels", !playerShowList_)) playerShowList_ = false;
    ImGui::SameLine();
    const std::string pl = "Playlist (" + std::to_string(songs_.size()) + ")";
    if (ImGui::RadioButton(pl.c_str(), playerShowList_)) playerShowList_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset Mix")) {
        for (int k = 0; k < kNumParts; ++k) { chMute_[k] = chSolo_[k] = chVolSet_[k] = false; }
        for (int k = 0; k < kNumParts; ++k) engine_.setPartMix(k, 0, 1.0f);
        playerApplyMix();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Songs...")) {
        const auto files = platform::openFiles({{"MIDI files (*.mid)", "*.mid;*.midi"}}, true);
        int added = 0;
        for (const auto& f : files) {
            std::error_code ec;
            fs::copy_file(utf8ToPath(f), songsDir() / utf8ToPath(f).filename(), fs::copy_options::overwrite_existing, ec);
            if (!ec) ++added;
        }
        if (!files.empty()) { refreshSongs(); playerMsg_ = std::to_string(added) + " song(s) added"; }
    }
    ImGui::SameLine();
    if (ImGui::Button("Songs Folder")) platform::openInExplorer(pathToUtf8(songsDir()));
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) refreshSongs();

    if (playerShowList_) {
        drawPlaylist();
        return;
    }

    // 16-channel mixer
    Engine::PartMeter m[kNumParts];
    engine_.partMeters(m);
    if (ImGui::BeginTable("mixer", 12, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingFixedFit, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ch");
        ImGui::TableSetupColumn("M");
        ImGui::TableSetupColumn("S");
        ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Volume", 0, 150 * d);
        ImGui::TableSetupColumn("Level", 0, 90 * d);
        ImGui::TableSetupColumn("Pan");
        ImGui::TableSetupColumn("Exp");
        ImGui::TableSetupColumn("Bend");
        ImGui::TableSetupColumn("Mod");
        ImGui::TableSetupColumn("Rev / Cho");
        ImGui::TableSetupColumn("Keys", 0, 200 * d);
        ImGui::TableHeadersRow();
        for (int k = 0; k < kNumParts; ++k) {
            ImGui::PushID(k);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%2d", k + 1);
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##m", &chMute_[k])) playerApplyMix();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##s", &chSolo_[k])) playerApplyMix();
            ImGui::TableNextColumn();
            std::string name, gm;
            for (const auto& p : songParts_)
                if (p.channel == k) { name = p.presetName; gm = p.drum ? "Drums" : p.gmName; }
            {
                std::lock_guard<std::mutex> lock(songMutex_);
                if (programs_) { const std::string now = programs_->presetName(k); if (!now.empty()) name = now; }
            }
            if (name.empty() && gm.empty()) ImGui::TextColored(kDim, "-");
            else ImGui::Text("%s%s%s", name.c_str(), gm.empty() ? "" : "   ", gm.c_str());
            ImGui::TableNextColumn();
            float v = chVolSet_[k] ? chVol_[k] : m[k].volume;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##v", &v, 0.0f, 1.0f, "%.2f")) {
                chVol_[k] = v;
                chVolSet_[k] = true;
                engine_.setPartMix(k, 0, v);
            }
            ImGui::TableNextColumn();
            chLevel_[k] = std::max(m[k].level, chLevel_[k] * 0.85f);
            ImGui::ProgressBar(std::min(1.0f, chLevel_[k]), ImVec2(-1, 0), "");
            ImGui::TableNextColumn();
            ImGui::Text("%+.2f", m[k].pan);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", m[k].expression);
            ImGui::TableNextColumn();
            ImGui::Text("%+.2f", m[k].bend);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", m[k].mod);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f / %.2f", m[k].reverb, m[k].chorus);
            ImGui::TableNextColumn();
            // the keys sounding: a strip of 128 cells
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight();
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), kBg);
            for (int n = 0; n < 128; ++n)
                if (m[k].notes[n / 32] & (1u << (n % 32))) {
                    const float x = p0.x + w * (float)n / 128.0f;
                    dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + std::max(2.0f, w / 128.0f), p0.y + h), kLine);
                }
            ImGui::Dummy(ImVec2(w, h));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void App::drawPlaylist() {
    // list chooser
    const char* cur = activePlaylist_ >= 0 && activePlaylist_ < (int)playlists_.size()
                          ? playlists_[(size_t)activePlaylist_].name.c_str()
                          : "All songs";
    ImGui::SetNextItemWidth(260 * dpiScale_);
    if (ImGui::BeginCombo("##list", cur)) {
        if (ImGui::Selectable("All songs", activePlaylist_ < 0)) { activePlaylist_ = -1; refreshSongs(); savePlaylists(); }
        for (int i = 0; i < (int)playlists_.size(); ++i)
            if (ImGui::Selectable((playlists_[(size_t)i].name + "##" + std::to_string(i)).c_str(), i == activePlaylist_)) {
                activePlaylist_ = i;
                refreshSongs();
                savePlaylists();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * dpiScale_);
    ImGui::InputTextWithHint("##plname", "playlist name", playlistName_, sizeof playlistName_);
    ImGui::SameLine();
    if (ImGui::Button("+ New Playlist")) {
        std::string n = playlistName_;
        if (n.empty()) n = "Playlist " + std::to_string(playlists_.size() + 1);
        playlists_.push_back({n, {}});
        activePlaylist_ = (int)playlists_.size() - 1;
        playlistName_[0] = 0;
        refreshSongs();
        savePlaylists();
    }
    const bool list = activePlaylist_ >= 0 && activePlaylist_ < (int)playlists_.size();
    if (list) {
        ImGui::SameLine();
        if (ImGui::Button("Rename") && playlistName_[0]) {
            playlists_[(size_t)activePlaylist_].name = playlistName_;
            playlistName_[0] = 0;
            savePlaylists();
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) ImGui::OpenPopup("delete_list");
        if (ImGui::BeginPopup("delete_list")) {
            ImGui::Text("Delete \"%s\"? (the songs stay)", playlists_[(size_t)activePlaylist_].name.c_str());
            if (ImGui::Button("Delete")) {
                playlists_.erase(playlists_.begin() + activePlaylist_);
                activePlaylist_ = -1;
                refreshSongs();
                savePlaylists();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Songs...")) ImGui::OpenPopup("add_songs");
        if (ImGui::BeginPopup("add_songs")) {
            ImGui::BeginChild("addlist", ImVec2(420 * dpiScale_, 360 * dpiScale_));
            for (const auto& f : allSongs_) {
                const std::string n = pathToUtf8(f.filename());
                if (ImGui::Selectable(n.c_str(), false, ImGuiSelectableFlags_DontClosePopups)) {
                    playlists_[(size_t)activePlaylist_].songs.push_back(n);
                    refreshSongs();
                    savePlaylists();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Edit", &playlistEdit_);
    }
    ImGui::BeginChild("songs", ImVec2(0, 0), ImGuiChildFlags_Borders);
    // positions in songs_ map to playlist entries whose files exist
    auto entryIndex = [&](int pos) {
        int seen = -1;
        auto& s = playlists_[(size_t)activePlaylist_].songs;
        for (int k = 0; k < (int)s.size(); ++k) {
            bool exists = false;
            for (const auto& f : allSongs_) exists |= pathToUtf8(f.filename()) == s[(size_t)k];
            if (exists && ++seen == pos) return k;
        }
        return -1;
    };
    int action = 0, at = -1;
    for (int i = 0; i < (int)songs_.size(); ++i) {
        ImGui::PushID(i);
        if (list && playlistEdit_) {
            if (ImGui::SmallButton("^") && i > 0) { action = -1; at = i; }
            ImGui::SameLine();
            if (ImGui::SmallButton("v") && i + 1 < (int)songs_.size()) { action = 1; at = i; }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { action = 2; at = i; }
            ImGui::SameLine();
        }
        const std::string label = std::to_string(i + 1) + ". " + pathToUtf8(songs_[(size_t)i].stem());
        if (ImGui::Selectable(label.c_str(), i == songIndex_, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            playSong(i);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (list && at >= 0) {
        auto& s = playlists_[(size_t)activePlaylist_].songs;
        if (action == 2) {
            const int k = entryIndex(at);
            if (k >= 0) s.erase(s.begin() + k);
        } else {
            const int a = entryIndex(at), b = entryIndex(at + action);
            if (a >= 0 && b >= 0) std::swap(s[(size_t)a], s[(size_t)b]);
        }
        refreshSongs();
        savePlaylists();
    }
    ImGui::TextColored(kDim, "Double-click a song to play it. Songs live in %s.", pathToUtf8(songsDir()).c_str());
}

// ================================================================== live (set lists)

fs::path App::setListDir() const { return lib_.dataDir() / "setlists"; }

void App::loadSetLists() {
    setLists_.clear();
    activeSet_ = -1;
    setListsLoaded_ = true;
    Json j;
    std::string err;
    if (!Json::parse(readText(setListDir() / "setlists.json"), j, err) || !j.isObject()) return;
    if (const Json* lists = j.find("lists"); lists && lists->isArray())
        for (size_t i = 0; i < lists->size(); ++i) {
            const Json& l = lists->at(i);
            SetList s;
            if (const Json* n = l.find("name")) s.name = n->string();
            if (const Json* e = l.find("entries"); e && e->isArray())
                for (size_t k = 0; k < e->size(); ++k) {
                    const Json& x = e->at(k);
                    SetEntry en;
                    if (const Json* v = x.find("kind")) en.kind = v->string() == "sound" ? 1 : v->string() == "scene" ? 2 : 0;
                    if (const Json* v = x.find("name")) en.name = v->string();
                    if (const Json* v = x.find("file")) en.file = v->string();
                    s.entries.push_back(std::move(en));
                }
            setLists_.push_back(std::move(s));
        }
    const int a = j.find("active") ? (int)j.find("active")->number(0) : 0;
    activeSet_ = setLists_.empty() ? -1 : std::max(0, std::min((int)setLists_.size() - 1, a));
}

void App::saveSetLists() {
    Json j = Json::object(), lists = Json::array();
    static const char* kinds[3] = {"library", "sound", "scene"};
    for (const auto& s : setLists_) {
        Json l = Json::object(), e = Json::array();
        l.set("name", s.name);
        for (const auto& en : s.entries) {
            Json x = Json::object();
            x.set("kind", kinds[en.kind]);
            x.set("name", en.name);
            x.set("file", en.file);
            e.push(std::move(x));
        }
        l.set("entries", std::move(e));
        lists.push(std::move(l));
    }
    j.set("active", activeSet_);
    j.set("lists", std::move(lists));
    if (!writeText(setListDir() / "setlists.json", j.dump())) log("cannot save setlists.json", 2);
}

void App::addCurrentToSet() {
    if (activeSet_ < 0) return;
    const std::string root = pathToUtf8(lib_.libraryRoot());
    std::error_code ec;
    fs::create_directories(setListDir(), ec);
    std::string err;
    SetEntry e;
    if (engine_.multiTimbral()) {
        e.kind = 2;
        e.file = uniqueName() + ".scene.json";
        std::string first = "Multi";
        for (const auto& n : partNames_)
            if (!n.empty() && n != "Init") { first = n; break; }
        MultiPreset m = captureMulti(engine_, first, root);
        for (int k = 0; k < kNumParts; ++k) m.parts[k].name = partNames_[k];
        if (!saveMultiFile(pathToUtf8(setListDir() / e.file), m, err)) { log("cannot save the multi: " + err, 2); return; }
        e.name = "Multi: " + first;
    } else {
        e.kind = 1;
        e.file = uniqueName() + ".sound.json";
        Preset p = capturePreset(engine_, currentPresetName_, root);
        p.category = "Set List";
        p.author = "user";
        if (!savePresetFile(pathToUtf8(setListDir() / e.file), p, err)) { log("cannot save the sound: " + err, 2); return; }
        e.name = currentPresetName_;
    }
    setLists_[(size_t)activeSet_].entries.push_back(e);
    saveSetLists();
    log("added to set list " + setLists_[(size_t)activeSet_].name + ": " + e.name);
}

void App::recallSetEntry(int k) {
    if (activeSet_ < 0) return;
    const auto& list = setLists_[(size_t)activeSet_].entries;
    if (k < 0 || k >= (int)list.size()) return;
    const SetEntry e = list[(size_t)k];
    setCurrent_ = k;
    engine_.songPlay(false);
    const std::string root = pathToUtf8(lib_.libraryRoot());
    if (e.kind == 0) {
        if (engine_.multiTimbral()) setMultiMode(false);
        loadPreset(utf8ToPath(e.file));
    } else if (e.kind == 1) {
        if (engine_.multiTimbral()) setMultiMode(false);
        loadPreset(setListDir() / e.file);
    } else {
        const std::string file = pathToUtf8(setListDir() / e.file);
        log("set list: " + e.name + " ...");
        runAsync([this, file, root, e] {
            MultiPreset m;
            std::string err;
            std::vector<std::string> warnings;
            const bool ok = loadMultiFile(file, m, err) && applyMulti(engine_, m, root, warnings);
            post([this, ok, err, warnings, m, e] {
                if (!ok) { log(e.name + ": cannot load " + err, 2); return; }
                for (auto& w : warnings) log("  " + w, 1);
                for (int k = 0; k < kNumParts; ++k) partNames_[k] = m.parts[k].name;
                midi_.setMultiTimbral(true);
                currentPresetName_ = e.name;
                loadedSnap_ = captureState();
                log("set list: " + e.name);
            });
        });
    }
}

void App::drawLive() {
    const float d = dpiScale_;
    if (!setListsLoaded_) loadSetLists();
    const bool has = activeSet_ >= 0 && activeSet_ < (int)setLists_.size();
    ImGui::SetNextItemWidth(260 * d);
    if (ImGui::BeginCombo("##set", has ? setLists_[(size_t)activeSet_].name.c_str() : "(no set list)")) {
        for (int i = 0; i < (int)setLists_.size(); ++i)
            if (ImGui::Selectable((setLists_[(size_t)i].name + "##" + std::to_string(i)).c_str(), i == activeSet_)) {
                activeSet_ = i;
                setCurrent_ = -1;
                saveSetLists();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("<< Prev") && has) recallSetEntry(std::max(0, setCurrent_ - 1));
    ImGui::SameLine();
    if (ImGui::Button("Next >>") && has) {
        const int n = (int)setLists_[(size_t)activeSet_].entries.size();
        recallSetEntry(std::min(n - 1, setCurrent_ + 1));
    }
    ImGui::SameLine();
    ImGui::Checkbox("Edit", &setEdit_);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "   Page Up / Page Down also step through the list");
    if (!ImGui::GetIO().WantTextInput && has) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) recallSetEntry(std::min((int)setLists_[(size_t)activeSet_].entries.size() - 1, setCurrent_ + 1));
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) recallSetEntry(std::max(0, setCurrent_ - 1));
    }

    if (setEdit_ || !has) {
        ImGui::SetNextItemWidth(220 * d);
        ImGui::InputTextWithHint("##setname", "name", setName_, sizeof setName_);
        ImGui::SameLine();
        if (ImGui::Button("+ New List")) {
            std::string n = setName_;
            if (n.empty()) n = "Set " + std::to_string(setLists_.size() + 1);
            setLists_.push_back({n, {}});
            activeSet_ = (int)setLists_.size() - 1;
            setCurrent_ = -1;
            setName_[0] = 0;
            saveSetLists();
        }
        if (has) {
            ImGui::SameLine();
            if (ImGui::Button("Rename List") && setName_[0]) {
                setLists_[(size_t)activeSet_].name = setName_;
                setName_[0] = 0;
                saveSetLists();
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete List")) ImGui::OpenPopup("del_set");
            if (ImGui::BeginPopup("del_set")) {
                ImGui::Text("Delete the set list \"%s\" and its snapshots?", setLists_[(size_t)activeSet_].name.c_str());
                if (ImGui::Button("Delete")) {
                    std::error_code ec;
                    for (const auto& e : setLists_[(size_t)activeSet_].entries)
                        if (e.kind != 0) fs::remove(setListDir() / e.file, ec);
                    setLists_.erase(setLists_.begin() + activeSet_);
                    activeSet_ = setLists_.empty() ? -1 : 0;
                    setCurrent_ = -1;
                    saveSetLists();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (ImGui::Button("+ Current Sound")) addCurrentToSet();
            ImGui::SameLine();
            if (ImGui::Button("+ Library...")) ImGui::OpenPopup("set_lib");
            if (ImGui::BeginPopup("set_lib")) {
                ImGui::BeginChild("setlib", ImVec2(420 * d, 360 * d));
                for (const auto& p : presets_)
                    if (ImGui::Selectable(p.title.c_str(), false, ImGuiSelectableFlags_DontClosePopups)) {
                        setLists_[(size_t)activeSet_].entries.push_back({0, p.title, pathToUtf8(p.file)});
                        saveSetLists();
                    }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("+ Scene...")) ImGui::OpenPopup("set_scene");
            if (ImGui::BeginPopup("set_scene")) {
                for (int i = 0; i < kScenes; ++i) {
                    if (!scenes_[i].used) continue;
                    const std::string l = std::to_string(i + 1) + ": " + scenes_[i].name;
                    if (ImGui::Selectable(l.c_str())) {
                        SetEntry e{2, scenes_[i].name, uniqueName() + ".scene.json"};
                        std::error_code ec;
                        fs::create_directories(setListDir(), ec);
                        fs::copy_file(scenePath(i), setListDir() / e.file, fs::copy_options::overwrite_existing, ec);
                        if (ec) log("cannot copy scene " + std::to_string(i + 1), 2);
                        else { setLists_[(size_t)activeSet_].entries.push_back(e); saveSetLists(); }
                    }
                }
                ImGui::EndPopup();
            }
        }
    }
    if (!has) {
        ImGui::TextColored(kDim, "Make a set list, then add the sounds of your songs in running order.");
        return;
    }

    // the entries as big buttons
    auto& entries = setLists_[(size_t)activeSet_].entries;
    ImGui::BeginChild("setgrid", ImVec2(0, 0));
    const float bw = 230 * d, bh = 72 * d;
    const int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (bw + 8 * d)));
    int action = 0, at = -1;
    static const char* kindName[3] = {"Library", "Sound", "Multi"};
    for (int i = 0; i < (int)entries.size(); ++i) {
        ImGui::PushID(i);
        if (i % cols != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        const bool cur = i == setCurrent_;
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.58f, 1.0f));
        const std::string label = std::to_string(i + 1) + ". " + entries[(size_t)i].name + "\n" + kindName[entries[(size_t)i].kind];
        if (ImGui::Button(label.c_str(), ImVec2(bw, bh))) recallSetEntry(i);
        if (cur) ImGui::PopStyleColor();
        if (setEdit_) {
            if (ImGui::SmallButton("<")) { action = -1; at = i; }
            ImGui::SameLine();
            if (ImGui::SmallButton(">")) { action = 1; at = i; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Rename") && setName_[0]) { entries[(size_t)i].name = setName_; setName_[0] = 0; saveSetLists(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { action = 2; at = i; }
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (at >= 0) {
        if (action == 2) {
            std::error_code ec;
            if (entries[(size_t)at].kind != 0) fs::remove(setListDir() / entries[(size_t)at].file, ec);
            entries.erase(entries.begin() + at);
            if (setCurrent_ == at) setCurrent_ = -1; else if (setCurrent_ > at) --setCurrent_;
        } else if (at + action >= 0 && at + action < (int)entries.size()) {
            std::swap(entries[(size_t)at], entries[(size_t)(at + action)]);
            if (setCurrent_ == at) setCurrent_ = at + action; else if (setCurrent_ == at + action) setCurrent_ = at;
        }
        saveSetLists();
    }
}

// ================================================================== sampling

void App::drawSample() {
    const float d = dpiScale_;
    const bool rec = audio_.recording() && sampling_;
    ImGui::TextColored(kAccent, "Record");
    if (ImGui::RadioButton("Input", takeFromInput_)) takeFromInput_ = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Output", !takeFromInput_)) takeFromInput_ = false;
    ImGui::SameLine();
    if (ImGui::Button(rec ? "Stop" : "Rec")) {
        if (rec) {
            std::vector<float> inter;
            const size_t frames = audio_.stopRecording(inter);
            sampling_ = false;
            takeL_.resize(frames);
            takeR_.resize(frames);
            for (size_t i = 0; i < frames; ++i) { takeL_[i] = inter[i * 2]; takeR_[i] = inter[i * 2 + 1]; }
            takeRate_ = audioStats_.sampleRate > 0 ? audioStats_.sampleRate : 48000;
            takeStart_ = 0;
            takeEnd_ = (int64_t)frames;
            takeLoop_ = false;
            loopStart_ = 0;
            loopEnd_ = takeEnd_;
            sampleMsg_ = frames ? "" : "Nothing recorded";
        } else if (takeFromInput_ && !audioStats_.input) {
            sampleMsg_ = "Turn on the audio input on the Monitor tab first";
        } else if (audio_.recording()) {
            sampleMsg_ = "The output recording (Monitor) is running";
        } else {
            audio_.startRecording(30.0, takeFromInput_);
            sampling_ = true;
            sampleMsg_.clear();
        }
    }
    ImGui::SameLine();
    if (rec) ImGui::TextColored(kWarn, "REC %.1f s", audio_.recordedSeconds());
    else if (!takeL_.empty()) ImGui::Text("TAKE %.2f s", (double)takeL_.size() / takeRate_);
    ImGui::TextColored(kDim, "Input: the audio input (turn it on on the Monitor tab). Output: HSynth's own sound. Up to 30 s.");

    ImGui::Separator();
    ImGui::TextColored(kAccent, "Edit");
    const int64_t total = (int64_t)takeL_.size();
    // waveform with start (green), end (red) and loop (orange) markers; drag near a line to move it
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 140 * d;
    ImGui::InvisibleButton("wave", ImVec2(w, h));
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), kBg);
    if (total > 0) {
        const int points = std::max(1, (int)w);
        for (int p = 0; p < points; ++p) {
            const int64_t a0 = total * p / points, a1 = std::max(a0 + 1, total * (p + 1) / points);
            float pk = 0;
            for (int64_t i = a0; i < a1 && i < total; ++i) pk = std::max(pk, std::max(std::fabs(takeL_[(size_t)i]), std::fabs(takeR_[(size_t)i])));
            pk = std::min(1.0f, pk);
            const float x = p0.x + (float)p, mid = p0.y + h * 0.5f;
            dl->AddLine(ImVec2(x, mid - pk * h * 0.48f), ImVec2(x, mid + pk * h * 0.48f), kLine);
        }
        auto xOf = [&](int64_t f) { return p0.x + w * (float)((double)f / (double)total); };
        auto fOf = [&](float x) { return (int64_t)std::clamp((double)(x - p0.x) / w * (double)total, 0.0, (double)total); };
        if (takeLoop_) {
            dl->AddRectFilled(ImVec2(xOf(loopStart_), p0.y), ImVec2(xOf(loopEnd_), p0.y + h), IM_COL32(255, 160, 40, 40));
            dl->AddLine(ImVec2(xOf(loopStart_), p0.y), ImVec2(xOf(loopStart_), p0.y + h), IM_COL32(255, 160, 40, 255), 2);
            dl->AddLine(ImVec2(xOf(loopEnd_), p0.y), ImVec2(xOf(loopEnd_), p0.y + h), IM_COL32(255, 160, 40, 255), 2);
        }
        dl->AddLine(ImVec2(xOf(takeStart_), p0.y), ImVec2(xOf(takeStart_), p0.y + h), IM_COL32(80, 220, 100, 255), 2);
        dl->AddLine(ImVec2(xOf(takeEnd_), p0.y), ImVec2(xOf(takeEnd_), p0.y + h), IM_COL32(240, 80, 80, 255), 2);
        if (ImGui::IsItemActivated()) {
            const float mx = ImGui::GetIO().MousePos.x;
            int64_t* handles[4] = {&takeStart_, &takeEnd_, &loopStart_, &loopEnd_};
            float best = 1e9f;
            dragHandle_ = -1;
            for (int i = 0; i < (takeLoop_ ? 4 : 2); ++i) {
                const float dx = std::fabs(xOf(*handles[i]) - mx);
                if (dx < best) { best = dx; dragHandle_ = i; }
            }
        }
        if (ImGui::IsItemActive() && dragHandle_ >= 0) {
            const int64_t f = fOf(ImGui::GetIO().MousePos.x);
            switch (dragHandle_) {
                case 0: takeStart_ = std::min(f, takeEnd_ - 1); break;
                case 1: takeEnd_ = std::max(f, takeStart_ + 1); break;
                case 2: loopStart_ = std::clamp(f, takeStart_, loopEnd_ - 1); break;
                case 3: loopEnd_ = std::clamp(f, loopStart_ + 1, takeEnd_); break;
            }
        }
    }
    if (ImGui::Button("Auto Trim") && total > 0) {
        int64_t s = 0, e = 0;
        sampleBounds(takeL_, takeR_, -40.0f, s, e);
        takeStart_ = s;
        takeEnd_ = std::max(s + 1, e);
        loopStart_ = std::clamp(loopStart_, takeStart_, takeEnd_);
        loopEnd_ = std::clamp(loopEnd_, loopStart_, takeEnd_);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &takeLoop_);
    ImGui::SameLine();
    if (ImGui::Button("Find Loop") && total > 0) {
        int64_t ls = 0, le = 0;
        if (findLoop(takeL_, takeR_, takeStart_, takeEnd_, ls, le)) { loopStart_ = ls; loopEnd_ = le; takeLoop_ = true; sampleMsg_.clear(); }
        else sampleMsg_ = "No loop found (the sound may be too short or changing)";
    }
    ImGui::SameLine();
    ImGui::Checkbox("Normalize", &takeNormalize_);
    ImGui::SameLine();
    ImGui::Text("   ROOT");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##rootdn", ImGuiDir_Left)) takeRoot_ = std::max(12, takeRoot_ - 1);
    ImGui::SameLine();
    ImGui::Text("%s", noteName(takeRoot_).c_str());
    ImGui::SameLine();
    if (ImGui::ArrowButton("##rootup", ImGuiDir_Right)) takeRoot_ = std::min(108, takeRoot_ + 1);
    ImGui::TextColored(kDim, "Green = start, red = end, orange = loop. Drag near a line to move it. ROOT = the key that plays the take at its own pitch.");

    ImGui::Separator();
    ImGui::TextColored(kAccent, "Save");
    ImGui::SetNextItemWidth(260 * d);
    ImGui::InputText("##takename", takeName_, sizeof takeName_);
    for (int l = 0; l < 3; ++l) {
        ImGui::SameLine();
        const std::string b = std::string("Save -> Layer ") + (char)('A' + l);
        if (ImGui::Button(b.c_str())) {
            if (takeL_.empty()) { sampleMsg_ = "Nothing recorded"; continue; }
            std::string safe;
            for (char c : std::string(takeName_)) safe.push_back(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ' ? c : '_');
            if (safe.empty()) safe = "sample";
            const float gain = takeNormalize_ ? normalizeGain(takeL_, takeR_, takeStart_, takeEnd_, -1.0f) : 1.0f;
            std::string sfz, err;
            if (!saveSampledInstrument(pathToUtf8(lib_.libraryRoot() / "user" / "sampled"), safe, takeL_, takeR_, takeRate_,
                                       takeStart_, takeEnd_, gain, takeLoop_ && loopEnd_ > loopStart_ ? loopStart_ : -1,
                                       loopEnd_, takeRoot_, sfz, err)) {
                sampleMsg_ = "Cannot save: " + err;
            } else {
                loadSamples(l, utf8ToPath(sfz));
                sampleMsg_ = "Saved " + pathToUtf8(utf8ToPath(sfz).filename()) + " and loaded it into layer " + (char)('A' + l);
            }
        }
    }
    ImGui::TextColored(kDim, "Saved as WAV + SFZ in library/user/sampled and loaded into a layer of the edit part (Granular layers can use it too).");
    if (!sampleMsg_.empty()) ImGui::TextColored(kWarn, "%s", sampleMsg_.c_str());
}

// ================================================================== perform

void App::drawPerform() {
    const auto& table = paramTable();
    ImGui::TextColored(kAccent, "Perform");
    if (engine_.multiTimbral()) {
        ImGui::SameLine();
        ImGui::TextColored(kDim, "  (scale / chord: part %d)", editPart() + 1);
    }
    for (const char* n : {"perf.super", "scale.lock", "scale.root", "scale.type", "chord.mode", "chord.inversion"}) {
        const int id = findParam(n);
        if (id >= 0) drawParam(table[(size_t)id], id);
    }
    ImGui::TextColored(kDim, "Super Knob: one knob for every part (route the source 'super' in a mod matrix). "
                             "Scale lock moves wrong notes into the scale; chord mode plays a chord from one key.");

    ImGui::Separator();
    ImGui::TextColored(kAccent, "Phrase recorder");
    static const char* states[5] = {"EMPTY", "REC", "PLAY", "OVERDUB", "STOP"};
    const int st = (int)engine_.phraseState();
    if (ImGui::Button("Rec")) engine_.phraseRecord();
    ImGui::SameLine();
    if (ImGui::Button("Play")) engine_.phrasePlay();
    ImGui::SameLine();
    if (ImGui::Button("Stop##phrase")) engine_.phraseStop();
    ImGui::SameLine();
    if (ImGui::Button("Undo##phrase")) engine_.phraseUndo();
    ImGui::SameLine();
    if (ImGui::Button("Clear##phrase")) engine_.phraseClear();
    ImGui::SameLine();
    const double plen = engine_.phraseLength();
    ImGui::Text("  %s   %.1f / %.0f   %d events", st >= 0 && st < 5 ? states[st] : "?", engine_.phrasePosition(), plen,
                engine_.phraseEventCount());
    int bars = engine_.phraseBars();
    static const int barChoices[5] = {0, 1, 2, 4, 8};
    ImGui::SetNextItemWidth(110 * dpiScale_);
    if (ImGui::BeginCombo("Length", bars == 0 ? "Free" : (std::to_string(bars) + " bar" + (bars > 1 ? "s" : "")).c_str())) {
        for (int b : barChoices)
            if (ImGui::Selectable(b == 0 ? "Free" : (std::to_string(b) + " bar" + (b > 1 ? "s" : "")).c_str(), b == bars))
                engine_.setPhraseBars(b);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    auto snapshot = [&](std::vector<Engine::PhraseEvent>& ev, double& len) {
        engine_.requestPhraseSnapshot();
        for (int i = 0; i < 100; ++i) {
            if (engine_.takePhraseSnapshot(ev, len)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    ImGui::SetNextItemWidth(110 * dpiScale_);
    if (ImGui::BeginCombo("Quantize", "...")) {
        static const std::pair<const char*, double> grids[5] = {{"1/4", 1.0}, {"1/8", 0.5}, {"1/16", 0.25}, {"1/32", 0.125}, {"1/8T", 1.0 / 3.0}};
        for (const auto& g : grids)
            if (ImGui::Selectable(g.first)) {
                std::vector<Engine::PhraseEvent> ev;
                double len = 0;
                if (snapshot(ev, len) && len > 0) {
                    quantizePhrase(ev, len, g.second);
                    engine_.replacePhrase(ev, len);
                    phraseMsg_ = std::string("Quantized to ") + g.first;
                } else {
                    phraseMsg_ = "Quantize: record a phrase first (the audio must run)";
                }
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save .mid")) {
        std::vector<Engine::PhraseEvent> ev;
        double len = 0;
        const fs::path path = songsDir() / ("Phrase " + stamp() + ".mid");
        std::string err;
        if (!snapshot(ev, len)) phraseMsg_ = "The audio is not running";
        else if (!writePhraseSmf(pathToUtf8(path), ev, len, engine_.getParam(P::fxChain(P::FxTempo)), 4, err))
            phraseMsg_ = "Cannot save: " + err;
        else { phraseMsg_ = "Saved " + pathToUtf8(path.filename()) + " (4 times round) in the Player's songs"; refreshSongs(); }
    }
    ImGui::TextColored(kDim, "Records the keys / MIDI (notes, controllers, bend, pressure) as a loop in tempo; "
                             "Rec again while it plays to overdub. Length 'Free' rounds the first pass to bars.");
    if (!phraseMsg_.empty()) ImGui::TextColored(kWarn, "%s", phraseMsg_.c_str());
}

// ================================================================== edit graphs

// ADSR as a drawing; drag the corners: attack (x), decay (x) + sustain (y), release (x)
void App::drawEnvGraph(int attackId, int decayId, int sustainId, int releaseId) {
    const float d = dpiScale_;
    const auto& table = paramTable();
    ImGui::PushID(attackId);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::min(560 * d, ImGui::GetContentRegionAvail().x), h = 110 * d;
    ImGui::InvisibleButton("env", ImVec2(w, h));
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), kBg);
    // time axis: each stage gets a log-scaled width (0.5 ms .. 20 s -> 0 .. w/4)
    auto seg = [&](float t) { return w * 0.25f * (float)(std::log10(std::max(5e-4f, t) / 5e-4f) / std::log10(40000.0)); };
    auto tOf = [&](float px) { return 5e-4f * std::pow(40000.0f, std::clamp(px / (w * 0.25f), 0.0f, 1.0f)); };
    const float a = getP(attackId), dc = getP(decayId), s = getP(sustainId), r = getP(releaseId);
    const float pad = 6 * d, top = p0.y + pad, bot = p0.y + h - pad;
    const float xA = p0.x + pad + seg(a), xD = xA + seg(dc), xS = xD + w * 0.15f, xR = xS + seg(r);
    const float yS = bot - (bot - top) * s;
    const ImVec2 pts[5] = {{p0.x + pad, bot}, {xA, top}, {xD, yS}, {xS, yS}, {xR, bot}};
    dl->AddConvexPolyFilled(pts, 5, kFill);
    dl->AddPolyline(pts, 5, kLine, 0, 2.0f);
    for (int i = 1; i < 5; ++i) if (i != 3) dl->AddCircleFilled(pts[i], 4 * d, kLine);
    if (ImGui::IsItemActivated()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        float best = 1e9f;
        envDrag_ = -1;
        for (int i : {1, 2, 4}) {
            const float dd = std::hypot(pts[i].x - m.x, pts[i].y - m.y);
            if (dd < best) { best = dd; envDrag_ = i; }
        }
    }
    if (ImGui::IsItemActive() && envDrag_ >= 0) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        auto clampTo = [&](int id, float v) { const auto& in = table[(size_t)id]; setP(id, std::clamp(v, in.min, in.max)); };
        if (envDrag_ == 1) clampTo(attackId, tOf(m.x - (p0.x + pad)));
        if (envDrag_ == 2) { clampTo(decayId, tOf(m.x - xA)); clampTo(sustainId, (bot - m.y) / (bot - top)); }
        if (envDrag_ == 4) clampTo(releaseId, tOf(m.x - xS));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag the points: attack, decay + sustain, release");
    ImGui::PopID();
}

// Filter response sketch: drag left / right = cutoff, up / down = resonance
void App::drawFilterGraph() {
    const float d = dpiScale_;
    const auto& table = paramTable();
    const int cutId = P::FilterCutoff, resId = P::FilterReso;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::min(560 * d, ImGui::GetContentRegionAvail().x), h = 110 * d;
    ImGui::InvisibleButton("filtergraph", ImVec2(w, h));
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), kBg);
    for (float f : {100.0f, 1000.0f, 10000.0f}) {
        const float x = p0.x + w * std::log10(f / 20.0f) / 3.0f;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + h), kGrid);
    }
    const float fc = getP(cutId), q = getP(resId);
    const int type = (int)std::lround(getP(P::FilterType));
    const std::string tname = type >= 0 && type < (int)table[(size_t)P::FilterType].options.size()
                                  ? table[(size_t)P::FilterType].options[(size_t)type] : std::string();
    const bool hp = tname.find("HP") != std::string::npos || tname.find("hp") != std::string::npos;
    const bool bp = tname.find("BP") != std::string::npos || tname.find("bp") != std::string::npos;
    const float k = 1.0f / (0.5f + 4.0f * q * q + 0.01f);  // damping from resonance
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 120; ++i) {
        const float f = 20.0f * std::pow(1000.0f, (float)i / 120.0f);
        const float x = f / std::max(20.0f, fc);
        // 2-pole magnitude (|H| of an SVF output)
        const float den = std::sqrt((1 - x * x) * (1 - x * x) + (k * x) * (k * x));
        float mag = hp ? x * x / den : bp ? k * x / den : 1.0f / den;
        if (tname.find("24") != std::string::npos) mag *= mag;
        const float db = 20.0f * std::log10(std::max(1e-4f, mag));
        const float y = p0.y + h * 0.35f - db * h / 60.0f;
        pts.push_back(ImVec2(p0.x + w * (float)i / 120.0f, std::clamp(y, p0.y, p0.y + h)));
    }
    dl->AddPolyline(pts.data(), (int)pts.size(), kLine, 0, 2.0f);
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float f = 20.0f * std::pow(1000.0f, std::clamp((m.x - p0.x) / w, 0.0f, 1.0f));
        setP(cutId, std::clamp(f, table[(size_t)cutId].min, table[(size_t)cutId].max));
        setP(resId, std::clamp(1.0f - (m.y - p0.y) / h, 0.0f, 1.0f));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag: left / right = cutoff, up / down = resonance");
}

// FM algorithm: operators as boxes, carriers on the bottom row, modulators above
void App::drawFmAlgorithm(int layer) {
    const float d = dpiScale_;
    const int idx = (int)std::lround(getP(P::layer(layer, P::FmAlgo)));
    const FmAlgorithm& a = fmAlgorithm(std::clamp(idx, 0, kNumFmAlgorithms - 1));
    const int n = std::max(1, (int)a.numOps);
    // level of each op: carriers 0, a modulator one above the highest op it modulates
    int level[P::kFmOps] = {};
    for (int pass = 0; pass < P::kFmOps; ++pass)
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (i != j && (a.modulators[j] & (1u << i))) level[i] = std::max(level[i], std::min(P::kFmOps - 1, level[j] + 1));
    int maxLevel = 0;
    for (int i = 0; i < n; ++i) maxLevel = std::max(maxLevel, level[i]);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float bw = 34 * d, bh = 22 * d, gx = 14 * d, gy = 18 * d;
    const float w = std::min(560 * d, ImGui::GetContentRegionAvail().x), h = (maxLevel + 1) * (bh + gy) + gy;
    ImGui::Dummy(ImVec2(w, h));
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), kBg);
    ImVec2 pos[P::kFmOps];
    for (int lv = 0; lv <= maxLevel; ++lv) {
        int count = 0, k = 0;
        for (int i = 0; i < n; ++i) count += level[i] == lv;
        const float rowW = count * bw + (count - 1) * gx;
        for (int i = 0; i < n; ++i) {
            if (level[i] != lv) continue;
            pos[i] = ImVec2(p0.x + (w - rowW) * 0.5f + k * (bw + gx), p0.y + h - gy - (lv + 1) * bh - lv * gy);
            ++k;
        }
    }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            if (i != j && (a.modulators[j] & (1u << i)))
                dl->AddLine(ImVec2(pos[i].x + bw * 0.5f, pos[i].y + bh), ImVec2(pos[j].x + bw * 0.5f, pos[j].y), kGrid, 1.5f);
    for (int i = 0; i < n; ++i) {
        const bool carrier = a.carriers & (1u << i);
        dl->AddRectFilled(pos[i], ImVec2(pos[i].x + bw, pos[i].y + bh), carrier ? IM_COL32(120, 64, 14, 255) : IM_COL32(44, 47, 53, 255), 3);
        dl->AddRect(pos[i], ImVec2(pos[i].x + bw, pos[i].y + bh), kLine, 3);
        const std::string t = std::to_string(i + 1);
        dl->AddText(ImVec2(pos[i].x + bw * 0.5f - 4 * d, pos[i].y + 3 * d), IM_COL32(230, 230, 230, 255), t.c_str());
        if (a.feedbackOp == i) dl->AddCircle(ImVec2(pos[i].x + bw + 5 * d, pos[i].y + 4 * d), 4 * d, IM_COL32(255, 184, 77, 255));
    }
    ImGui::TextColored(kDim, "Algorithm %d: %d operators; orange = carriers (heard), lines = modulation, ring = feedback", idx + 1, n);
}

// ================================================================== host ops (CLI / MCP / AI)

bool App::runOnUi(std::function<void()> fn, int timeoutMs) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> f = done->get_future();
    post([fn = std::move(fn), done] {
        fn();
        done->set_value();
    });
    return f.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
}

// "host.*" requests of the remote protocol, on the RemoteControl worker thread.
//   host.tab        {"tab":"Player"}                     switch the UI tab
//   host.screenshot {"file"?}                            save the window as BMP, reply its path
//   host.log        {"lines"?:40}                        the app's message log
//   host.player     {"cmd":"list|play|toggle|pause|stop|next|prev|repeat", "index"?, "repeat"?:"off|all|one"}
//   host.live       {"cmd":"list|recall|next|prev|add_current", "index"?, "list"?}
bool App::hostOp(const std::string& op, const Json& req, Json& reply) {
    auto str = [&](const char* k, const std::string& def = "") {
        const Json* v = req.find(k);
        return v && v->isString() ? v->string() : def;
    };
    auto num = [&](const char* k, double def) {
        const Json* v = req.find(k);
        return v ? v->number(def) : def;
    };
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    if (op == "host.tab") {
        static const char* names[11] = {"Play", "Edit", "Library", "Monitor", "Browser", "Multi",
                                         "Seq", "Perform", "Live", "Player", "Sample"};
        const std::string want = lower(str("tab"));
        int idx = -1;
        for (int i = 0; i < 11; ++i)
            if (lower(names[i]) == want) idx = i;
        if (idx < 0) {
            reply.set("ok", false);
            reply.set("error", "tabs: Play Edit Browser Multi Seq Perform Live Player Sample Library Monitor");
            return true;
        }
        runOnUi([this, idx] { forceTab_ = idx; });
        reply.set("ok", true);
        reply.set("tab", names[idx]);
        return true;
    }
    if (op == "host.screenshot") {
        std::string file = str("file");
        std::error_code ec;
        if (file.empty()) {
            fs::create_directories(lib_.dataDir() / "screenshots", ec);
            file = pathToUtf8(lib_.dataDir() / "screenshots" / ("shot-" + stamp() + ".bmp"));
        }
        fs::remove(utf8ToPath(file), ec);
        runOnUi([this, file] { screenshotRequest_ = file; });
        bool ok = false;
        for (int i = 0; i < 60 && !ok; ++i) {  // the window writes it within a frame or two
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ok = fs::exists(utf8ToPath(file), ec) && fs::file_size(utf8ToPath(file), ec) > 54;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        reply.set("ok", ok);
        if (ok) reply.set("file", file);
        else reply.set("error", "no screenshot (is the window minimized?)");
        return true;
    }
    if (op == "host.log") {
        const int n = std::clamp((int)num("lines", 40), 1, 500);
        Json lines = Json::array();
        runOnUi([this, n, &lines] {
            const int start = std::max(0, (int)logLines_.size() - n);
            for (int i = start; i < (int)logLines_.size(); ++i) {
                const auto& l = logLines_[(size_t)i];
                lines.push(std::string(l.level == 2 ? "ERROR " : l.level == 1 ? "warn " : "") + l.text);
            }
        });
        reply.set("ok", true);
        reply.set("lines", std::move(lines));
        return true;
    }
    if (op == "host.player") {
        const std::string cmd = str("cmd", "list");
        const int index = (int)num("index", -1);
        const std::string rep = str("repeat");
        std::string error;
        Json info = Json::object();
        const bool ran = runOnUi([&] {
            if (!songsScanned_) { loadPlaylists(); refreshSongs(); }
            if (cmd == "list") {
                refreshSongs();
            } else if (cmd == "play") {
                if (index >= 0 && index < (int)songs_.size()) playSong(index);
                else if (engine_.songLength() > 0) engine_.songPlay(true);
                else playSong(std::max(0, songIndex_));
            } else if (cmd == "toggle") {
                if (engine_.songLength() <= 0) playSong(std::max(0, songIndex_));
                else engine_.songPlay(!engine_.songPlaying());
            } else if (cmd == "pause") {
                engine_.songPlay(false);
            } else if (cmd == "stop") {
                engine_.songPlay(false);
                engine_.songSeek(0);
            } else if (cmd == "next") {
                playerNext(true);
            } else if (cmd == "prev") {
                if (!songs_.empty()) playSong(songIndex_ <= 0 ? (int)songs_.size() - 1 : songIndex_ - 1);
            } else if (cmd == "repeat") {
                repeatMode_ = rep == "off" ? 0 : rep == "one" ? 2 : 1;
            } else {
                error = "cmd: list play toggle pause stop next prev repeat";
            }
            Json songs = Json::array();
            for (const auto& s : songs_) songs.push(pathToUtf8(s.filename()));
            info.set("songs", std::move(songs));
            info.set("index", songIndex_);
            info.set("playlist", activePlaylist_ >= 0 && activePlaylist_ < (int)playlists_.size()
                                     ? playlists_[(size_t)activePlaylist_].name
                                     : std::string("All songs"));
            static const char* reps[3] = {"off", "all", "one"};
            info.set("repeat", reps[repeatMode_]);
            info.set("loading", songLoading_.load());
            info.set("message", playerMsg_);
        });
        if (!ran) error = "the window is busy";
        info.set("ok", error.empty());
        if (!error.empty()) info.set("error", error);
        info.set("playing", engine_.songPlaying());
        info.set("position", engine_.songPosition());
        info.set("length", engine_.songLength());
        reply = std::move(info);
        return true;
    }
    if (op == "host.live") {
        const std::string cmd = str("cmd", "list");
        const int index = (int)num("index", -1);
        const std::string list = str("list");
        std::string error;
        Json info = Json::object();
        runOnUi([&] {
            if (!setListsLoaded_) loadSetLists();
            if (!list.empty())
                for (int i = 0; i < (int)setLists_.size(); ++i)
                    if (setLists_[(size_t)i].name == list) { activeSet_ = i; setCurrent_ = -1; }
            const bool has = activeSet_ >= 0 && activeSet_ < (int)setLists_.size();
            const int n = has ? (int)setLists_[(size_t)activeSet_].entries.size() : 0;
            if (cmd == "recall") {
                if (index >= 0 && index < n) recallSetEntry(index);
                else error = "index out of range";
            } else if (cmd == "next") {
                if (n) recallSetEntry(std::min(n - 1, setCurrent_ + 1));
            } else if (cmd == "prev") {
                if (n) recallSetEntry(std::max(0, setCurrent_ - 1));
            } else if (cmd == "add_current") {
                if (has) addCurrentToSet();
                else error = "no set list";
            } else if (cmd != "list") {
                error = "cmd: list recall next prev add_current";
            }
            Json lists = Json::array();
            for (const auto& s : setLists_) lists.push(s.name);
            info.set("lists", std::move(lists));
            info.set("list", has ? setLists_[(size_t)activeSet_].name : std::string());
            Json entries = Json::array();
            static const char* kinds[3] = {"library", "sound", "multi"};
            if (has)
                for (const auto& e : setLists_[(size_t)activeSet_].entries) {
                    Json x = Json::object();
                    x.set("name", e.name);
                    x.set("kind", kinds[e.kind]);
                    entries.push(std::move(x));
                }
            info.set("entries", std::move(entries));
            info.set("current", setCurrent_);
        });
        info.set("ok", error.empty());
        if (!error.empty()) info.set("error", error);
        reply = std::move(info);
        return true;
    }
    return false;
}
