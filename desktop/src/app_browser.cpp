// Desktop UI: preset browser, multi-timbral editor (parts / zones), scenes and
// MIDI learn. Split from app.cpp; same App class.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.h"
#include "hw_ui.h"
#include "hsynth/host_utils.h"
#include "hsynth/preset.h"
#include "hsynth/streaming.h"
#include "imgui.h"
#include "platform.h"

using namespace hsynth;

namespace {

const ImVec4 kAccent(1.0f, 0.62f, 0.22f, 1.0f);  // LED orange
const ImVec4 kDim(0.63f, 0.65f, 0.70f, 1.0f);  // engraved
const ImVec4 kWarn(1.0f, 0.72f, 0.30f, 1.0f);
const ImVec4 kStar(1.0f, 0.82f, 0.25f, 1.0f);

// distinct colours for the parts (zone view, part column)
ImU32 partColor(int k, float alpha = 1.0f) {
    const float h = (float)k / (float)kNumParts;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, 0.55f, 0.95f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

std::string noteName(int n) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

}  // namespace

// ------------------------------------------------------------------ settings

void App::applyStreamingSettings() {
    SampleLoadOptions o = sampleLoadOptions();
    o.mode = (StreamMode)std::max(0, std::min(2, s_.streamMode));
    o.cacheDir = pathToUtf8(lib_.dataDir() / "cache" / "stream");
    setSampleLoadOptions(o);
}

void App::saveMidiMap() {
    std::string err;
    if (!learn_.save(pathToUtf8(lib_.dataDir() / "midi_map.json"), err)) log("MIDI map: " + err, 1);
}

// ------------------------------------------------------------------ MIDI learn

bool App::paramContextMenu(const ParamInfo& info, int id, float& v, bool inlineInfo) {
    bool changed = false;
    const std::string mapped = learn_.describe(id);
    const bool armed = learn_.armedParam() == id;
    if (!inlineInfo) {
        if (armed) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), hw::kLed, 3, 0, 2);
    } else if (armed) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "move a knob / slider on your MIDI controller...");
    } else if (!mapped.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kDim, "[%s]", mapped.c_str());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s%s%s\ndrag up / down (Shift: fine), double-click: default\nright-click: reset / MIDI learn, ctrl+click: type a value", info.name.c_str(),
                          mapped.empty() ? "" : "\nMIDI: ", mapped.c_str());
    if (ImGui::BeginPopupContextItem("param_ctx")) {
        ImGui::TextColored(kAccent, "%s", info.label.c_str());
        if (ImGui::MenuItem("Reset to default")) { v = info.def; changed = true; }
        if (ImGui::MenuItem(armed ? "Cancel MIDI learn" : "MIDI learn (move a controller)")) {
            if (armed) learn_.cancel();
            else learn_.arm(id);
        }
        if (!mapped.empty() && ImGui::MenuItem(("Forget MIDI (" + mapped + ")").c_str())) {
            learn_.removeParam(id);
            saveMidiMap();
        }
        ImGui::EndPopup();
    }
    return changed;
}

