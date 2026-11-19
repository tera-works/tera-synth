#include "hw_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "imgui_internal.h"

namespace hw {

namespace {
Fonts g_fonts;
float g_scale = 1.0f;

struct PanelState {
    ImVec2 start;
    float width;
    ImDrawListSplitter split;
};
std::vector<PanelState> g_panels;

bool fileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return (bool)f;
}

std::string upper(const char* s) {
    std::string u(s);
    for (auto& c : u)
        if ((unsigned char)c < 0x80) c = (char)std::toupper((unsigned char)c);
    return u;
}

void withFont(ImFont* f, float size) {
    if (f) ImGui::PushFont(f);
    else ImGui::PushFont(ImGui::GetFont());
    (void)size;
}
}  // namespace

Fonts& fonts() { return g_fonts; }
float scale() { return g_scale; }

void loadFonts(float s, const std::string& factoryDir, const char* japaneseFont) {
    g_scale = s;
    ImGuiIO& io = ImGui::GetIO();
    const std::string dir = factoryDir + "/fonts/";
    const std::string semi = dir + "Rajdhani-SemiBold.ttf", bold = dir + "Rajdhani-Bold.ttf",
                      mono = dir + "ShareTechMono-Regular.ttf";
    const bool haveRaj = fileExists(semi) && fileExists(bold), haveMono = fileExists(mono);
    // text font: Rajdhani (Latin) + the Japanese font for file names and messages
    ImFont* base = nullptr;
    if (haveRaj) base = io.Fonts->AddFontFromFileTTF(semi.c_str(), 19.0f * s);
    if (!base && fileExists("C:\\Windows\\Fonts\\segoeui.ttf")) base = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * s);
    if (!base) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * s;
        base = io.Fonts->AddFontDefault(&cfg);
    }
    if (japaneseFont && fileExists(japaneseFont)) {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(japaneseFont, 17.0f * s, &cfg, io.Fonts->GetGlyphRangesJapanese());
    }
    auto add = [&](const std::string& f, float px, bool jp) -> ImFont* {
        if (!fileExists(f)) return nullptr;
        ImFont* font = io.Fonts->AddFontFromFileTTF(f.c_str(), px * s);
        if (font && jp && japaneseFont && fileExists(japaneseFont)) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(japaneseFont, px * 0.9f * s, &cfg, io.Fonts->GetGlyphRangesJapanese());
        }
        return font;
    };
    if (haveRaj) {
        g_fonts.label = add(bold, 14.0f, false);
        g_fonts.title = add(bold, 17.0f, true);
        g_fonts.logo = add(bold, 28.0f, false);
    }
    if (haveMono) {
        g_fonts.lcd = add(mono, 17.0f, true);
        g_fonts.lcdBig = add(mono, 24.0f, true);
    }
}

