// Desktop UI: step sequencer / motion lanes, macro knobs, transport, MIDI
// output and undo / redo / compare. Same App class as app.cpp.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.h"
#include "hsynth/params.h"
#include "imgui.h"

using namespace hsynth;

namespace {

const ImVec4 kAccent(0.31f, 0.76f, 0.97f, 1.0f);
const ImVec4 kDim(0.55f, 0.58f, 0.64f, 1.0f);
constexpr size_t kUndoDepth = 100;

std::string noteName(int n) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

// destinations offered by the quick-assign menus (macros, motion lanes)
const ModDest kQuickDest[] = {ModDest::Cutoff, ModDest::Resonance, ModDest::Pitch, ModDest::Amp, ModDest::Pan,
                              ModDest::FmIndex, ModDest::WtPos, ModDest::Morph, ModDest::Drive, ModDest::VfxAmount,
                              ModDest::FilterEnvAmt, ModDest::AmpRelease, ModDest::Lfo1Depth, ModDest::FmModPitch};

}  // namespace

// ------------------------------------------------------------------ undo / compare

App::Snap App::captureState() const {
    Snap s;
    s.part = editPart();
    s.p.resize((size_t)P::kNumParams);
    engine_.getPartParams(s.part, s.p.data());
    return s;
}

void App::applyState(const Snap& s) {
    if (s.p.size() != (size_t)P::kNumParams) return;
    engine_.setPartParams(s.part, s.p.data());
    engine_.setGlobalParams(s.p.data());
}

void App::setP(int id, float v) {
    // one undo step per parameter gesture: consecutive changes of the same
    // parameter within a second are merged
    const double now = ImGui::GetTime();
    if (id != lastEditId_ || now - lastEditTime_ > 1.0) {
        undo_.push_back(captureState());
        if (undo_.size() > kUndoDepth) undo_.pop_front();
        redo_.clear();
    }
    lastEditId_ = id;
    lastEditTime_ = now;
    comparing_ = false;
    engine_.setPartParam(editPart(), id, v);
}

void App::undo() {
    if (undo_.empty()) return;
    redo_.push_back(captureState());
    applyState(undo_.back());
    undo_.pop_back();
    lastEditId_ = -1;
    log("undo (" + std::to_string(undo_.size()) + " left)");
}

void App::redo() {
    if (redo_.empty()) return;
    undo_.push_back(captureState());
    applyState(redo_.back());
    redo_.pop_back();
    lastEditId_ = -1;
    log("redo");
}

void App::toggleCompare() {
    if (loadedSnap_.p.empty()) return;
    if (!comparing_) {
        compareSaved_ = captureState();
        Snap s = loadedSnap_;
        s.part = editPart();
        applyState(s);
        comparing_ = true;
    } else {
        applyState(compareSaved_);
        comparing_ = false;
    }
}

// ------------------------------------------------------------------ notes / transport / MIDI out

void App::noteEvent(bool on, uint32_t id, int key, float vel) {
    engine_.sendEvent(on ? Event::noteOn(id, key, vel) : Event::noteOff(id, key, vel));
    if (s_.midiOutEcho && midiOut_.isOpen()) {
        const uint8_t ch = (uint8_t)(s_.midiOutChannel & 15);
        const uint8_t v = (uint8_t)std::max(1, std::min(127, (int)std::lround(vel * 127.0f)));
        midiOut_.send((uint8_t)((on ? 0x90 : 0x80) | ch), (uint8_t)key, on ? v : 64);
    }
}

void App::setTransport(bool on) {
    engine_.setTransport(on);
    if (midiOut_.isOpen() && s_.midiOutClock) midiOut_.sendRealtime(on ? 0xFA : 0xFC);
}

