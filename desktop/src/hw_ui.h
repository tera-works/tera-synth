// Hardware-synth look for the desktop UI, the same as the phone apps
// (ios/HSynth/Controls.swift, android Widgets.kt): dark chassis, framed panels with
// silkscreen titles, green LCD read-outs, knobs with an LED ring, buttons with an LED.
#pragma once

#include <string>

#include "imgui.h"

namespace hw {

// palette (Hw in Controls.swift)
constexpr ImU32 kChassis = IM_COL32(14, 15, 17, 255);
constexpr ImU32 kPanel = IM_COL32(31, 33, 37, 255);
constexpr ImU32 kEdge = IM_COL32(8, 9, 10, 255);
constexpr ImU32 kSilk = IM_COL32(214, 218, 226, 255);
constexpr ImU32 kEngrave = IM_COL32(160, 166, 178, 255);
constexpr ImU32 kLed = IM_COL32(255, 140, 26, 255);
constexpr ImU32 kLedOff = IM_COL32(64, 40, 24, 255);
constexpr ImU32 kLcdBg = IM_COL32(12, 18, 8, 255);
constexpr ImU32 kLcdFg = IM_COL32(174, 234, 0, 255);
constexpr ImU32 kLcdDim = IM_COL32(96, 132, 22, 255);
constexpr ImU32 kKey = IM_COL32(44, 47, 53, 255);
inline ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

struct Fonts {
    ImFont* label = nullptr;  // Rajdhani Bold, small caps labels
    ImFont* title = nullptr;  // Rajdhani Bold, panel titles / buttons
    ImFont* logo = nullptr;   // Rajdhani Bold, large
    ImFont* lcd = nullptr;    // Share Tech Mono
    ImFont* lcdBig = nullptr;
};
Fonts& fonts();

// Fonts from <factory>/fonts (Rajdhani, Share Tech Mono; SIL OFL) plus a Japanese
// font merged into the text font. Call before the first frame.
void loadFonts(float scale, const std::string& factoryDir, const char* japaneseFont);
void applyStyle(float scale);
float scale();

// LCD read-out (width 0 = the rest of the line)
void Lcd(const char* text, float width = 0, bool dim = false, bool big = false);
// two-line LCD: a main line and a dim line below
void Lcd2(const char* main, const char* sub, float width);
// push button; lit: nullptr = no LED. Returns true when clicked.
bool Button(const char* label, const bool* lit = nullptr, float minWidth = 0, bool small = false);
// knob with an LED ring. norm 0..1 is changed by dragging (up / right = more);
// double click -> resetRequested. label / value are drawn under it. The whole knob is
// the last item (for context menus / tooltips). Returns true while the value changes.
bool Knob(const char* id, float* norm, const char* label, const char* value, bool* resetRequested = nullptr,
          bool bipolar = false, float size = 0);
// on / off switch with an LED and a label under it
bool Switch(const char* id, bool* on, const char* label);
// LCD-style combo box header with a label above; use with ImGui::EndCombo when true
// The group stays open: the caller ends it with ImGui::EndGroup() in any case, and calls
// EndLcdCombo() first when BeginLcdCombo returned true.
bool BeginLcdCombo(const char* id, const char* label, const char* preview, float width);
void EndLcdCombo();
// framed panel with a silkscreen title; content inside a group. width 0 = as wide as the content
void BeginPanel(const char* title, float width = 0);
void EndPanel();
// next panel on the same row (panels draw 10 px past their content on the right)
void SameLinePanel();
// label text in the engraved style
void Label(const char* text);

}  // namespace hw