void applyStyle(float s) {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0;
    st.ChildRounding = 2;
    st.FrameRounding = 2;
    st.GrabRounding = 2;
    st.PopupRounding = 2;
    st.TabRounding = 2;
    st.ScrollbarRounding = 2;
    st.WindowPadding = ImVec2(10, 8);
    st.FramePadding = ImVec2(8, 5);
    st.ItemSpacing = ImVec2(8, 6);
    st.FrameBorderSize = 1;
    st.ChildBorderSize = 1;
    st.PopupBorderSize = 1;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = v4(kSilk);
    c[ImGuiCol_TextDisabled] = v4(kEngrave);
    c[ImGuiCol_WindowBg] = v4(kChassis);
    c[ImGuiCol_ChildBg] = v4(kChassis);
    c[ImGuiCol_PopupBg] = v4(IM_COL32(24, 26, 29, 250));
    c[ImGuiCol_Border] = v4(kEdge);
    c[ImGuiCol_FrameBg] = v4(kLcdBg);
    c[ImGuiCol_FrameBgHovered] = v4(IM_COL32(22, 32, 14, 255));
    c[ImGuiCol_FrameBgActive] = v4(IM_COL32(28, 40, 16, 255));
    c[ImGuiCol_TitleBg] = v4(kPanel);
    c[ImGuiCol_TitleBgActive] = v4(kPanel);
    c[ImGuiCol_ScrollbarBg] = v4(kChassis);
    c[ImGuiCol_ScrollbarGrab] = v4(kKey);
    c[ImGuiCol_ScrollbarGrabHovered] = v4(IM_COL32(70, 74, 82, 255));
    c[ImGuiCol_ScrollbarGrabActive] = v4(kLedOff);
    c[ImGuiCol_CheckMark] = v4(kLed);
    c[ImGuiCol_SliderGrab] = v4(kLed);
    c[ImGuiCol_SliderGrabActive] = v4(IM_COL32(255, 170, 80, 255));
    c[ImGuiCol_Button] = v4(kKey);
    c[ImGuiCol_ButtonHovered] = v4(IM_COL32(62, 66, 74, 255));
    c[ImGuiCol_ButtonActive] = v4(IM_COL32(84, 60, 36, 255));
    c[ImGuiCol_Header] = v4(IM_COL32(70, 48, 26, 255));
    c[ImGuiCol_HeaderHovered] = v4(IM_COL32(90, 60, 30, 255));
    c[ImGuiCol_HeaderActive] = v4(IM_COL32(110, 72, 34, 255));
    c[ImGuiCol_Separator] = v4(IM_COL32(60, 63, 70, 255));
    c[ImGuiCol_Tab] = v4(kKey);
    c[ImGuiCol_TabHovered] = v4(IM_COL32(62, 66, 74, 255));
    c[ImGuiCol_TabSelected] = v4(IM_COL32(84, 60, 36, 255));
    c[ImGuiCol_TableHeaderBg] = v4(kPanel);
    c[ImGuiCol_TableRowBg] = v4(IM_COL32(20, 21, 24, 255));
    c[ImGuiCol_TableRowBgAlt] = v4(IM_COL32(25, 27, 30, 255));
    c[ImGuiCol_TableBorderLight] = v4(IM_COL32(40, 42, 46, 255));
    c[ImGuiCol_TableBorderStrong] = v4(kEdge);
    c[ImGuiCol_PlotHistogram] = v4(kLcdFg);
    c[ImGuiCol_PlotLines] = v4(kLcdFg);
    c[ImGuiCol_TextSelectedBg] = v4(IM_COL32(110, 72, 34, 200));
    c[ImGuiCol_NavHighlight] = v4(kLed);
    st.ScaleAllSizes(s);
}

void Label(const char* text) {
    if (g_fonts.label) ImGui::PushFont(g_fonts.label);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kEngrave));
    ImGui::TextUnformatted(upper(text).c_str());
    ImGui::PopStyleColor();
    if (g_fonts.label) ImGui::PopFont();
}

void Lcd(const char* text, float width, bool dim, bool big) {
    ImFont* f = big ? g_fonts.lcdBig : g_fonts.lcd;
    if (f) ImGui::PushFont(f);
    const ImVec2 pad(8 * g_scale, 4 * g_scale);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float w = width > 0 ? width : std::max(ts.x + pad.x * 2, ImGui::GetContentRegionAvail().x);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz(w, ts.y + pad.y * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), kLcdBg, 2 * g_scale);
    dl->AddRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), kEdge, 2 * g_scale);
    dl->PushClipRect(p0, ImVec2(p0.x + sz.x - pad.x * 0.5f, p0.y + sz.y), true);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), dim ? kLcdDim : kLcdFg, text);
    dl->PopClipRect();
    ImGui::Dummy(sz);
    if (f) ImGui::PopFont();
}