void App::drawMidiMap() {
    // a learn completed on the MIDI thread: save and report once
    const int learned = learn_.lastLearned();
    if (learned >= 0 && learned != learnSeen_) {
        learnSeen_ = learned;
        saveMidiMap();
        log("MIDI learn: " + paramTable()[(size_t)learned].label + " <- " + learn_.describe(learned));
    }
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "MIDI learn");
    const auto maps = learn_.mappings();
    if (maps.empty()) {
        ImGui::TextColored(kDim, "no assignments - right-click any parameter in Edit and choose 'MIDI learn'");
        return;
    }
    if (ImGui::BeginTable("midimap", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("CC");
        ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Range");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int remove = -1;
        for (const auto& m : maps) {
            const ParamInfo& info = paramTable()[(size_t)m.param];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d%s", m.cc, m.channel >= 0 ? (" ch" + std::to_string(m.channel + 1)).c_str() : "");
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", info.label.c_str(), m.part >= 0 ? (" (part " + std::to_string(m.part + 1) + ")").c_str() : "");
            ImGui::TableNextColumn();
            ImGui::Text("%.3g .. %.3g", m.min, m.max);
            ImGui::TableNextColumn();
            ImGui::PushID(m.param * 128 + m.cc);
            if (ImGui::SmallButton("x")) remove = m.param;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (remove >= 0) {
            learn_.removeParam(remove);
            saveMidiMap();
        }
    }
    if (ImGui::SmallButton("Clear all")) {
        learn_.setMappings({});
        saveMidiMap();
    }
}

// ------------------------------------------------------------------ browser

void App::rescanBrowser() {
    PresetIndex idx;
    idx.setFolders({{pathToUtf8(lib_.dataDir() / "presets" / "factory"), false},
                    {pathToUtf8(lib_.userPresetDir()), true}});
    const std::string fav = pathToUtf8(lib_.dataDir() / "favorites.json");
    runAsync([this, idx, fav]() mutable {
        idx.setFavoritesFile(fav);
        idx.rescan();
        post([this, idx = std::move(idx)]() mutable {
            index_ = std::move(idx);
            browserDirty_ = true;
        });
    });
}

void App::updateBrowserResults() {
    PresetQuery q;
    q.text = browserSearch_;
    q.category = browserCategory_;
    q.favoritesOnly = browserFavorites_;
    q.userOnly = browserUser_;
    browserResults_ = index_.query(q);
    browserDirty_ = false;
}

void App::loadPresetInto(const fs::path& file, int part) {
    log("loading " + pathToUtf8(file.filename()) + " into part " + std::to_string(part + 1) + " ...");
    const std::string root = pathToUtf8(lib_.libraryRoot());
    runAsync([this, file, part, root] {
        Preset p;
        std::string err;
        std::vector<std::string> warnings;
        const bool ok = loadPresetFile(pathToUtf8(file), p, err) && applyPartPreset(engine_, part, p, root, warnings);
        post([this, ok, err, warnings, p, part, file] {
            if (!ok) { log("preset load failed: " + err, 2); return; }
            for (auto& w : warnings) log("  " + w, 1);
            partNames_[part] = p.name;
            if (part == engine_.selectedPart()) {
                currentPreset_ = file;
                currentPresetName_ = p.name;
                presetComment_ = p.comment;
                loadedSnap_ = captureState();
                comparing_ = false;
            }
            log("part " + std::to_string(part + 1) + ": \"" + p.name + "\"");
        });
    });
}

void App::drawBrowser() {
    const float d = dpiScale_;
    if (browserDirty_) updateBrowserResults();
    const auto& all = index_.all();

    // ---- left: search, filters, categories, tags
    ImGui::BeginChild("bfilters", ImVec2(240 * d, 0));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##search", "search  (#tag for tags)", browserSearch_, sizeof browserSearch_))
        browserDirty_ = true;
    auto filterItem = [&](const char* label, bool active, int count) {
        const std::string l = std::string(label) + "  (" + std::to_string(count) + ")";
        return ImGui::Selectable(l.c_str(), active);
    };
    int favCount = 0, userCount = 0;
    for (const auto& p : all) { favCount += p.favorite; userCount += p.user; }
    if (filterItem("All presets", browserCategory_.empty() && !browserFavorites_ && !browserUser_, (int)all.size())) {
        browserCategory_.clear();
        browserFavorites_ = browserUser_ = false;
        browserDirty_ = true;
    }
    if (filterItem("Favorites", browserFavorites_, favCount)) { browserFavorites_ = !browserFavorites_; browserDirty_ = true; }
    if (filterItem("User presets", browserUser_, userCount)) { browserUser_ = !browserUser_; browserDirty_ = true; }
    ImGui::Separator();
    ImGui::TextColored(kAccent, "Categories");
    ImGui::BeginChild("bcats", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f));
    for (const auto& [cat, n] : index_.categories()) {
        const std::string label = (cat.empty() ? std::string("(none)") : cat) + "  (" + std::to_string(n) + ")##c" + cat;
        if (ImGui::Selectable(label.c_str(), browserCategory_ == cat && !cat.empty())) {
            browserCategory_ = browserCategory_ == cat ? std::string() : cat;
            browserDirty_ = true;
        }
    }
    ImGui::EndChild();
    ImGui::TextColored(kAccent, "Tags");
    ImGui::BeginChild("btags", ImVec2(0, 0));
    {
        const float maxX = ImGui::GetContentRegionAvail().x;
        float x = 0;
        int shown = 0;
        for (const auto& [tag, n] : index_.tags()) {
            if (++shown > 40) break;
            const std::string label = "#" + tag;
            const float w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
            if (x > 0 && x + w < maxX) ImGui::SameLine();
            else x = 0;
            x += w + ImGui::GetStyle().ItemSpacing.x;
            if (ImGui::SmallButton(label.c_str())) {
                std::string s = browserSearch_;
                if (s.find(label) == std::string::npos) s += (s.empty() ? "" : " ") + label;
                std::snprintf(browserSearch_, sizeof browserSearch_, "%s", s.c_str());
                browserDirty_ = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d presets", n);
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();

    // ---- right: results and details
    ImGui::SameLine();
    ImGui::BeginChild("bright", ImVec2(0, 0));
    const bool multi = engine_.multiTimbral();
    ImGui::TextColored(kDim, "%d presets   click: load%s   up/down: previous / next   star: favorite", (int)browserResults_.size(),
                       multi ? (" into part " + std::to_string(engine_.selectedPart() + 1)).c_str() : "");
    const float detailH = 150 * d;
    ImGui::BeginChild("bresults", ImVec2(0, ImGui::GetContentRegionAvail().y - detailH), ImGuiChildFlags_Borders);
    // keyboard: step through the results (loading each, like a hardware synth)
    int step = 0;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) step = -1;
    }
    int load = -1;
    if (step != 0 && !browserResults_.empty()) {
        int pos = -1;
        for (int i = 0; i < (int)browserResults_.size(); ++i)
            if (browserResults_[(size_t)i] == browserSel_) pos = i;
        pos = std::max(0, std::min((int)browserResults_.size() - 1, pos + step));
        load = browserResults_[(size_t)pos];
    }
    if (ImGui::BeginTable("bt", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24 * d);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)browserResults_.size());
        int favToggle = -1;
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const int i = browserResults_[(size_t)r];
                const PresetInfo& p = all[(size_t)i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, p.favorite ? kStar : kDim);
                if (ImGui::SmallButton(p.favorite ? "*" : ".")) favToggle = i;
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                const bool current = utf8ToPath(p.file) == currentPreset_;
                if (ImGui::Selectable(p.name.c_str(), i == browserSel_ || current, ImGuiSelectableFlags_SpanAllColumns))
                    load = i;
                if (step != 0 && load == i) ImGui::SetScrollHereY();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.category.c_str());
                ImGui::TableNextColumn();
                std::string t = PresetIndex::joinTags(p.tags);
                const std::string a = PresetIndex::joinTags(p.autoTags);
                ImGui::TextUnformatted(t.c_str());
                ImGui::SameLine();
                ImGui::TextColored(kDim, "%s", a.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
        if (favToggle >= 0) {
            index_.setFavorite(favToggle, !all[(size_t)favToggle].favorite);
            if (browserFavorites_) browserDirty_ = true;
        }
    }
    ImGui::EndChild();
    if (load >= 0) {
        browserSel_ = load;
        const PresetInfo& p = all[(size_t)load];
        std::snprintf(tagBuf_, sizeof tagBuf_, "%s", PresetIndex::joinTags(p.tags).c_str());
        std::snprintf(catBuf_, sizeof catBuf_, "%s", p.category.c_str());
        loadPreset(utf8ToPath(p.file));
    }

    // details / tag editor
    if (browserSel_ >= 0 && browserSel_ < (int)all.size()) {
        const PresetInfo& p = all[(size_t)browserSel_];
        ImGui::TextColored(kAccent, "%s", p.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kDim, "  %s%s", p.user ? "user  " : "factory  ", p.author.c_str());
        if (!p.comment.empty()) ImGui::TextWrapped("%s", p.comment.c_str());
        ImGui::SetNextItemWidth(160 * d);
        ImGui::InputText("Category", catBuf_, sizeof catBuf_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(320 * d);
        ImGui::InputTextWithHint("Tags", "comma separated", tagBuf_, sizeof tagBuf_);
        ImGui::SameLine();
        if (p.user) {
            if (ImGui::Button("Save tags")) {
                std::string err;
                if (index_.updateMetadata(browserSel_, catBuf_, PresetIndex::splitTags(tagBuf_), err)) {
                    log("tags saved: " + p.name);
                    browserDirty_ = true;
                } else {
                    log(err, 2);
                }
            }
        } else if (ImGui::Button("Save as user preset with these tags")) {
            Preset q;
            std::string err;
            if (loadPresetFile(p.file, q, err)) {
                q.category = catBuf_;
                q.tags = PresetIndex::splitTags(tagBuf_);
                const fs::path f = lib_.userPresetFile(q.name);
                if (savePresetFile(pathToUtf8(f), q, err)) {
                    log("saved user copy: " + pathToUtf8(f.filename()));
                    refreshLists();
                }
            }
            if (!err.empty()) log(err, 2);
        }
    }
    ImGui::EndChild();
}