void App::drawMidiOut() {
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "MIDI output");
    ImGui::SetNextItemWidth(300 * dpiScale_);
    if (ImGui::BeginCombo("Port##midiout", s_.midiOutName.empty() ? "(none)" : s_.midiOutName.c_str())) {
        if (ImGui::Selectable("(none)", s_.midiOutName.empty())) {
            midiOut_.setClock(false, nullptr);
            midiOut_.close();
            s_.midiOutName.clear();
        }
        for (const auto& p : MidiOut::ports())
            if (ImGui::Selectable(p.c_str(), p == s_.midiOutName)) {
                std::string e;
                midiOut_.setClock(false, nullptr);
                if (midiOut_.open(p, e)) {
                    s_.midiOutName = p;
                    if (s_.midiOutClock) midiOut_.setClock(true, [this] { return engine_.getParam(P::fxChain(P::FxTempo)); });
                    log("MIDI output: " + p);
                } else {
                    log(e, 2);
                }
            }
        ImGui::EndCombo();
    }
    if (ImGui::Checkbox("Send MIDI clock (24 ppqn, Start / Stop with the transport)", &s_.midiOutClock)) {
        midiOut_.setClock(s_.midiOutClock && midiOut_.isOpen(),
                          [this] { return engine_.getParam(P::fxChain(P::FxTempo)); });
    }
    ImGui::Checkbox("Echo screen / PC keyboard notes", &s_.midiOutEcho);
    ImGui::SameLine();
    int ch = s_.midiOutChannel + 1;
    ImGui::SetNextItemWidth(80 * dpiScale_);
    if (ImGui::SliderInt("Channel##midiout", &ch, 1, 16)) s_.midiOutChannel = ch - 1;
}

// ------------------------------------------------------------------ macros

bool App::quickAssign(const char* id, ModSource src) {
    bool done = false;
    if (ImGui::BeginPopup(id)) {
        ImGui::TextColored(kAccent, "Route to (uses a free mod matrix slot)");
        const auto& names = paramTable()[(size_t)P::mod(0, P::ModDst)].options;
        for (ModDest d : kQuickDest) {
            if (!ImGui::Selectable(names[(size_t)d].c_str())) continue;
            int slot = -1;
            for (int s = P::kModSlots - 1; s >= 0; --s)
                if (getP(P::mod(s, P::ModSrc)) == 0.0f || getP(P::mod(s, P::ModAmt)) == 0.0f) { slot = s; break; }
            if (slot < 0) { log("no free mod matrix slot", 1); break; }
            setP(P::mod(slot, P::ModSrc), (float)src);
            setP(P::mod(slot, P::ModVia), 0.0f);
            setP(P::mod(slot, P::ModDst), (float)d);
            setP(P::mod(slot, P::ModAmt), d == ModDest::Pitch ? 0.04f : 0.5f);
            log("mod slot " + std::to_string(slot + 1) + ": " + paramTable()[(size_t)P::mod(0, P::ModSrc)].options[(size_t)src] +
                " -> " + names[(size_t)d]);
            done = true;
        }
        ImGui::EndPopup();
    }
    return done;
}

void App::drawMacros() {
    const float d = dpiScale_;
    ImGui::TextColored(kAccent, "Macros");
    for (int i = 0; i < 4; ++i) {
        ImGui::SameLine();
        ImGui::PushID(i);
        const int id = P::Macro1 + i;
        float v = getP(id);
        ImGui::SetNextItemWidth(110 * d);
        if (ImGui::SliderFloat(("##m" + std::to_string(i)).c_str(), &v, 0, 1, ("M" + std::to_string(i + 1) + " %.2f").c_str()))
            setP(id, v);
        paramContextMenu(paramTable()[(size_t)id], id, v);
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) ImGui::OpenPopup("assign");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("route macro %d to a destination", i + 1);
        quickAssign("assign", (ModSource)((int)ModSource::Macro1 + i));
        ImGui::PopID();
    }
}

// ------------------------------------------------------------------ sequencer