void Lcd2(const char* main, const char* sub, float width) {
    const ImVec2 pad(8 * g_scale, 3 * g_scale);
    ImFont* big = g_fonts.lcdBig ? g_fonts.lcdBig : ImGui::GetFont();
    ImFont* small = g_fonts.lcd ? g_fonts.lcd : ImGui::GetFont();
    const float h1 = big->FontSize, h2 = small->FontSize * 0.85f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz(width, h1 + h2 + pad.y * 3);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), kLcdBg, 2 * g_scale);
    dl->AddRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), kEdge, 2 * g_scale);
    dl->PushClipRect(p0, ImVec2(p0.x + sz.x - pad.x * 0.5f, p0.y + sz.y), true);
    dl->AddText(big, h1, ImVec2(p0.x + pad.x, p0.y + pad.y), kLcdFg, main);
    dl->AddText(small, h2, ImVec2(p0.x + pad.x, p0.y + pad.y * 2 + h1), kLcdDim, sub);
    dl->PopClipRect();
    ImGui::Dummy(sz);
}

bool Button(const char* label, const bool* lit, float minWidth, bool small) {
    ImFont* f = g_fonts.title;
    if (f) ImGui::PushFont(f);
    const float s = g_scale;
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float led = lit ? 13 * s : 0;
    const ImVec2 pad(small ? 9 * s : 12 * s, small ? 4 * s : 7 * s);
    const ImVec2 sz(std::max(minWidth, ts.x + led + pad.x * 2), ts.y + pad.y * 2);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, sz);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    const ImU32 top = act ? IM_COL32(40, 42, 47, 255) : hov ? IM_COL32(82, 86, 95, 255) : IM_COL32(69, 72, 79, 255);
    const ImU32 bot = act ? IM_COL32(56, 59, 66, 255) : kKey;
    dl->AddRectFilledMultiColor(p0, p1, top, top, bot, bot);
    dl->AddRect(p0, p1, kEdge, 3 * s);
    float x = p0.x + pad.x;
    if (lit) {
        const ImVec2 c(x + 3.5f * s, (p0.y + p1.y) * 0.5f);
        if (*lit) dl->AddCircleFilled(c, 7 * s, IM_COL32(255, 140, 26, 60));
        dl->AddCircleFilled(c, 3.5f * s, *lit ? kLed : kLedOff);
        x += led;
    }
    dl->AddText(ImVec2(x, p0.y + pad.y), disabled ? kEngrave : IM_COL32(255, 255, 255, 255), label, end);
    if (f) ImGui::PopFont();
    return clicked;
}