// Compact preset list for a part (used in the multi editor popup).
bool App::presetPicker(const char* id, int part) {
    bool picked = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(300 * dpiScale_);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##pick", "search", pickerSearch_, sizeof pickerSearch_);
    PresetQuery q;
    q.text = pickerSearch_;
    const auto res = index_.query(q);
    ImGui::BeginChild("picklist", ImVec2(420 * dpiScale_, 320 * dpiScale_), ImGuiChildFlags_Borders);
    std::string lastCat = "\x01";
    for (int i : res) {
        const PresetInfo& p = index_.all()[(size_t)i];
        if (p.category != lastCat) {
            lastCat = p.category;
            ImGui::TextColored(kDim, "%s", p.category.empty() ? "(none)" : p.category.c_str());
        }
        if (ImGui::Selectable((p.name + "##" + p.file).c_str())) {
            loadPresetInto(utf8ToPath(p.file), part);
            picked = true;
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
    return picked;
}

// ------------------------------------------------------------------ multi

void App::setMultiMode(bool on) {
    engine_.setMultiTimbral(on);
    midi_.setMultiTimbral(on);
    if (!on) engine_.selectPart(0);
}

void App::drawZones(float width, float height) {
    // keyboard ruler with one bar per part that can sound: key range, transpose, mute
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float d = dpiScale_;
    const float keyW = width / 128.0f;
    const float rulerH = 14 * d;
    dl->AddRectFilled(o, ImVec2(o.x + width, o.y + rulerH), ImGui::GetColorU32(ImGuiCol_FrameBg));
    for (int n = 0; n < 128; ++n) {
        if (n % 12 == 0) {
            dl->AddLine(ImVec2(o.x + n * keyW, o.y), ImVec2(o.x + n * keyW, o.y + height), ImGui::GetColorU32(ImGuiCol_Separator));
            dl->AddText(ImVec2(o.x + n * keyW + 2, o.y), ImGui::GetColorU32(kDim), noteName(n).c_str());
        }
    }
    std::vector<int> shown;
    for (int k = 0; k < kNumParts; ++k)
        if (k == 0 || partNames_[k] != "Init" || engine_.partActiveVoices(k) > 0) shown.push_back(k);
    const float rowH = std::max(10.0f * d, (height - rulerH - 4 * d) / std::max<float>(1.0f, (float)shown.size()));
    for (size_t r = 0; r < shown.size(); ++r) {
        const int k = shown[r];
        const int lo = (int)engine_.getPartMix(k, Mix::KeyLow), hi = (int)engine_.getPartMix(k, Mix::KeyHigh);
        const bool muted = engine_.getPartMix(k, Mix::Mute) > 0.5f;
        const float y = o.y + rulerH + 2 * d + (float)r * rowH;
        const ImVec2 a(o.x + lo * keyW, y + 1), b(o.x + (hi + 1) * keyW, y + rowH - 1);
        dl->AddRectFilled(a, b, partColor(k, muted ? 0.25f : (k == engine_.selectedPart() ? 0.95f : 0.6f)), 3 * d);
        const int ch = (int)engine_.getPartMix(k, Mix::Channel);
        const int tr = (int)engine_.getPartMix(k, Mix::Transpose);
        char label[160];
        std::snprintf(label, sizeof label, "%d %s  ch%d%s%s", k + 1, partNames_[k].c_str(), ch,
                      tr ? (tr > 0 ? "  +" : "  ") : "", tr ? std::to_string(tr).c_str() : "");
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + 4 * d, a.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(20, 20, 24, 255), label);
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(width, height));
}

void App::drawMulti() {
    const float d = dpiScale_;
    bool multi = engine_.multiTimbral();
    if (ImGui::Checkbox("Multi-timbral (16 parts, one per MIDI channel)", &multi)) setMultiMode(multi);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "   PC keyboard / mouse play the selected part; MIDI channels route by part channel");
    drawSceneBar();
    if (ImGui::Button("Load multi...")) {
        const auto files = platform::openFiles({{"HSynth multi (*.json)", "*.json"}}, false);
        if (!files.empty()) {
            const std::string root = pathToUtf8(lib_.libraryRoot());
            const std::string file = files.front();
            runAsync([this, file, root] {
                MultiPreset m;
                std::string err;
                std::vector<std::string> warnings;
                const bool ok = loadMultiFile(file, m, err) && applyMulti(engine_, m, root, warnings);
                post([this, ok, err, warnings, m] {
                    if (!ok) { log("multi load failed: " + err, 2); return; }
                    for (auto& w : warnings) log("  " + w, 1);
                    for (int k = 0; k < kNumParts; ++k) partNames_[k] = m.parts[k].name;
                    midi_.setMultiTimbral(true);
                    log("multi \"" + m.name + "\" loaded");
                });
            });
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save multi...")) {
        const std::string path = platform::saveFile({{"HSynth multi (*.json)", "*.json"}}, "multi.json", "json");
        if (!path.empty()) {
            MultiPreset m = captureMulti(engine_, utf8ToPath(path).stem().u8string().empty() ? "Multi" : pathToUtf8(utf8ToPath(path).stem()),
                                         pathToUtf8(lib_.libraryRoot()));
            for (int k = 0; k < kNumParts; ++k) m.parts[k].name = partNames_[k];
            std::string err;
            if (saveMultiFile(path, m, err)) log("multi saved: " + path);
            else log(err, 2);
        }
    }
    if (!multi) {
        ImGui::TextColored(kWarn, "Single-part mode: only part 1 plays. Turn on multi-timbral mode to layer / split parts.");
    }

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Zones");
    drawZones(ImGui::GetContentRegionAvail().x, 120 * d);

    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Parts");
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("parts", 12, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Edit");
        ImGui::TableSetupColumn("Preset", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Mute");
        ImGui::TableSetupColumn("Ch");
        ImGui::TableSetupColumn("Low");
        ImGui::TableSetupColumn("High");
        ImGui::TableSetupColumn("Trans");
        ImGui::TableSetupColumn("Volume");
        ImGui::TableSetupColumn("Pan");
        ImGui::TableSetupColumn("FX send");
        ImGui::TableSetupColumn("Voices");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        const float w = 70 * d;
        for (int k = 0; k < kNumParts; ++k) {
            ImGui::TableNextRow();
            ImGui::PushID(k);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_CheckMark, partColor(k));
            if (ImGui::RadioButton(std::to_string(k + 1).c_str(), engine_.selectedPart() == k)) {
                engine_.selectPart(k);
                currentPresetName_ = partNames_[k];
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("select: Edit / Browser / PC keyboard work on this part");
            ImGui::TableNextColumn();
            if (ImGui::Button((partNames_[k] + "##p").c_str(), ImVec2(-1, 0))) {
                pickerSearch_[0] = 0;
                ImGui::OpenPopup("pick");
            }
            if (ImGui::BeginPopup("pick")) {
                ImGui::TextColored(kAccent, "Preset for part %d", k + 1);
                if (presetPicker("picker", k)) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            auto mixInt = [&](int id, int lo, int hi, const char* fmt) {
                int v = (int)std::lround(engine_.getPartMix(k, id));
                ImGui::SetNextItemWidth(w);
                if (ImGui::DragInt(("##m" + std::to_string(id)).c_str(), &v, 0.2f, lo, hi, fmt)) engine_.setPartMix(k, id, (float)v);
            };
            auto mixFloat = [&](int id, float lo, float hi, const char* fmt) {
                float v = engine_.getPartMix(k, id);
                ImGui::SetNextItemWidth(w);
                if (ImGui::SliderFloat(("##m" + std::to_string(id)).c_str(), &v, lo, hi, fmt)) engine_.setPartMix(k, id, v);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    float def[Mix::Count];
                    defaultPartMix(k, def);
                    engine_.setPartMix(k, id, def[id]);
                }
            };
            ImGui::TableNextColumn();
            bool mute = engine_.getPartMix(k, Mix::Mute) > 0.5f;
            if (ImGui::Checkbox("##mute", &mute)) engine_.setPartMix(k, Mix::Mute, mute ? 1.0f : 0.0f);
            ImGui::TableNextColumn();
            mixInt(Mix::Channel, 1, 16, "%d");
            ImGui::TableNextColumn();
            {
                int lo = (int)engine_.getPartMix(k, Mix::KeyLow);
                ImGui::SetNextItemWidth(w);
                if (ImGui::DragInt("##lo", &lo, 0.3f, 0, 127, noteName(lo).c_str())) engine_.setPartMix(k, Mix::KeyLow, (float)lo);
            }
            ImGui::TableNextColumn();
            {
                int hi = (int)engine_.getPartMix(k, Mix::KeyHigh);
                ImGui::SetNextItemWidth(w);
                if (ImGui::DragInt("##hi", &hi, 0.3f, 0, 127, noteName(hi).c_str())) engine_.setPartMix(k, Mix::KeyHigh, (float)hi);
            }
            ImGui::TableNextColumn();
            mixInt(Mix::Transpose, -48, 48, "%+d");
            ImGui::TableNextColumn();
            mixFloat(Mix::Volume, 0.0f, 1.0f, "%.2f");
            ImGui::TableNextColumn();
            mixFloat(Mix::Pan, -1.0f, 1.0f, "%.2f");
            ImGui::TableNextColumn();
            mixFloat(Mix::FxSend, 0.0f, 1.0f, "%.2f");
            ImGui::TableNextColumn();
            const int v = engine_.partActiveVoices(k);
            ImGui::TextColored(v ? kAccent : kDim, "%3d", v);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Init")) {
                Preset init;
                std::vector<std::string> warnings;
                applyPartPreset(engine_, k, init, pathToUtf8(lib_.libraryRoot()), warnings);
                partNames_[k] = "Init";
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ scenes

fs::path App::scenePath(int slot) const {
    char name[32];
    std::snprintf(name, sizeof name, "scene_%02d.json", slot + 1);
    return lib_.dataDir() / "scenes" / name;
}

void App::refreshScenes() {
    for (int i = 0; i < kScenes; ++i) {
        scenes_[i] = SceneSlot{};
        const fs::path p = scenePath(i);
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        MultiPreset m;
        std::string err;
        if (!loadMultiFile(pathToUtf8(p), m, err)) continue;
        scenes_[i].used = true;
        scenes_[i].name = m.name;
    }
}

void App::recallScene(int slot) {
    if (slot < 0 || slot >= kScenes || !scenes_[slot].used) return;
    const std::string root = pathToUtf8(lib_.libraryRoot());
    const std::string file = pathToUtf8(scenePath(slot));
    currentScene_ = slot;
    log("scene " + std::to_string(slot + 1) + " \"" + scenes_[slot].name + "\" ...");
    runAsync([this, file, root, slot] {
        MultiPreset m;
        std::string err;
        std::vector<std::string> warnings;
        const bool ok = loadMultiFile(file, m, err) && applyMulti(engine_, m, root, warnings);
        post([this, ok, err, warnings, m, slot] {
            if (!ok) { log("scene recall failed: " + err, 2); return; }
            for (auto& w : warnings) log("  " + w, 1);
            for (int k = 0; k < kNumParts; ++k) partNames_[k] = m.parts[k].name;
            midi_.setMultiTimbral(true);
            currentPresetName_ = partNames_[engine_.selectedPart()];
            log("scene " + std::to_string(slot + 1) + " recalled");
        });
    });
}

void App::storeScene(int slot, const std::string& name) {
    MultiPreset m = captureMulti(engine_, name, pathToUtf8(lib_.libraryRoot()));
    for (int k = 0; k < kNumParts; ++k) m.parts[k].name = partNames_[k];
    std::error_code ec;
    fs::create_directories(scenePath(slot).parent_path(), ec);
    std::string err;
    if (!saveMultiFile(pathToUtf8(scenePath(slot)), m, err)) {
        log("scene store failed: " + err, 2);
        return;
    }
    scenes_[slot].used = true;
    scenes_[slot].name = name;
    currentScene_ = slot;
    log("scene " + std::to_string(slot + 1) + " stored: " + name);
}

void App::drawSceneBar() {
    const float d = dpiScale_;
    ImGui::TextColored(kAccent, "Scenes");
    ImGui::SameLine();
    for (int i = 0; i < kScenes; ++i) {
        ImGui::PushID(i);
        const SceneSlot& s = scenes_[i];
        const bool cur = i == currentScene_;
        if (i) ImGui::SameLine(0, 4 * d);
        if (hw::Button(std::to_string(i + 1).c_str(), s.used ? &cur : nullptr, 44 * d, true)) {
            if (s.used) recallScene(i);
            else {
                sceneEdit_ = i;
                std::snprintf(sceneName_, sizeof sceneName_, "Scene %d", i + 1);
                ImGui::OpenPopup("store");
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nclick: %s   right-click: store / clear", s.used ? s.name.c_str() : "(empty)",
                              s.used ? "recall" : "store the current setup");
        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem("Store current setup here...")) {
                sceneEdit_ = i;
                std::snprintf(sceneName_, sizeof sceneName_, "%s", s.used ? s.name.c_str() : ("Scene " + std::to_string(i + 1)).c_str());
                ImGui::CloseCurrentPopup();
                ImGui::OpenPopup("store");
            }
            if (s.used && ImGui::MenuItem("Clear")) {
                std::error_code ec;
                fs::remove(scenePath(i), ec);
                scenes_[i] = SceneSlot{};
                if (currentScene_ == i) currentScene_ = -1;
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("store")) {
            ImGui::Text("Store scene %d (all 16 parts, zones, mix, effects)", sceneEdit_ + 1);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("Name", sceneName_, sizeof sceneName_, ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Store") || enter) {
                storeScene(sceneEdit_, sceneName_[0] ? sceneName_ : "Scene");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();
}