void App::drawSeq() {
    const float d = dpiScale_;
    const auto& table = paramTable();
    const int part = editPart();
    if (engine_.multiTimbral()) ImGui::TextColored(kAccent, "Part %d: %s", part + 1, partNames_[part].c_str());
    else ImGui::TextColored(kAccent, "Step sequencer");
    ImGui::SameLine();
    ImGui::TextColored(kDim, "   free: runs with the transport (Play in the top bar / MIDI clock start)   key: runs while a key is held, transposed from C4");

    // settings (generic parameter widgets: undo, MIDI learn)
    ImGui::BeginChild("seqset", ImVec2(380 * d, 250 * d));
    for (int id : {P::SeqOn, P::SeqMode, P::SeqRate, P::SeqSteps, P::SeqGate, P::SeqSwing, P::SeqTranspose})
        drawParam(table[(size_t)id], id);
    ImGui::EndChild();
    ImGui::SameLine();

    // step grid
    ImGui::BeginChild("seqgrid", ImVec2(0, 250 * d), ImGuiChildFlags_Borders);
    const int steps = std::max(1, std::min(P::kSeqSteps, (int)getP(P::SeqSteps)));
    const int play = engine_.seqPosition(part);
    const float cellW = std::max(34.0f * d, (ImGui::GetContentRegionAvail().x - 8 * d) / 16.0f - ImGui::GetStyle().ItemSpacing.x);
    for (int s = 0; s < steps; ++s) {
        if (s % 16) ImGui::SameLine();
        ImGui::PushID(s);
        ImGui::BeginGroup();
        const int vId = P::seqStep(s, P::SeqVel), nId = P::seqStep(s, P::SeqNote), lId = P::seqStep(s, P::SeqLen);
        float vel = getP(vId);
        const bool on = vel > 0.0f;
        const bool cur = s == play;
        ImVec4 col = on ? ImVec4(0.25f, 0.5f + 0.4f * vel, 0.85f, 1.0f) : ImVec4(0.18f, 0.19f, 0.22f, 1.0f);
        if (cur) col = ImVec4(0.95f, 0.75f, 0.25f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        if (ImGui::Button(std::to_string(s + 1).c_str(), ImVec2(cellW, 26 * d))) setP(vId, on ? 0.0f : 0.8f);
        ImGui::PopStyleColor();
        int note = (int)getP(nId);
        ImGui::SetNextItemWidth(cellW);
        if (ImGui::DragInt("##n", &note, 0.15f, 0, 127, noteName(note).c_str())) setP(nId, (float)note);
        ImGui::BeginDisabled(!on);
        if (ImGui::VSliderFloat("##v", ImVec2(cellW, 70 * d), &vel, 0.0f, 1.0f, "")) setP(vId, std::max(0.01f, vel));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("velocity %.2f", vel);
        float len = getP(lId);
        ImGui::SetNextItemWidth(cellW);
        if (ImGui::DragFloat("##l", &len, 0.02f, 0.1f, 8.0f, "%.1f")) setP(lId, len);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("length in steps (> 1 = tie into the next steps)");
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();

    // motion lanes
    ImGui::Spacing();
    ImGui::TextColored(kAccent, "Motion lanes");
    ImGui::SameLine();
    ImGui::TextColored(kDim, "tempo-synced step values; route with the mod matrix sources motion1..4");
    for (int ln = 0; ln < P::kMotionLanes; ++ln) {
        ImGui::SameLine();
        if (ImGui::RadioButton(("Lane " + std::to_string(ln + 1)).c_str(), seqLane_ == ln)) seqLane_ = ln;
    }
    ImGui::SameLine();
    if (ImGui::Button("Route lane...")) ImGui::OpenPopup("lane_assign");
    quickAssign("lane_assign", (ModSource)((int)ModSource::Motion1 + seqLane_));
    ImGui::BeginChild("laneset", ImVec2(380 * d, 0));
    for (int f : {P::MotionRate, P::MotionSteps, P::MotionSmooth}) {
        const int id = P::motion(seqLane_, f);
        drawParam(table[(size_t)id], id);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("lanebars", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const int lsteps = std::max(1, std::min(P::kMotionSteps, (int)getP(P::motion(seqLane_, P::MotionSteps))));
    const float barW = std::max(24.0f * d, (ImGui::GetContentRegionAvail().x - 8 * d) / 16.0f - ImGui::GetStyle().ItemSpacing.x);
    const float barH = std::max(80.0f * d, ImGui::GetContentRegionAvail().y - 8 * d);
    for (int s = 0; s < P::kMotionSteps; ++s) {
        if (s) ImGui::SameLine();
        ImGui::PushID(s);
        const int id = P::motion(seqLane_, P::MotionValue1 + s);
        float v = getP(id);
        ImGui::BeginDisabled(s >= lsteps);
        if (ImGui::VSliderFloat("##b", ImVec2(barW, barH), &v, -1.0f, 1.0f, "%.2f")) setP(id, v);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) setP(id, 0.0f);
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndChild();
}