bool Knob(const char* id, float* norm, const char* label, const char* value, bool* resetRequested, bool bipolar, float size) {
    const float s = g_scale;
    const float d = size > 0 ? size : 46 * s;  // ring diameter
    ImFont* lf = g_fonts.label ? g_fonts.label : ImGui::GetFont();
    ImFont* vf = g_fonts.lcd ? g_fonts.lcd : ImGui::GetFont();
    const float lh = lf->FontSize, vh = vf->FontSize * 0.82f;
    const float w = std::max(d + 18 * s, 74 * s);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz(w, d + 10 * s + lh + vh + 4 * s);
    ImGui::InvisibleButton(id, sz);
    bool changed = false;
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && resetRequested) *resetRequested = true;
    if (active) {
        const ImVec2 md = ImGui::GetIO().MouseDelta;
        const float fine = ImGui::GetIO().KeyShift ? 0.2f : 1.0f;
        const float nv = std::clamp(*norm + (-md.y + md.x * 0.5f) / (200.0f * s) * fine, 0.0f, 1.0f);
        if (nv != *norm) { *norm = nv; changed = true; }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemHovered() && !active) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f && ImGui::GetIO().KeyCtrl) {  // Ctrl + wheel: fine steps (plain wheel scrolls the page)
            *norm = std::clamp(*norm + wheel * 0.01f, 0.0f, 1.0f);
            changed = true;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + w * 0.5f, p0.y + 5 * s + d * 0.5f);
    const float r = d * 0.5f;
    // LED ring: 15 dots over 270 degrees (from the centre when bipolar)
    for (int i = 0; i < 15; ++i) {
        const float t = (float)i / 14.0f;
        const float a = (135.0f + 270.0f * t) * 3.14159265f / 180.0f;
        bool on;
        if (bipolar) on = (t >= 0.5f && t <= *norm + 0.001f) || (t <= 0.5f && t >= *norm - 0.001f);
        else on = *norm > 0.0005f && t <= *norm + 0.001f;
        const ImVec2 q(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
        if (on) dl->AddCircleFilled(q, 3.6f * s, IM_COL32(255, 140, 26, 70));
        dl->AddCircleFilled(q, 1.9f * s, on ? kLed : kLedOff);
    }
    // cap
    const float cr = r - 7 * s;
    const int segs = 40;
    for (int k = 0; k < 6; ++k) {  // a soft top-lit gradient
        const float f = (float)k / 5.0f;
        const int g = (int)(77 - 44 * f);
        dl->AddCircleFilled(ImVec2(c.x, c.y - cr * 0.12f * (1 - f)), cr * (1.0f - 0.08f * f), IM_COL32(g, g, g + 3, 255), segs);
    }
    dl->AddCircle(c, cr, IM_COL32(0, 0, 0, 255), segs, 1.2f * s);
    if (active || ImGui::IsItemHovered()) dl->AddCircle(c, cr + 2 * s, IM_COL32(255, 140, 26, 90), segs, 1.0f * s);
    const float a = (-135.0f + 270.0f * *norm - 90.0f) * 3.14159265f / 180.0f;
    dl->AddLine(ImVec2(c.x + std::cos(a) * cr * 0.25f, c.y + std::sin(a) * cr * 0.25f),
                ImVec2(c.x + std::cos(a) * (cr - 3 * s), c.y + std::sin(a) * (cr - 3 * s)), IM_COL32(255, 255, 255, 255), 2.4f * s);
    // label + value
    const std::string L = upper(label);
    auto centred = [&](ImFont* f, float fs, float y, ImU32 col, const char* t) {
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, t);
        dl->PushClipRect(ImVec2(p0.x, p0.y), ImVec2(p0.x + w, p0.y + sz.y), true);
        dl->AddText(f, fs, ImVec2(p0.x + std::max(0.0f, (w - ts.x) * 0.5f), y), col, t);
        dl->PopClipRect();
    };
    centred(lf, lh, p0.y + d + 8 * s, kEngrave, L.c_str());
    centred(vf, vh, p0.y + d + 8 * s + lh + 1 * s, kLcdFg, value);
    return changed;
}

bool Switch(const char* id, bool* on, const char* label) {
    const float s = g_scale;
    ImFont* lf = g_fonts.label ? g_fonts.label : ImGui::GetFont();
    const std::string L = upper(label);
    const ImVec2 ts = lf->CalcTextSizeA(lf->FontSize, FLT_MAX, 0.0f, L.c_str());
    const float w = std::max(74 * s, ts.x + 8 * s);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz(w, 46 * s + 10 * s + lf->FontSize);
    const bool clicked = ImGui::InvisibleButton(id, sz);
    if (clicked) *on = !*on;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 led(p0.x + w * 0.5f, p0.y + 10 * s);
    if (*on) dl->AddCircleFilled(led, 8 * s, IM_COL32(255, 140, 26, 60));
    dl->AddCircleFilled(led, 4 * s, *on ? kLed : kLedOff);
    // slide switch
    const ImVec2 b0(p0.x + w * 0.5f - 18 * s, p0.y + 22 * s), b1(p0.x + w * 0.5f + 18 * s, p0.y + 40 * s);
    dl->AddRectFilled(b0, b1, IM_COL32(10, 11, 12, 255), 3 * s);
    dl->AddRect(b0, b1, kEdge, 3 * s);
    const float kx = *on ? b1.x - 17 * s : b0.x + 1 * s;
    dl->AddRectFilledMultiColor(ImVec2(kx, b0.y + 1 * s), ImVec2(kx + 16 * s, b1.y - 1 * s), IM_COL32(95, 99, 108, 255),
                                IM_COL32(95, 99, 108, 255), kKey, kKey);
    dl->AddText(lf, lf->FontSize, ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + 46 * s + 6 * s), kEngrave, L.c_str());
    return clicked;
}

bool BeginLcdCombo(const char* id, const char* label, const char* preview, float width) {
    ImGui::BeginGroup();
    if (label && *label) Label(label);
    if (g_fonts.lcd) ImGui::PushFont(g_fonts.lcd);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kLcdFg));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(kLcdBg));
    ImGui::PushStyleColor(ImGuiCol_Button, v4(kLcdBg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(IM_COL32(28, 40, 16, 255)));
    ImGui::SetNextItemWidth(width);
    const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge);
    ImGui::PopStyleColor(4);
    if (g_fonts.lcd) ImGui::PopFont();
    return open;
}

void EndLcdCombo() { ImGui::EndCombo(); }

void BeginPanel(const char* title, float width) {
    const float s = g_scale;
    PanelState ps;
    ps.start = ImGui::GetCursorScreenPos();
    ps.width = width;
    g_panels.push_back(ps);
    PanelState& p = g_panels.back();
    p.split.Split(ImGui::GetWindowDrawList(), 2);
    p.split.SetCurrentChannel(ImGui::GetWindowDrawList(), 1);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 2 * s));
    ImGui::Indent(10 * s);
    // title: silkscreen text and a rule to the right
    ImFont* f = g_fonts.title;
    if (f) ImGui::PushFont(f);
    const std::string T = upper(title);
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Text, v4(kSilk));
    ImGui::TextUnformatted(T.c_str());
    ImGui::PopStyleColor();
    const ImVec2 ts = ImGui::CalcTextSize(T.c_str());
    if (f) ImGui::PopFont();
    p.split.SetCurrentChannel(ImGui::GetWindowDrawList(), 0);
    const float ruleEnd = width > 0 ? p.start.x + width - 10 * s : tp.x + ts.x + 8 * s;  // finished in EndPanel
    (void)ruleEnd;
    p.split.SetCurrentChannel(ImGui::GetWindowDrawList(), 1);
    ImGui::PushID(title);
    if (width > 0) ImGui::PushItemWidth(width - 20 * s);
    // remember the title for the rule
    ImGui::GetStateStorage()->SetFloat(ImGui::GetID("##title_x"), tp.x + ts.x + 8 * s);
    ImGui::GetStateStorage()->SetFloat(ImGui::GetID("##title_y"), tp.y + ts.y * 0.55f);
}

void EndPanel() {
    const float s = g_scale;
    PanelState& p = g_panels.back();
    const float rx = ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##title_x"), 0);
    const float ry = ImGui::GetStateStorage()->GetFloat(ImGui::GetID("##title_y"), 0);
    if (p.width > 0) {
        ImGui::Dummy(ImVec2(p.width - 20 * s, 0));  // the panel is at least this wide
        ImGui::PopItemWidth();
    }
    ImGui::PopID();
    ImGui::Unindent(10 * s);
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::EndGroup();
    const ImVec2 a = p.start;
    const ImVec2 b(ImGui::GetItemRectMax().x + 10 * s, ImGui::GetItemRectMax().y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    p.split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(a, b, kPanel, 2 * s);
    dl->AddRect(a, b, kEdge, 2 * s);
    if (rx > 0 && rx < b.x - 10 * s) dl->AddLine(ImVec2(rx, ry), ImVec2(b.x - 10 * s, ry), IM_COL32(89, 89, 89, 255), 1.0f);
    p.split.Merge(dl);
    g_panels.pop_back();
}

void SameLinePanel() { ImGui::SameLine(0, 18 * g_scale); }

}  // namespace hw
